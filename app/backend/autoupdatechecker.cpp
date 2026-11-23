#include "autoupdatechecker.h"
#include "version.h"
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QVersionNumber>
#include <QTimer>

AutoUpdateChecker::AutoUpdateChecker(QObject *parent) : QObject(parent), m_Nam(this)
{
    m_Nam.setStrictTransportSecurityEnabled(true);
    m_Nam.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

QString AutoUpdateChecker::evaluate(const QByteArray& json, const QString& current,
                                    QString& version, QString& notes, QString& date, QString& url)
{
    version.clear(); notes.clear(); date.clear(); url.clear();
    const auto document = QJsonDocument::fromJson(json);
    if (!document.isArray()) return QStringLiteral("error");
    // This fork deliberately follows numbered iMac builds, including previews.
    // /releases/latest excludes previews and upstream numeric versions lose the
    // compatibility revision when parsed with QVersionNumber alone.
    const QRegularExpression channel(QStringLiteral(
        "^v?([0-9]+\\.[0-9]+\\.[0-9]+)-imac\\.([1-9][0-9]*)(?:-[a-z0-9]+(?:[.-][a-z0-9]+)*)?$"));
    auto revision = [&channel](const QString& value) {
        const auto match = channel.match(value);
        if (!match.hasMatch()) return QVersionNumber();
        const auto parsed = QVersionNumber::fromString(match.captured(1) + '.' + match.captured(2));
        return parsed.segmentCount() == 4 ? parsed : QVersionNumber();
    };
    auto newest = revision(current);
    if (newest.isNull()) return QStringLiteral("error");
    for (const auto& value : document.array()) {
        if (!value.isObject()) continue;
        const auto release = value.toObject();
        if (!release.value("draft").isBool() || release.value("draft").toBool() ||
            !release.value("prerelease").isBool()) continue;
        const auto tag = release.value("tag_name").toString();
        const auto candidate = revision(tag);
        const auto link = release.value("html_url").toString();
        if (candidate.isNull() || QVersionNumber::compare(candidate, newest) <= 0 ||
            link != QStringLiteral("https://github.com/snchngny/deskport/releases/tag/") + tag)
            continue;
        // A client-only checkpoint or incomplete upload is not an update.
        const auto match = channel.match(tag);
        const auto prefix = QStringLiteral("DeskPort-") + match.captured(1) +
                            QStringLiteral("-imac.") + match.captured(2);
        bool windows = false, intel = false, arm = false;
        for (const auto& entry : release.value("assets").toArray()) {
            const auto asset = entry.toObject();
            const auto name = asset.value("name").toString();
            if (asset.value("state").toString() != QStringLiteral("uploaded") ||
                asset.value("size").toDouble() <= 0 ||
                asset.value("browser_download_url").toString() !=
                    QStringLiteral("https://github.com/snchngny/deskport/releases/download/") + tag + '/' + name)
                continue;
            windows |= name == prefix + QStringLiteral("-windows-x64-portable-full.zip");
            intel |= name == prefix + QStringLiteral("-macos-x86_64-intel-bigsur.zip");
            arm |= name == prefix + QStringLiteral("-macos-arm64.zip");
        }
        if (!windows || !intel || !arm) continue;
        newest = candidate;
        version = tag.startsWith('v') ? tag.mid(1) : tag;
        notes = release.value("body").toString();
        date = release.value("published_at").toString();
        url = link;
    }
    return url.isEmpty() ? QStringLiteral("current") : QStringLiteral("available");
}

void AutoUpdateChecker::start()
{
    if (m_Status == QStringLiteral("checking")) return;
    m_Status = QStringLiteral("checking");
    emit changed();
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/snchngny/deskport/releases?per_page=100")));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "DeskPort/" VERSION_STR);
    auto *reply = m_Nam.get(request);
    auto *timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(15000);
    // Bound memory use even if a server returns an unexpectedly large response.
    connect(reply, &QNetworkReply::readyRead, reply, [reply]() {
        if (reply->bytesAvailable() > 1024 * 1024) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_Status = QStringLiteral("error");
        m_Version.clear(); m_Notes.clear(); m_Date.clear(); m_Url.clear();
        if (reply->error() == QNetworkReply::NoError &&
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200)
            m_Status = evaluate(reply->readAll(), QStringLiteral(VERSION_STR),
                                m_Version, m_Notes, m_Date, m_Url);
        reply->deleteLater();
        emit changed();
    });
}
