#pragma once
#include <QJsonObject>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringList>
#include <cmath>

namespace DeskPortCli {
// Local API only. Never forward arbitrary command-line switches or settings.
inline bool validSessionOptions(const QJsonObject& options) {
    const QStringList allowed{"owner", "resolution", "fps", "bitrate", "codec", "decoder", "window", "pacing"};
    for (auto it = options.begin(); it != options.end(); ++it) {
        if (!allowed.contains(it.key())) return false;
        const auto value = it.value();
        if (it.key() == "owner") {
            if (!value.isString() || !QRegularExpression("^[a-f0-9]{32}$").match(value.toString()).hasMatch()) return false;
        } else if (it.key() == "resolution") {
            const auto match = QRegularExpression("^([1-9][0-9]{1,4})x([1-9][0-9]{1,4})$").match(value.toString());
            if (!value.isString() || !match.hasMatch() || match.captured(1).toInt() < 64 || match.captured(1).toInt() > 16384 ||
                match.captured(2).toInt() < 64 || match.captured(2).toInt() > 16384) return false;
        } else if (it.key() == "fps" || it.key() == "bitrate") {
            const double number = value.toDouble(-1);
            if (!value.isDouble() || number != std::floor(number) || number < 1 || number > (it.key() == "fps" ? 240 : 300000)) return false;
        } else if (it.key() == "pacing") {
            if (!value.isBool()) return false;
        } else {
            const QStringList choices = it.key() == "codec" ? QStringList{"auto", "H.264", "HEVC", "AV1"} :
                it.key() == "decoder" ? QStringList{"auto", "hardware", "software"} : QStringList{"windowed", "fullscreen", "borderless"};
            if (!value.isString() || !choices.contains(value.toString())) return false;
        }
    }
    return options.contains("owner");
}
inline bool parseSessionOptions(const QString& json, QJsonObject& options) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json.toUtf8(), &error);
    options = document.object();
    return error.error == QJsonParseError::NoError && document.isObject() && validSessionOptions(options);
}
}
