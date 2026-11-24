#pragma once
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSslCertificate>
#include <QUuid>
#include <QSettings>

namespace PeerStore {
// A host UUID is not an identity. Old approvals can survive a host key change.
// Keep control and clipboard pinned to the same certificate as the video host.
inline QJsonObject approvedHost(const QJsonObject& peers, const QString& hostId,
                                const QSslCertificate& pinned = QSslCertificate()) {
    if (hostId.isEmpty()) return {};
    QJsonObject selected;
    QSslCertificate identity;
    for (const auto& value : peers) {
        const auto peer=value.toObject();
        if (peer["hostId"].toString().compare(hostId,Qt::CaseInsensitive) ||
            !peer["ready"].toBool() || !peer["granted"].toBool()) continue;
        const QSslCertificate certificate(peer["clientCert"].toString().toUtf8());
        const int port=peer["bindingPort"].toInt();
        if (certificate.isNull() || port<1 || port>65535 || (!pinned.isNull() && certificate!=pinned)) continue;
        if (!selected.isEmpty() && certificate!=identity) return {}; // No arbitrary first-record fallback.
        selected=peer;identity=certificate;
    }
    return selected;
}
inline QSslCertificate pinnedHost(QSettings& settings, const QString& hostId) {
    int count=settings.beginReadArray("hostsbackup");
    if (!count) {settings.endArray();count=settings.beginReadArray("hosts");}
    QSslCertificate result;
    for(int i=0;i<count;++i) {
        settings.setArrayIndex(i);
        if (!settings.value("uuid").toString().compare(hostId,Qt::CaseInsensitive)) {
            result=QSslCertificate(settings.value("srvcert").toByteArray());break;
        }
    }
    settings.endArray();return result;
}
inline bool write(const QString& path, const QJsonObject& object) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    const auto bytes = QJsonDocument(object).toJson();
    return file.write(bytes) == bytes.size() && file.commit();
}
inline QJsonObject read(const QString& path, bool* ok = nullptr) {
    QFile file(path);
    if (!file.exists()) { if (ok) *ok = true; return {}; }
    if (!file.open(QIODevice::ReadOnly)) { if (ok) *ok = false; return {}; }
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (ok) *ok = error.error == QJsonParseError::NoError && doc.isObject();
    return doc.object();
}
// The caller holds the host instance lock and has stopped its Sunshine process.
// Preserve unrelated clients and all unrelated top-level state.
inline bool trust(const QString& path, const QString& id, const QString& name,
                  const QSslCertificate& certificate, bool remove = false) {
    if (id.isEmpty() || (!remove && certificate.isNull())) return false;
    bool ok;
    auto state = read(path, &ok);
    if (!ok) return false;
    auto root = state["root"].toObject();
    if (root["uniqueid"].toString().isEmpty())
        root["uniqueid"] = QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
    const auto old = root["named_devices"];
    if (!old.isUndefined() && !old.isArray() && old.toString() != "") return false;
    QJsonArray devices;
    for (const auto& item : old.toArray()) {
        const auto device = item.toObject();
        const bool sameCertificate = !remove && QSslCertificate(device["cert"].toString().toUtf8()) == certificate;
        if (device["uuid"].toString() != id && !sameCertificate) devices.append(item);
    }
    if (!remove) devices.append(QJsonObject{{"uuid", id}, {"name", name},
        {"cert", QString::fromUtf8(certificate.toPem())}, {"enabled", "true"}});
    root["named_devices"] = devices;
    state["root"] = root;
    return write(path, state);
}
}
