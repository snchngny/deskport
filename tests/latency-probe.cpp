#include <QtTest>
#include <QPainter>
#include "gui/latencyprobe.h"

namespace {
QImage marker(bool white=false) {
    QImage image(320,320,QImage::Format_RGB32); image.fill(QColor(30,30,30));
    QPainter p(&image);
    p.fillRect(126,126,69,69,white?Qt::white:Qt::black);
    p.fillRect(92,152,17,17,Qt::cyan); p.fillRect(212,152,17,17,Qt::magenta);
    p.fillRect(152,92,17,17,Qt::green); p.fillRect(152,212,17,17,Qt::yellow);
    return image;
}
}
class LatencyProbeTest : public QObject {
    Q_OBJECT
private slots:
    void markerAndUnrelatedImages() {
        QPoint center; bool white;
        QVERIFY(LatencyProbe::marker(marker(),&center,&white)); QCOMPARE(center,QPoint(160,160)); QVERIFY(!white);
        QVERIFY(LatencyProbe::marker(marker(true),&center,&white)); QVERIFY(white);
        auto scaled=marker().scaled(480,480,Qt::IgnoreAspectRatio,Qt::SmoothTransformation); scaled.setDevicePixelRatio(1.5);
        QVERIFY(LatencyProbe::marker(scaled,&center,&white)); QVERIFY((center-QPoint(240,240)).manhattanLength()<3);
        auto missing=marker(); QPainter p(&missing); p.fillRect(92,152,17,17,Qt::black); p.end();
        QVERIFY(!LatencyProbe::marker(missing,&center,&white));
        QImage blank(240,240,QImage::Format_RGB32); blank.fill(Qt::black);
        QVERIFY(!LatencyProbe::marker(blank,&center,&white));
        auto moved=marker(); QPainter q(&moved); q.fillRect(212,152,17,17,Qt::black); q.fillRect(212,182,17,17,Qt::magenta); q.end();
        QVERIFY(!LatencyProbe::marker(moved,&center,&white));
        auto inactive=marker(); QPainter inactivePainter(&inactive); inactivePainter.fillRect(126,126,69,69,QColor(120,120,120)); inactivePainter.end();
        QVERIFY(!LatencyProbe::marker(inactive,&center,&white));
        QVERIFY(LatencyProbe::marker(inactive,&center,&white,false));
    }
    void neverTypesIntoAnUnrecognizedTarget() {
        int keys=0,clicks=0;
        LatencyProbe probe([](QRect r){QImage im(r.size(),QImage::Format_RGB32);im.fill(Qt::black);return im;},
            [&](QPoint){++clicks;},[&]{++keys;},[]{return true;},this);
        QSignalSpy done(&probe,&LatencyProbe::finished);
        probe.arm(); QVERIFY(probe.select(QPoint(160,160)));
        QCOMPARE(keys,0); QCOMPARE(clicks,0); QCOMPARE(done.size(),1);
        QCOMPARE(done.first().first().toJsonObject()["status"].toString(),QString("marker-missing"));
    }
    void twelveResponsesDiscardTwoWarmups() {
        bool white=false; int keys=0,clicks=0;
        LatencyProbe probe([&](QRect r){return marker(white).copy(r);},[&](QPoint){++clicks;},
            [&]{++keys;QTimer::singleShot(35,this,[&]{white=!white;});},[]{return true;},this);
        QSignalSpy done(&probe,&LatencyProbe::finished);
        probe.arm(); probe.select(QPoint(160,160));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);
        const auto result=done.first().first().toJsonObject();
        QCOMPARE(result["status"].toString(),QString("completed"));
        QCOMPARE(result["samples"].toInt(),10); QCOMPARE(keys,12); QCOMPARE(clicks,1);
        QVERIFY(result["median_ms"].toDouble()>=30); QVERIFY(result["median_ms"].toDouble()<300);
    }
    void focusLossStopsFurtherInputs() {
        bool valid=true; int keys=0;
        LatencyProbe probe([](QRect r){return marker().copy(r);},[](QPoint){},
            [&]{++keys;valid=false;},[&]{return valid;},this);
        QSignalSpy done(&probe,&LatencyProbe::finished);
        probe.arm();probe.select(QPoint(160,160));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,2000);
        QCOMPARE(keys,1); QCOMPARE(done.first().first().toJsonObject()["status"].toString(),QString("cancelled"));
        QTest::qWait(300); QCOMPARE(keys,1); QCOMPARE(done.size(),1);
    }
    void inactiveHostMarkerReceivesNoKeys() {
        auto inactive=marker(); QPainter p(&inactive);p.fillRect(126,126,69,69,QColor(120,120,120));p.end();
        int keys=0,clicks=0;
        LatencyProbe probe([&](QRect r){return inactive.copy(r);},[&](QPoint){++clicks;},[&]{++keys;},[]{return true;},this);
        QSignalSpy done(&probe,&LatencyProbe::finished);
        probe.arm();probe.select(QPoint(160,160));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,2000);
        QCOMPARE(keys,0);QCOMPARE(clicks,1);
        QCOMPARE(done.first().first().toJsonObject()["status"].toString(),QString("marker-missing"));
    }
};
QTEST_MAIN(LatencyProbeTest)
#include "latency-probe.moc"
