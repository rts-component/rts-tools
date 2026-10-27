#include "plot/server/plotserver.h"
#include "plot/viewer/plotcanvas.h"
#include "plot/viewer/plotclient.h"
#include "common/plotendpoints.h"

#include <QtTest>
#include <QWheelEvent>

// 暂停后的画面必须冻结：滚轮缩放不能让画布恢复跟随，暂停之后才到达的点也不能被画出来
class TestPlotCanvas : public QObject
{
    Q_OBJECT

private slots:
    void pausedViewStaysFrozenWhileZooming();
};

static void wheel(PlotCanvas *canvas, int delta, const QPointF &at)
{
    QWheelEvent event(at, canvas->mapToGlobal(at), QPoint(), QPoint(0, delta), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(canvas, &event);
}

void TestPlotCanvas::pausedViewStaysFrozenWhileZooming()
{
    PlotServer server;
    PlotServer::GroupDefinition group;
    group.key = QStringLiteral("g1");
    group.title = QStringLiteral("Row 1");
    PlotServer::SeriesDefinition series;
    series.key = QStringLiteral("g1:0");
    series.title = QStringLiteral("S0");
    series.color = QColor(0xe0, 0x30, 0x30);
    group.series.append(series);
    server.syncSeries({group});
    server.writeEndpointsFile();
    const PlotEndpointFile file = readPlotEndpoints(defaultEndpointsFilePath());
    QVERIFY(file.valid && !file.groups.isEmpty());

    PlotClient client;
    client.connectToPipe(file.groups.first().pipe, QStringLiteral("g1"));
    QTRY_VERIFY_WITH_TIMEOUT(server.isAttached(QStringLiteral("g1")), 5000);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < 100; ++i)
        server.append(QStringLiteral("g1:0"), (i % 20) - 10, now + i);
    QTRY_COMPARE_WITH_TIMEOUT(client.totalSamples(), quint64(100), 5000);

    PlotCanvas canvas(&client);
    canvas.resize(800, 400);
    canvas.grab(); // 先画一次，建立视口和缓存

    canvas.setPaused(true);

    // 向外缩放(滚轮向下)直到视口右边缘超过最新数据——这是之前会恢复“跟随”的情形
    for (int i = 0; i < 6; ++i)
        wheel(&canvas, -120, QPointF(400, 200));
    QVERIFY2(!canvas.following(), "paused canvas must not resume following new data");
    QVERIFY(!canvas.autoFitX());

    const QImage before = canvas.grab().toImage();

    // 暂停之后主程序继续采样；这些点落在已经放大的视口范围内，但不应被画出来
    for (int i = 100; i < 160; ++i)
        server.append(QStringLiteral("g1:0"), (i % 20) - 10, now + i);
    QTRY_COMPARE_WITH_TIMEOUT(client.totalSamples(), quint64(160), 5000);
    QTest::qWait(100);

    wheel(&canvas, 120, QPointF(400, 200)); // 再缩放一次，仍在暂停
    QVERIFY(!canvas.following());
    wheel(&canvas, -120, QPointF(400, 200)); // 缩回原来的视口

    const QImage after = canvas.grab().toImage();
    QCOMPARE(after.size(), before.size());
    QVERIFY2(after == before, "new samples must not appear while paused");
}

QTEST_MAIN(TestPlotCanvas)
#include "test_plotcanvas.moc"
