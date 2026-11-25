#pragma once
#include <QObject>
#include <QImage>
#include <QElapsedTimer>
#include <QTimer>
#include <QJsonObject>
#include <QPointer>
#include <functional>
class QWidget;

// Only application-owned pixels and the existing session input adapter are used.
// No screen-wide capture, OS input injection or accessibility API is involved.
class LatencyProbe : public QObject {
    Q_OBJECT
public:
    using Capture = std::function<QImage(QRect)>;
    LatencyProbe(Capture capture, std::function<void(QPoint)> click,
                 std::function<void()> enter, std::function<bool()> valid, QObject* parent);
    void arm();
    bool select(QPoint point);
    void cancel();
    void frameReady() { m_FramePending=true; if(m_State==Waiting && !m_Sampling) tick(); }
    bool busy() const { return m_State != Idle; }
    bool selecting() const { return m_State == Selecting; }
    static bool marker(const QImage& image, QPoint* center, bool* white, bool requireReady = true);
signals:
    void finished(QJsonObject result);
    void activityChanged(bool active);
private:
    enum State { Idle, Selecting, Settle, Waiting, Cooldown };
    void tick();
    void finish(const QString& code);
    bool target(QPoint* center, bool* white, bool requireReady = true);
    Capture m_Capture;
    std::function<void(QPoint)> m_Click;
    std::function<void()> m_Enter;
    std::function<bool()> m_Valid;
    QTimer m_Timer;
    QElapsedTimer m_Age, m_Trial;
    State m_State = Idle;
    QPoint m_Point;
    bool m_White = false;
    bool m_Sampling = false;
    bool m_FramePending = false;
    quint64 m_Epoch = 0;
    int m_CooldownMs = 250;
    QList<qint64> m_Samples;
};

class LatencyTools : public QObject {
    Q_OBJECT
public:
    static LatencyTools& instance();
    Q_INVOKABLE void showMarker();
    Q_INVOKABLE void measure();
    void report(const QJsonObject& result);
signals:
    void probeRequested();
private:
    QPointer<QWidget> m_Marker;
};
