#include "backend/identitymanager.h"
#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QDebug>
#ifdef Q_OS_UNIX
#include <sys/resource.h>
#endif

static QByteArray contents(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) qFatal("Cannot read fixture settings");
    return file.readAll();
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("DeskPortIdentityTest");
    QCoreApplication::setApplicationName("IsolatedFixture");
    const auto args = app.arguments();
    if (args.size() == 3 && args[1] == "load") {
#ifdef Q_OS_UNIX
        const rlimit noCore = {0, 0};
        setrlimit(RLIMIT_CORE, &noCore);
#endif
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, args[2]);
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, args[2]);
        IdentityManager::get()->getUniqueId();
        return 0;
    }
    QTemporaryDir first, second;
    if (!first.isValid() || !second.isValid()) qFatal("Cannot isolate fixtures");
    auto load = [&](const QString& path) {
        QProcess child;
        child.start(app.applicationFilePath(), {"load", path});
        if (!child.waitForFinished(15000)) {
            child.kill(); child.waitForFinished();
            qFatal("Identity fixture timed out");
        }
        return child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0;
    };
    if (!load(first.path()) || !load(second.path())) qFatal("Fresh identity failed");
    const auto settingsPath = [](const QString& root) {
        return root + "/DeskPortIdentityTest/IsolatedFixture.ini";
    };
    const QString path = settingsPath(first.path());
    const QByteArray original = contents(path);
    if (!load(first.path()) || contents(path) != original) qFatal("Identity changed on restart");
    QSettings other(settingsPath(second.path()), QSettings::IniFormat);
    const auto otherKey = other.value("key");
    int passed = 2;
    for (int scenario = 0; scenario < 8; ++scenario) {
        QFile restore(path);
        if (!restore.open(QIODevice::WriteOnly | QIODevice::Truncate) || restore.write(original) != original.size())
            qFatal("Cannot restore synthetic fixture");
        restore.close();
        {
            QSettings settings(path, QSettings::IniFormat);
            switch (scenario) {
            case 0: settings.remove("certificate"); break;
            case 1: settings.remove("key"); break;
            case 2: settings.setValue("certificate", QByteArray()); break;
            case 3: settings.setValue("key", QByteArray()); break;
            case 4: settings.setValue("certificate", QByteArray("invalid")); break;
            case 5: settings.setValue("key", QByteArray("invalid")); break;
            case 6: settings.setValue("key", otherKey); break;
            case 7: settings.remove("certificate"); settings.remove("key"); break;
            }
            settings.sync();
            if (settings.status() != QSettings::NoError) qFatal("Cannot seed fixture");
        }
        const auto before = contents(path);
        if (load(first.path())) qFatal("Invalid identity was accepted");
        if (contents(path) != before) qFatal("Invalid identity was overwritten");
        ++passed;
    }
    qInfo() << passed << "identity persistence checks passed (isolated synthetic credentials)";
}
