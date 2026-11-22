// HostManager members for the shared display: policies, resize and restore.
// Moved unchanged from hostmanager.cpp.
#include "diagnostics.h"
#include <QSysInfo>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#ifdef Q_OS_LINUX
#include <cerrno>
#include <csignal>
#include <sys/prctl.h>
#include <unistd.h>
#endif
#include "hostmanager.h"
#include "hostcaretmonitor.h"
#include <QScreen>
#ifdef Q_OS_MACOS
#include <CoreGraphics/CoreGraphics.h>
#endif
#include "workspaceresolution.h"
#include "peerstore.h"
#include "serviceconfig.h"
#include <QElapsedTimer>
#include <algorithm>
#include <limits>
#include <QSslError>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QNetworkProxy>
#include <QApplication>
#include <QPalette>
#include <QWindow>
#include <QMenu>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkReply>
#include <QDesktopServices>
#include <QUuid>
#include <QTimer>
#include <QThread>
#include <QMessageBox>
#include <QPushButton>
#include <QDateTime>
#include "../../host/macos/recovery-policy.h"
#include <QSettings>
#include <QPointer>
#include <QActionGroup>
#include <QRegularExpression>
#include <QHostInfo>
#ifdef Q_OS_MACOS
#include <CoreGraphics/CoreGraphics.h>
#include <ApplicationServices/ApplicationServices.h>
#include "macpermissions.h"
#include "macdock.h"
#include "macunattended.h"
#endif

bool HostManager::saveLinuxDisplayState() {
#ifdef Q_OS_LINUX
    if (!m_LinuxOutputName.isEmpty()) {
        return PeerStore::write(m_Directory + "/virtual-display.json", {
            {"node", double(m_LinuxPipewireNode)}, {"serial", m_LinuxPipewireSerial}, {"output", m_LinuxOutputName},
            {"width", m_DisplayWidth}, {"height", m_DisplayHeight}, {"scale", m_DisplayScale}, {"leaseToken", m_LinuxX11Lease}});
    }
#endif
    return true;
}

bool HostManager::physicalDisplaySharing() const {
    if (m_CaptureSource != QStringLiteral("virtual")) return true;
#ifdef Q_OS_WIN
    QSettings owned(QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\DeskPort"), QSettings::NativeFormat);
    const auto instance=owned.value("VirtualDisplayDevice").toString();
    return !m_WindowsVirtualDisplay && !instance.startsWith("ROOT\\DESKPORTVIRTUALDISPLAY\\", Qt::CaseInsensitive);
#else
    return false;
#endif
}

QVariantList HostManager::captureSources() const {
    QVariantList sources{QVariantMap{{"id", "virtual"}, {"label", tr("Virtual extended display")}}};
#ifdef Q_OS_MACOS
    CGDirectDisplayID displays[64];
    uint32_t count = 0;
    if (CGGetActiveDisplayList(64, displays, &count) == kCGErrorSuccess) {
        for (uint32_t i = 0; i < count; ++i) {
            CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(displays[i]);
            if (!uuid) continue;
            CFStringRef value = CFUUIDCreateString(kCFAllocatorDefault, uuid);
            CFRelease(uuid);
            char buffer[128] = {};
            const bool valid = value && CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8);
            if (value) CFRelease(value);
            if (!valid) continue;
            const auto bounds = CGDisplayBounds(displays[i]);
            const auto label = tr("Display %1 · %2 × %3%4").arg(i + 1)
                .arg(int(bounds.size.width)).arg(int(bounds.size.height))
                .arg(displays[i] == CGMainDisplayID() ? tr(" · Main") : QString());
            sources.append(QVariantMap{{"id", QStringLiteral("mac:") + QString::fromUtf8(buffer)}, {"label", label}});
        }
    }
#elif defined(Q_OS_WIN)
    // Qt may expose a friendly monitor name, while Sunshine's Windows capture
    // adapter consumes a GDI device name. Enumerate the same native identity.
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &device, 0); ++index) {
        if (!(device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ||
            (device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)) continue;
        const auto name = QString::fromWCharArray(device.DeviceName);
        const auto label = QString::fromWCharArray(device.DeviceString) + QStringLiteral(" (") + name + QLatin1Char(')') +
            (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE ? tr(" · Main") : tr(" · Extended"));
        sources.append(QVariantMap{{"id", QStringLiteral("win:") + name}, {"label", label}});
    }
#endif
    return sources;
}

bool HostManager::setCaptureSource(const QString& source) {
    // Never switch a live stream or silently fall back to a different screen.
    if (running() || changing()) return false;
    bool found = false;
    for (const auto& item : captureSources())
        if (item.toMap().value("id").toString() == source) found = true;
    if (!found) return false;
    m_CaptureSource = source;
    if (!m_Isolated) QSettings().setValue("host/captureSource", source);
    emit changed();
    return true;
}

bool HostManager::adaptiveDisplayAvailable() const {
#if defined(Q_OS_MACOS) || defined(Q_OS_LINUX) || defined(Q_OS_WIN)
    return running() && !changing() && m_Display.state() == QProcess::Running;
#else
    return false;
#endif
}

bool HostManager::displayPoliciesAvailable() const {
#ifdef Q_OS_WIN
    return adaptiveDisplayAvailable() && m_WindowsVirtualDisplay;
#elif defined(Q_OS_LINUX)
    return adaptiveDisplayAvailable() && (!m_LinuxGnome || m_LinuxGnomePolicies) && !m_LinuxX11 && (!m_LinuxHyprland || m_LinuxHyprlandPolicies);
#else
    return adaptiveDisplayAvailable();
#endif
}

// Windows rebuilds the whole display topology through an elevated guardian
// that allows itself 20 seconds; three outputs can take longer than five.
#ifdef Q_OS_WIN
static constexpr int DisplayChangeTimeoutMs = 20000;
#else
static constexpr int DisplayChangeTimeoutMs = 5000;
#endif

QString HostManager::displayResizeRejection(int width, int height, int scale, int sequence, int policy) const {
#ifdef Q_OS_LINUX
    if (m_LinuxX11 && policy != 0) return QStringLiteral("policy-unsupported");
#endif
    if (policy < 0 || policy > 2) return QStringLiteral("policy-unsupported");
    if (!adaptiveDisplayAvailable()) return QStringLiteral("unavailable");
    if (sequence == 0 || width < 640 || width > DeskPortDisplay::MaxWidth ||
        height < 360 || height > DeskPortDisplay::MaxHeight || width % 4 || height % 4 ||
        (scale != 1 && scale != 2)) return QStringLiteral("topology-invalid");
    if (!m_QueuedDisplayRequest.isEmpty() || (m_DisplaySequence && (m_DisplaySequence > 0 || sequence < 0)))
        return QStringLiteral("busy");
    return {};
}

bool HostManager::resizeDisplay(int width, int height, int scale, int sequence, int policy) {
    if (!displayResizeRejection(width, height, scale, sequence, policy).isEmpty()) return false;
    if (m_DisplaySequence) {
        // Finish the previous session's restoration before a new client takes over.
        m_QueuedDisplayRequest = {{"width", width}, {"height", height}, {"scale", scale},
                                  {"seq", sequence}, {"policy", policy}};
        return true;
    }
    if(m_CaretMonitor) m_CaretMonitor->stop();
    m_DisplaySequence = sequence;
    m_DisplayWireSequence = m_DisplayWireSequence == std::numeric_limits<int>::max() ? 1 : m_DisplayWireSequence + 1;
    const auto generation = ++m_DisplayGeneration;
    // An unadvertised Hyprland policy contract retains its legacy extension
    // behavior. Older helpers still receive their legacy ID 0. A negotiated
    // request is passed through without substitution.
#ifdef Q_OS_LINUX
    if (m_LinuxHyprlandPolicyRequests && !m_LinuxHyprlandPolicies && policy == 0) policy = 2;
#endif
    QJsonObject request{{"seq", m_DisplayWireSequence}, {"width", width}, {"height", height}, {"scale", scale}, {"session", sequence > 0}};
#ifdef Q_OS_LINUX
    if (!m_LinuxX11)
#endif
        request["displayPolicy"] = policy;
    m_Display.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    // Mutter's independent guardian verifies output removal and may replay a
    // saved layout before admitting a new stream. Do not time out a valid
    // recovery at the ordinary five-second resize deadline.
    const int timeoutMs = m_LinuxGnome ? 20000 : DisplayChangeTimeoutMs;
    QTimer::singleShot(timeoutMs, this, [this, generation] {
        if (m_DisplaySequence && generation == m_DisplayGeneration) {
            const auto sequence = m_DisplaySequence; m_DisplaySequence = 0;
            emit displayResized(sequence, m_DisplayWidth, m_DisplayHeight, tr("Virtual display resize timed out"));
        }
    });
    return true;
}

void HostManager::settleSessionDisplay(QObject* context, std::function<void(bool)> completion) {
    auto pending = new QObject(context);
    auto finished = std::make_shared<bool>(false);
    auto finish = [pending, finished, completion](bool ok) {
        if (*finished) return;
        *finished = true; completion(ok); pending->deleteLater();
    };
    connect(this, &HostManager::displayResized, pending, [finish](int sequence, int, int, const QString& error) {
        if (sequence < 0) finish(error.isEmpty());
    });
    const int timeoutMs = m_LinuxGnome ? 20000 : DisplayChangeTimeoutMs;
    QTimer::singleShot(timeoutMs + 5000, pending, [finish] { finish(false); });
    restoreDisplay();
}

void HostManager::restoreDisplay() {
    m_QueuedDisplayRequest = {}; // A disconnected queued controller must never take over later.
    // Video renegotiation retains the controller. Restore promptly on actual
    // disconnect, while allowing an already pending helper request to settle.
    const auto generation = m_DisplayGeneration;
    QTimer::singleShot(250, this, [this, generation] {
        if (generation != m_DisplayGeneration) return;
        if (m_DisplaySequence) { restoreDisplay(); return; }
        resizeDisplay(sharingWidth(), sharingHeight(), 1, -1);
    });
}
