#include "forkupdater.h"
#include "autoupdatechecker.h"
#include "version.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QVersionNumber>
#include <stdexcept>
#include <memory>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <signal.h>
#include <errno.h>
#endif

namespace {
const QString BundleId = QStringLiteral("io.github.keithxc.DeskPort");
const qint64 MaxPackage = 512LL * 1024 * 1024;
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
QByteArray command(const QString& program, const QStringList& args, int timeout = 120000,
                   const QProcessEnvironment& environment = QProcessEnvironment::systemEnvironment()) {
    QProcess p; p.setProcessEnvironment(environment); p.start(program, args);
    require(p.waitForStarted(10000), "Could not start the system update tool.");
    if (!p.waitForFinished(timeout)) { p.kill(); p.waitForFinished(); throw std::runtime_error("The update tool timed out."); }
    require(p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0, "The system update tool failed. The current app was retained.");
    return p.readAllStandardOutput();
}
QByteArray hashFile(const QString& path) {
    QFile f(path); require(f.open(QIODevice::ReadOnly), "Cannot read an update file.");
    QCryptographicHash h(QCryptographicHash::Sha256);
    require(h.addData(&f), "Cannot hash an update file."); return h.result().toHex();
}
QString platform() {
#ifdef Q_OS_WIN
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
#ifdef Q_PROCESSOR_X86_64
    return QStringLiteral("intel");
#else
    return QStringLiteral("arm");
#endif
#else
    return {};
#endif
}
QString appRoot() {
#ifdef Q_OS_MACOS
    return QDir(QCoreApplication::applicationDirPath() + "/../..").canonicalPath();
#else
    return QCoreApplication::applicationDirPath();
#endif
}
QString executable(const QString& root) {
#ifdef Q_OS_MACOS
    return root + "/Contents/MacOS/DeskPort";
#else
    return root + "/DeskPort.exe";
#endif
}
void copyTree(const QString& source, const QString& target) {
    require(!QFileInfo(source).isSymLink() && !QFileInfo::exists(target), "Unsafe update copy destination.");
    require(QDir().mkpath(target), "Cannot create the update directory.");
    for (const auto& item : QDir(source).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System)) {
        require(!item.isSymLink(), "Symbolic links are not allowed in Windows updates.");
        const auto dest = target + '/' + item.fileName();
        if (item.isDir()) copyTree(item.absoluteFilePath(), dest);
        else {
            require(QFile::copy(item.absoluteFilePath(), dest), "Cannot copy an update file.");
            require(hashFile(item.absoluteFilePath()) == hashFile(dest), "An update or settings copy failed verification.");
        }
    }
}
bool processAlive(qint64 pid) {
#ifdef Q_OS_WIN
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!p) return GetLastError() != ERROR_INVALID_PARAMETER;
    const bool alive = WaitForSingleObject(p, 0) == WAIT_TIMEOUT; CloseHandle(p); return alive;
#else
    return kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}
void launch(const QString& root) {
#ifdef Q_OS_MACOS
    // LaunchServices preserves the app's privacy responsibility. Direct exec
    // from Terminal can instead inherit Terminal's denied privacy permissions.
    // The staged installer shares our bundle ID; force the installed app to
    // launch even while that background helper is still returning from main.
    QProcess::startDetached("/usr/bin/open", {"-n", "-a", root});
#else
    QProcess::startDetached(executable(root), {}, root);
#endif
}
QString signingRequirement(const QString& app) {
#ifdef Q_OS_MACOS
    QProcess p; p.setProcessChannelMode(QProcess::MergedChannels);
    p.start("/usr/bin/codesign", {"-d", "-r-", app});
    require(p.waitForFinished(15000) && p.exitCode() == 0, "Cannot inspect the current app signature.");
    const auto value = QString::fromUtf8(p.readAll());
    const QRegularExpression leaf("certificate leaf = H\"([a-fA-F0-9]{40})\"");
    const auto match = leaf.match(value);
    require(value.contains("identifier \"" + BundleId + '"') && match.hasMatch() && !value.contains("cdhash"),
            "Set up a persistent local signing identity before using in-app updates.");
    return match.captured(1);
#else
    Q_UNUSED(app); return {};
#endif
}
void verifyMac(const QString& app, const QString& identity = {}) {
#ifdef Q_OS_MACOS
    require(QString::fromUtf8(command("/usr/libexec/PlistBuddy", {"-c", "Print :CFBundleIdentifier", app + "/Contents/Info.plist"})).trimmed() == BundleId,
            "The update has a different application identifier.");
    command("/usr/bin/codesign", {"--verify", "--deep", "--strict", app});
    if (!identity.isEmpty()) command("/usr/bin/codesign", {"--verify", "--strict", "-R",
        "=identifier \"" + BundleId + "\" and certificate leaf = H\"" + identity + "\"", app});
#else
    Q_UNUSED(app); Q_UNUSED(identity);
#endif
}
}

ForkUpdater::ForkUpdater(QObject* parent) : QObject(parent), m_Network(this) {
    m_Network.setStrictTransportSecurityEnabled(true);
    m_Network.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}
bool ForkUpdater::supported() const {
    if (platform().isEmpty()) return false;
    const auto root = appRoot();
    return QFileInfo(executable(root)).isFile() && !root.startsWith("/nix/") && QFileInfo(QFileInfo(root).absolutePath()).isWritable();
}
QJsonObject ForkUpdater::selectAsset(const QByteArray& json, const QString& current, const QString& target) {
    QString v,n,d,url;
    // Also allow an explicit reinstall of the latest compatible build. Never
    // downgrade to an older compatibility revision.
    AutoUpdateChecker::evaluate(json, "0.0.0-imac.1", v,n,d,url);
    const QRegularExpression channel("^v?([0-9]+\\.[0-9]+\\.[0-9]+)-imac\\.([1-9][0-9]*)(?:-[a-z0-9]+(?:[.-][a-z0-9]+)*)?$");
    const auto parsed = [&](QString x) { auto m=channel.match(x); return m.hasMatch() ? QVersionNumber::fromString(m.captured(1)+'.'+m.captured(2)) : QVersionNumber(); };
    if (url.isEmpty() || parsed(current).isNull() || QVersionNumber::compare(parsed(v), parsed(current)) < 0) return {};
    const QString suffix = target == "windows" ? "-windows-x64-portable-full.zip" : target == "intel" ? "-macos-x86_64-intel-bigsur.dmg" : target == "arm" ? "-macos-arm64.dmg" : QString();
    if (suffix.isEmpty()) return {};
    auto m=channel.match(v); const QString name="DeskPort-"+m.captured(1)+"-imac."+m.captured(2)+suffix;
    for (auto value : QJsonDocument::fromJson(json).array()) {
        auto release=value.toObject(); if (release["html_url"].toString()!=url) continue;
        for (auto a : release["assets"].toArray()) {
            auto asset=a.toObject();
            if (asset["name"].toString()==name && asset["state"].toString()=="uploaded" &&
                asset["browser_download_url"].toString()==QString(url).replace("/tag/","/download/")+'/'+name &&
                QRegularExpression("^sha256:[a-f0-9]{64}$").match(asset["digest"].toString()).hasMatch() &&
                asset["size"].toDouble()>0 && asset["size"].toDouble()<=MaxPackage) return asset;
        }
    }
    return {};
}
void ForkUpdater::fail(const QString& message) { m_State="error"; m_Message=message; emit changed(); }
void ForkUpdater::installLatest() {
    if (m_State=="downloading" || m_State=="preparing" || m_State=="restarting") return;
    if (!supported()) { fail(tr("Use the original installer or package manager for this installation.")); return; }
    try { if (platform()!="windows") signingRequirement(appRoot()); }
    catch (const std::exception& e) { fail(QString::fromUtf8(e.what())); return; }
    m_State="downloading"; m_Message=tr("Checking the fork release and downloading its verified package…"); emit changed();
    QNetworkRequest request(QUrl("https://api.github.com/repos/snchngny/deskport/releases?per_page=100"));
    request.setRawHeader("Accept","application/vnd.github+json"); request.setRawHeader("User-Agent","DeskPort/" VERSION_STR);
    request.setTransferTimeout(30000);
    auto reply=m_Network.get(request);
    connect(reply,&QNetworkReply::readyRead,reply,[reply] { if(reply->bytesAvailable()>1024*1024) reply->abort(); });
    connect(reply,&QNetworkReply::finished,this,[this,reply] {
        const auto bytes=reply->readAll(); const bool ok=reply->error()==QNetworkReply::NoError && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()==200; reply->deleteLater();
        const auto asset=ok ? selectAsset(bytes,VERSION_STR,platform()) : QJsonObject();
        if(asset.isEmpty()) { fail(tr("No compatible package with a GitHub SHA-256 digest is available.")); return; }
        const QString base=QStandardPaths::writableLocation(QStandardPaths::CacheLocation)+"/updates";
        QDir().mkpath(base); QTemporaryDir run(base+"/update-XXXXXX");
        if(!run.isValid()) { fail(tr("Cannot create an update staging directory.")); return; }
        run.setAutoRemove(false); const auto dir=run.path();
        QFile::setPermissions(dir,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
        const QString path=dir+'/'+asset["name"].toString();
        auto file=new QFile(path,this);
        if(!file->open(QIODevice::WriteOnly|QIODevice::NewOnly)) { file->deleteLater(); fail(tr("Cannot write the update package.")); return; }
        file->setPermissions(QFile::ReadOwner|QFile::WriteOwner);
        auto digest=std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
        auto count=std::make_shared<qint64>(0); auto good=std::make_shared<bool>(true);
        QNetworkRequest download(QUrl(asset["browser_download_url"].toString())); download.setTransferTimeout(60000);
        auto data=m_Network.get(download); data->setReadBufferSize(1024*1024);
        const qint64 expected=qint64(asset["size"].toDouble());
        auto drain=[data,file,digest,count,good,expected] {
            const auto chunk=data->readAll(); *count+=chunk.size();
            if(*count>expected || file->write(chunk)!=chunk.size()) { *good=false; data->abort(); return; } digest->addData(chunk);
        };
        connect(data,&QNetworkReply::readyRead,this,drain);
        connect(data,&QNetworkReply::finished,this,[this,data,file,digest,count,good,asset,path,dir,expected,drain] {
            drain(); file->close(); file->deleteLater();
            const bool valid=*good && data->error()==QNetworkReply::NoError && data->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()==200 &&
                *count==expected && digest->result().toHex()==asset["digest"].toString().mid(7).toLatin1(); data->deleteLater();
            if(!valid) { fail(tr("The download failed SHA-256 or size verification. The installed app was retained.")); return; }
            m_State="preparing";m_Message=tr("Preparing the update; settings and signing identity will be retained…");emit changed();
            QTimer::singleShot(0,this,[this,path,dir] { prepare(path,dir); });
        });
    });
}

void ForkUpdater::prepare(const QString& archive,const QString& directory) {
    try {
        const QString payload=directory+"/payload"; require(QDir().mkpath(payload),"Cannot create the update staging directory.");
        QString candidate;
#ifdef Q_OS_MACOS
        const QString mount=directory+"/volume";require(QDir().mkpath(mount),"Cannot create the disk image mount point.");
        command("/usr/bin/hdiutil",{"attach","-readonly","-nobrowse","-mountpoint",mount,archive});
        try {
            verifyMac(mount+"/DeskPort.app");
            candidate=payload+"/DeskPort.app";
            command("/usr/bin/ditto",{mount+"/DeskPort.app",candidate});
        } catch (...) { command("/usr/bin/hdiutil",{"detach",mount}); throw; }
        command("/usr/bin/hdiutil",{"detach",mount});
        const auto identity=signingRequirement(appRoot());
        command("/usr/bin/codesign",{"--force","--deep","--sign",identity,"--timestamp=none","--preserve-metadata=identifier,entitlements,flags",candidate});
        verifyMac(candidate,identity);
#elif defined(Q_OS_WIN)
        // Validate entries before extraction. Reject links, traversal, ADS and
        // decompression bombs; Expand-Archive then only sees a bounded archive.
        const auto script=QString::fromUtf8(R"PS(
$Archive=$env:DESKPORT_UPDATE_ARCHIVE
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z=[IO.Compression.ZipFile]::OpenRead($Archive)
try {
 $total=[long]0
 if($z.Entries.Count -gt 30000){throw 'Too many archive entries'}
 foreach($e in $z.Entries){
  $n=$e.FullName
  if(!$n -or $n.StartsWith('/') -or $n.Contains('\') -or $n.Contains(':') -or $n -match '[\x00-\x1f]' -or ($n.Split('/')|Where-Object {$_ -eq '..' -or $_ -eq '.'})){throw 'Unsafe archive path'}
  if((($e.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000){throw 'Archive links are not allowed'}
  $total+=$e.Length
  if($e.Length -gt 536870912 -or $total -gt 2147483648){throw 'Archive exceeds size limit'}
 }
} finally {$z.Dispose()}
)PS");
        auto environment=QProcessEnvironment::systemEnvironment();environment.insert("DESKPORT_UPDATE_ARCHIVE",archive);
        // This is a fixed built-in command with its path supplied as data, not
        // a downloaded script. No execution-policy override is used.
        command("powershell.exe",{"-NoProfile","-NonInteractive","-Command",script},120000,environment);
        // Windows' bundled libarchive handles the long third-party notice paths.
        command("tar.exe",{"-xf",archive,"-C",payload});
        candidate=payload;
        require(QFileInfo(executable(candidate)).isFile() && QFileInfo(candidate+"/host/deskport-host.exe").isFile(),"The update is not a full portable package.");
#else
        throw std::runtime_error("Use the system package manager on this platform.");
#endif
        const auto target=appRoot();
        QJsonObject request{{"version",1},{"target",target},{"parent",double(QCoreApplication::applicationPid())},
            {"oldSha256",QString::fromLatin1(hashFile(executable(target)))},{"newSha256",QString::fromLatin1(hashFile(executable(candidate)))},
            {"candidate",candidate},{"identity",platform()=="windows" ? QString() : signingRequirement(target)}};
        const auto job=directory+"/apply.json";QSaveFile out(job);require(out.open(QIODevice::WriteOnly),"Cannot save the update transaction.");
        out.setPermissions(QFile::ReadOwner|QFile::WriteOwner);out.write(QJsonDocument(request).toJson());require(out.commit(),"Cannot save the update transaction.");
        require(QProcess::startDetached(executable(candidate),{"--apply-fork-update",job},directory),"Cannot start the update installer.");
        m_State="restarting";m_Message=tr("Restarting DeskPort to install the update…");emit changed();
        require(m_Exit && m_Exit(),"Cannot prepare a normal shutdown for the update. The current app was retained.");
    } catch(const std::exception& e) { fail(QString::fromUtf8(e.what())); }
}

int ForkUpdater::apply(int argc,char** argv) {
    if(argc!=3 || QByteArray(argv[1])!="--apply-fork-update") return -1;
    QCoreApplication app(argc,argv);
    const QString job=QString::fromLocal8Bit(argv[2]); QString target,backup,prepared;bool stopped=false;
    QJsonObject result{{"status","failed"}};
    try {
        QFile f(job);require(f.open(QIODevice::ReadOnly) && f.size()<16384,"Invalid update transaction.");
        const auto r=QJsonDocument::fromJson(f.readAll()).object();
        target=r["target"].toString(); const auto candidate=r["candidate"].toString();
        const QString transactionDirectory=
#ifdef Q_OS_MACOS
            QFileInfo(QFileInfo(candidate).absolutePath()).absolutePath();
#else
            QFileInfo(candidate).absolutePath();
#endif
        require(r["version"].toInt()==1 && QFileInfo(target).isDir() && !QFileInfo(target).isSymLink() &&
            QFileInfo(target).canonicalFilePath()==target && candidate==appRoot() &&
            QFileInfo(job).canonicalPath()==transactionDirectory,"Invalid update paths.");
        require(QFileInfo(target).absolutePath()!=target && target!=QDir::homePath() && QFileInfo(executable(target)).isFile(),"Invalid installation target.");
#ifdef Q_OS_MACOS
        require(QFileInfo(target).fileName()=="DeskPort.app" && !target.startsWith("/System/") && !target.startsWith("/Library/"),"Invalid Mac installation target.");
        verifyMac(target,r["identity"].toString());verifyMac(candidate,r["identity"].toString());
#endif
        require(hashFile(executable(target))==r["oldSha256"].toString().toLatin1() && hashFile(executable(candidate))==r["newSha256"].toString().toLatin1(),"An update file changed before installation.");
        const qint64 parent=qint64(r["parent"].toDouble());require(parent>1 && parent!=QCoreApplication::applicationPid(),"Invalid app process.");
        for(int i=0;i<600 && processAlive(parent);++i) QThread::msleep(100);
        require(!processAlive(parent),"DeskPort did not finish closing. The update was not installed.");stopped=true;
        require(hashFile(executable(target))==r["oldSha256"].toString().toLatin1(),"The installed app changed while closing.");
        const auto token=QUuid::createUuid().toString(QUuid::WithoutBraces);
        prepared=QFileInfo(target).absolutePath()+"/.DeskPort-update-"+token+".noindex";
#ifdef Q_OS_MACOS
        prepared+="/DeskPort.app";
        require(QDir().mkpath(QFileInfo(prepared).absolutePath()),"Cannot stage the replacement app.");
        command("/usr/bin/ditto",{candidate,prepared});verifyMac(prepared,r["identity"].toString());
        const auto backups=QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)+"/DeskPort/backups/updates.noindex";
        require(QDir().mkpath(backups),"Cannot create the rollback directory.");backup=backups+'/'+token+".app";
        command("/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister",{"-u",target});
#else
        copyTree(candidate,prepared);
        // Portable QSettings live beside the executable. Copy them only after
        // the old process has saved and closed; never overlay package settings.
        require(!QFileInfo::exists(prepared+"/DeskPort"),"The package unexpectedly contains portable user settings.");
        if(QFileInfo(target+"/DeskPort").isDir()) copyTree(target+"/DeskPort",prepared+"/DeskPort");
        backup=target+"-before-update-"+token;
#endif
        require(hashFile(executable(prepared))==r["newSha256"].toString().toLatin1(),"The staged executable failed verification.");
        result["target"]=target;result["backup"]=backup;result["prepared"]=prepared;
        // Persist the rollback mapping before either rename, including when
        // power is lost between retaining the old app and activating the new one.
        QSaveFile journal(QFileInfo(job).absolutePath()+"/transaction.json");
        require(journal.open(QIODevice::WriteOnly),"Cannot save the rollback journal.");
        journal.setPermissions(QFile::ReadOwner|QFile::WriteOwner);
        journal.write(QJsonDocument(result).toJson());require(journal.commit(),"Cannot save the rollback journal.");
        require(QDir().rename(target,backup),"Cannot retain the old app for rollback.");
        if(!QDir().rename(prepared,target)) {
            require(QDir().rename(backup,target),"Update swap and rollback failed; restore the retained app manually.");
            throw std::runtime_error("Cannot activate the update; the old app was restored.");
        }
        result={{"status","completed"},{"backup",backup},{"settings","retained"}};
    } catch(const std::exception& e) { result["error"]=QString::fromUtf8(e.what()); }
    QSaveFile record(QFileInfo(job).absolutePath()+"/result.json");
    if(record.open(QIODevice::WriteOnly)) {record.setPermissions(QFile::ReadOwner|QFile::WriteOwner);record.write(QJsonDocument(result).toJson());record.commit();}
    if(stopped && QFileInfo(executable(target)).isFile()) launch(target);
    return result["status"].toString()=="completed" ? 0 : 1;
}
