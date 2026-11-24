#pragma once
#include <QObject>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <functional>

// Explicit local updates for this fork's portable Windows and locally signed
// macOS deployments. No service, credentials or user settings are replaced.
class ForkUpdater : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state MEMBER m_State NOTIFY changed)
    Q_PROPERTY(QString message MEMBER m_Message NOTIFY changed)
    Q_PROPERTY(bool supported READ supported CONSTANT)
public:
    explicit ForkUpdater(QObject* parent = nullptr);
    bool supported() const;
    Q_INVOKABLE void installLatest();
    void setExitHandler(std::function<bool()> handler) { m_Exit = std::move(handler); }
    static QJsonObject selectAsset(const QByteArray& releases, const QString& current,
                                  const QString& platform);
    static int apply(int argc, char** argv);
signals:
    void changed();
private:
    void fail(const QString& message);
    void prepare(const QString& archive, const QString& directory);
    QString m_State = QStringLiteral("idle"), m_Message;
    QNetworkAccessManager m_Network;
    std::function<bool()> m_Exit;
};
