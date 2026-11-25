#include "latencyprobe.h"
#include <QApplication>
#include <QScreen>
#include <QPainter>
#include <QKeyEvent>
#include <QMessageBox>
#include <QInputDialog>
#include <QDebug>
#include <algorithm>
#include <cmath>

namespace {
// Four asymmetric, fixed-color fiducials guard against sending Return to an
// arbitrary document. The center alone alternates; compression is tolerated.
const QColor colors[] = {Qt::cyan, Qt::magenta, Qt::green, Qt::yellow};
const QPoint offsets[] = {{-60,0},{60,0},{0,-60},{0,60}};
int colorId(QRgb p) {
    const int r=qRed(p), g=qGreen(p), b=qBlue(p);
    if (r<80 && g>160 && b>160) return 0;
    if (r>160 && g<80 && b>160) return 1;
    if (r<80 && g>160 && b<80) return 2;
    if (r>160 && g>160 && b<80) return 3;
    return -1;
}
bool light(const QImage& image, QPoint center, bool* white) {
    if (!image.rect().contains(QRect(center-QPoint(2,2),QSize(5,5)))) return false;
    int sum=0;
    for (int y=-2;y<=2;++y) for (int x=-2;x<=2;++x) {
        const auto p=image.pixel(center+QPoint(x,y));
        if (std::max({qRed(p),qGreen(p),qBlue(p)})-std::min({qRed(p),qGreen(p),qBlue(p)})>45) return false;
        sum += qGray(p);
    }
    const int mean=sum/25;
    if (mean>65 && mean<190) return false;
    *white=mean>=190;
    return true;
}
class Marker final : public QWidget {
public:
    Marker() { setAttribute(Qt::WA_DeleteOnClose); setWindowTitle(LatencyTools::tr("DeskPort response latency marker")); resize(320,320); QTimer::singleShot(600000,this,&QWidget::close); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.fillRect(rect(),QColor(30,30,30));
        const QPoint c=rect().center();
        p.fillRect(QRect(c-QPoint(34,34),QSize(69,69)), !isActiveWindow()?QColor(120,120,120):(m_White?QColor(Qt::white):QColor(Qt::black)));
        for (int i=0;i<4;++i) p.fillRect(QRect(c+offsets[i]-QPoint(8,8),QSize(17,17)),colors[i]);
        p.setPen(Qt::white);
        p.drawText(QRect(8,8,width()-16,54),Qt::AlignCenter|Qt::TextWordWrap,LatencyTools::tr("On the receiving DeskPort, start a measurement and click the center of this marker."));
    }
    void keyPressEvent(QKeyEvent* e) override {
        if ((e->key()==Qt::Key_Return || e->key()==Qt::Key_Enter) && !e->isAutoRepeat()) { m_White=!m_White; repaint(); e->accept(); }
        else QWidget::keyPressEvent(e);
    }
    void changeEvent(QEvent* e) override {
        if(e->type()==QEvent::ActivationChange) update();
        QWidget::changeEvent(e);
    }
private:
    bool m_White=false;
};
}

LatencyProbe::LatencyProbe(Capture capture, std::function<void(QPoint)> click,
        std::function<void()> enter, std::function<bool()> valid, QObject* parent)
    : QObject(parent), m_Capture(std::move(capture)), m_Click(std::move(click)),
      m_Enter(std::move(enter)), m_Valid(std::move(valid)) {
    m_Timer.setInterval(8);
    connect(&m_Timer,&QTimer::timeout,this,&LatencyProbe::tick);
}
bool LatencyProbe::marker(const QImage& image, QPoint* center, bool* white, bool requireReady) {
    if (image.isNull() || image.width()>2048 || image.height()>2048) return false;
    QPointF sums[4]; int count[4]={};
    for (int y=0;y<image.height();++y) for (int x=0;x<image.width();++x) {
        const int id=colorId(image.pixel(x,y));
        if (id>=0) { sums[id]+=QPointF(x,y); ++count[id]; }
    }
    QPointF p[4];
    for (int i=0;i<4;++i) { if(count[i]<4) return false; p[i]=sums[i]/count[i]; }
    const QPointF a=(p[0]+p[1])/2, b=(p[2]+p[3])/2;
    const qreal radius=(p[1].x()-p[0].x())/2;
    if (radius<9 || radius>image.width()/2.0 ||
        std::abs(p[0].y()-p[1].y())>3 || std::abs(p[2].x()-p[3].x())>3 ||
        std::abs(a.x()-b.x())>3 || std::abs(a.y()-b.y())>3 ||
        std::abs((p[3].y()-p[2].y())/2-radius)>3) return false;
    *center=((a+b)/2).toPoint();
    *white=false;
    return light(image,*center,white) || !requireReady;
}
void LatencyProbe::arm() {
    if (busy()) cancel();
    m_Samples.clear(); m_State=Selecting; m_Age.start(); m_Timer.start();
}
void LatencyProbe::cancel() { if(busy()) finish(QStringLiteral("cancelled")); }
bool LatencyProbe::target(QPoint* center, bool* white, bool requireReady) {
    const auto image=m_Capture(QRect(m_Point-QPoint(120,120),QSize(240,240)));
    QPoint pixel;
    if (!marker(image,&pixel,white,requireReady)) return false;
    const qreal scale=image.devicePixelRatio();
    if (!std::isfinite(scale) || scale<=0) return false;
    *center=m_Point-QPoint(120,120)+QPoint(qRound(pixel.x()/scale),qRound(pixel.y()/scale));
    return true;
}
bool LatencyProbe::select(QPoint point) {
    if (!selecting()) return false;
    if (!m_Valid()) { finish("cancelled"); return true; }
    m_Point=point; QPoint center;
    if (!target(&center,&m_White,false)) { finish("marker-missing"); return true; }
    m_Point=center;
    // The selected marker receives a click only after its signature was found.
    m_Click(center); m_State=Settle; m_Trial.start();
    return true;
}
void LatencyProbe::tick() {
    if (m_State==Idle) return;
    if (!m_Valid()) { finish("cancelled"); return; }
    if (m_Age.elapsed()>45000) { finish("timeout"); return; }
    if (m_State==Selecting) return;
    if (m_State==Settle || m_State==Cooldown) {
        if(m_Trial.elapsed()<250) return;
        QPoint center;
        if(!target(&center,&m_White) || (center-m_Point).manhattanLength()>4) { finish("marker-missing"); return; }
        // Timestamp immediately before sending through the normal session path.
        m_State=Waiting; m_Trial.start(); m_Enter(); return;
    }
    if (m_State!=Waiting) return;
    const auto image=m_Capture(QRect(m_Point-QPoint(4,4),QSize(8,8)));
    bool white;
    if(image.isNull() || !light(image,QPoint(image.width()/2,image.height()/2),&white)) { finish("capture-unavailable"); return; }
    if(white!=m_White) {
        const qint64 us=m_Trial.nsecsElapsed()/1000;
        m_Samples.append(us);
        qInfo("DeskPort response-latency stage=sample trial=%d duration_us=%lld",int(m_Samples.size()),static_cast<long long>(us));
        if(m_Samples.size()==12) { finish("completed"); return; }
        m_State=Cooldown; m_Trial.restart();
    } else if(m_Trial.elapsed()>2000) finish("timeout");
}
void LatencyProbe::finish(const QString& code) {
    m_State=Idle; m_Timer.stop();
    QJsonObject result{{"status",code},{"samples",qMax(0,int(m_Samples.size())-2)}};
    if(code=="completed" && m_Samples.size()==12) {
        auto values=m_Samples.mid(2); std::sort(values.begin(),values.end());
        result["median_ms"]=(values[4]+values[5])/2000.0;
        result["min_ms"]=values.first()/1000.0; result["max_ms"]=values.last()/1000.0;
    }
    qInfo("DeskPort response-latency stage=%s trial=%d duration_us=0",qPrintable(code),int(m_Samples.size()));
    emit finished(result);
}
LatencyTools& LatencyTools::instance() { static LatencyTools tools; return tools; }
void LatencyTools::showMarker() {
    if(m_Marker) { m_Marker->show(); m_Marker->raise(); m_Marker->activateWindow(); return; }
    const auto screens=QGuiApplication::screens(); QStringList choices;
    for(int i=0;i<screens.size();++i) choices<<tr("Display %1 (%2 × %3)").arg(i+1).arg(screens[i]->size().width()).arg(screens[i]->size().height());
    if(choices.isEmpty()) return;
    bool ok=false;
    const auto choice=QInputDialog::getItem(nullptr,tr("Response latency marker"),tr("Display to test"),choices,0,false,&ok);
    if(!ok) return;
    auto screen=screens[choices.indexOf(choice)];
    m_Marker=new Marker;
    m_Marker->move(screen->availableGeometry().center()-QPoint(160,160));
    m_Marker->show(); m_Marker->raise(); m_Marker->activateWindow();
}
void LatencyTools::measure() {
    QMessageBox::information(nullptr,tr("Response latency"),tr("Open the DeskPort marker on the shared display first. After closing this message, click its center in the remote picture. Twelve Return presses will be sent only after the marker is recognized; the first two are warm-ups. Escape or changing focus cancels the test. No extra OS permissions are requested."));
    emit probeRequested();
}
void LatencyTools::report(const QJsonObject& result) {
    QString text;
    if(result["status"]=="completed") text=tr("Input to response picture: median %1 ms (min %2, max %3), 10 samples.").arg(result["median_ms"].toDouble(),0,'f',1).arg(result["min_ms"].toDouble(),0,'f',1).arg(result["max_ms"].toDouble(),0,'f',1);
    else {
        const auto code=result["status"].toString();
        if(code=="marker-missing") text=tr("The marker was not recognized. Open it on the shared display and click its center. No latency result was accepted.");
        else if(code=="capture-unavailable") text=tr("This display backend could not read DeskPort's own picture. No extra permissions were requested and no latency result was accepted.");
        else if(code=="no-active-session") text=tr("Connect to a remote desktop before starting a measurement.");
        else if(code=="timeout") text=tr("No marker response arrived before the time limit. Check that remote input is enabled and the marker is in front.");
        else text=tr("Measurement cancelled. Keep the same desktop and window focused until it finishes.");
    }
    text+=QStringLiteral("\n\n")+tr("Includes software sampling overhead. This is not physical key-to-photon latency or network RTT. Numeric samples are recorded in the existing local diagnostic logs when enabled.");
    auto dialog=new QMessageBox(QMessageBox::Information,tr("Response latency"),text,QMessageBox::Ok);
    dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->open();
}
