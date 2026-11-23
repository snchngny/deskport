#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "backend/autoupdatechecker.h"
class UpdateCheckerTest : public QObject {
    Q_OBJECT
private slots:
    void releases() {
        const QString tag = "v2.9.0-imac.4-preview";
        QJsonArray assets;
        for (const auto& suffix : {"-windows-x64-portable-full.zip", "-macos-x86_64-intel-bigsur.zip", "-macos-arm64.zip"}) {
            const auto name = QString("DeskPort-2.9.0-imac.4") + suffix;
            assets.append(QJsonObject{{"name", name}, {"state", "uploaded"}, {"size", 100},
                {"browser_download_url", "https://github.com/snchngny/deskport/releases/download/" + tag + '/' + name}});
        }
        QJsonObject release{{"tag_name", tag}, {"draft", false}, {"prerelease", true}, {"assets", assets},
            {"html_url", "https://github.com/snchngny/deskport/releases/tag/" + tag},
            {"body", "Fixes <script> are plain text"}, {"published_at", "2026-09-21T00:00:00Z"}};
        QString v,n,d,u;
        auto check = [&](QString current) { return AutoUpdateChecker::evaluate(QJsonDocument(QJsonArray{release}).toJson(),current,v,n,d,u); };
        QCOMPARE(check("2.9.0-imac.3"), QString("available"));
        QCOMPARE(v, QString("2.9.0-imac.4-preview")); QCOMPARE(n, release["body"].toString());
        QCOMPARE(check("2.9.0-imac.4"), QString("current")); QVERIFY(u.isEmpty());
        QCOMPARE(check("2.9.0-imac.4-identity-preview"), QString("current"));
        QCOMPARE(check("2.9.0-imac.5"), QString("current"));
        QCOMPARE(check("2.10.0-imac.1"), QString("current"));
        QCOMPARE(check("2.9.0"), QString("error"));
        QCOMPARE(check("2.9.0-imac.999999999999999999999"), QString("error"));
        release["prerelease"] = false; QCOMPARE(check("2.9.0-imac.3"), QString("available"));
        release["draft"] = true; QCOMPARE(check("2.9.0-imac.3"), QString("current"));
        release["draft"] = false;
        const auto complete = release;
        release["html_url"] = "https://github.com/keithxc/deskport/releases/tag/" + tag;
        QCOMPARE(check("2.9.0-imac.3"), QString("current")); QVERIFY(u.isEmpty());
        release["html_url"] = "https://example.com/update"; QCOMPARE(check("2.9.0-imac.3"), QString("current"));
        release = complete; release["tag_name"] = "v3.0.0"; QCOMPARE(check("2.9.0-imac.3"), QString("current"));
        release = complete; release.remove("draft"); QCOMPARE(check("2.9.0-imac.3"), QString("current"));
        release = complete; release.remove("prerelease"); QCOMPARE(check("2.9.0-imac.3"), QString("current"));
        release = complete; release["assets"] = QJsonArray(); QCOMPARE(check("2.9.0-imac.3"), QString("current"));
        assets.removeLast(); release["assets"] = assets; QCOMPARE(check("2.9.0-imac.3"), QString("current"));
        release = complete; assets = complete["assets"].toArray(); auto asset = assets[0].toObject();
        asset["state"] = "new"; assets[0] = asset; release["assets"] = assets;
        QCOMPARE(check("2.9.0-imac.3"), QString("current"));
        asset["state"] = "uploaded"; asset["size"] = 0; assets[0] = asset; release["assets"] = assets;
        QCOMPARE(check("2.9.0-imac.3"), QString("current"));
        asset["size"] = 100; asset["browser_download_url"] = "https://example.com/package.zip";
        assets[0] = asset; release["assets"] = assets; QCOMPARE(check("2.9.0-imac.3"), QString("current"));
        auto newer = complete;
        newer["tag_name"] = "v2.9.0-imac.10";
        newer["html_url"] = "https://github.com/snchngny/deskport/releases/tag/v2.9.0-imac.10";
        QJsonArray newerAssets;
        for (const auto& entry : complete["assets"].toArray()) {
            auto a = entry.toObject(); a["name"] = a["name"].toString().replace("imac.4", "imac.10");
            a["browser_download_url"] = a["browser_download_url"].toString().replace(tag, "v2.9.0-imac.10").replace("imac.4", "imac.10");
            newerAssets.append(a);
        }
        newer["assets"] = newerAssets;
        for (const auto& releases : {QJsonArray{newer, complete}, QJsonArray{complete, newer}}) {
            QCOMPARE(AutoUpdateChecker::evaluate(QJsonDocument(releases).toJson(),"2.9.0-imac.3",v,n,d,u), QString("available"));
            QCOMPARE(v, QString("2.9.0-imac.10"));
        }
        QCOMPARE(AutoUpdateChecker::evaluate("[]","2.9.0-imac.3",v,n,d,u), QString("current"));
        QCOMPARE(AutoUpdateChecker::evaluate("{}","2.9.0-imac.3",v,n,d,u), QString("error"));
        QCOMPARE(AutoUpdateChecker::evaluate("not JSON","2.9.0-imac.3",v,n,d,u), QString("error"));
    }
};
QTEST_GUILESS_MAIN(UpdateCheckerTest)
#include "update-checker.moc"
