#include "common/plotendpoints.h"
#include "plot/server/plotserver.h"
#include "plot/viewer/plotclient.h"

#include <QtTest>

// 进程内真实走一遍本地管道：PlotServer(主程序侧) <-> PlotClient(rts-plot 侧)
class TestPlotRoundTrip : public QObject
{
    Q_OBJECT

private slots:
    void streamsSamplesAndColors();
    void reconnectResyncsRecentSamples();

private:
    static PlotServer::GroupDefinition makeGroup();
    static QString pipeOf(PlotServer &server, const QString &groupKey);
};

PlotServer::GroupDefinition TestPlotRoundTrip::makeGroup()
{
    PlotServer::GroupDefinition group;
    group.key = QStringLiteral("g1");
    group.title = QStringLiteral("Row 1");
    for (int i = 0; i < 2; ++i) {
        PlotServer::SeriesDefinition series;
        series.key = QStringLiteral("g1:%1").arg(i);
        series.title = QStringLiteral("S%1").arg(i);
        series.color = QColor(0x10 * (i + 1), 0x20, 0x30);
        group.series.append(series);
    }
    return group;
}

// 端点清单是主程序告诉 rts-plot 管道名的唯一途径，这里一并验证写出与读回
QString TestPlotRoundTrip::pipeOf(PlotServer &server, const QString &groupKey)
{
    server.writeEndpointsFile();
    const PlotEndpointFile file = readPlotEndpoints(defaultEndpointsFilePath());
    if (!file.valid)
        return {};
    for (const PlotGroup &group : file.groups) {
        if (group.key == groupKey)
            return group.pipe;
    }
    return {};
}

void TestPlotRoundTrip::streamsSamplesAndColors()
{
    PlotServer server;
    server.syncSeries({makeGroup()});
    const QString pipe = pipeOf(server, QStringLiteral("g1"));
    QVERIFY2(!pipe.isEmpty(), "endpoint file must expose the group's pipe");

    PlotClient client;
    client.connectToPipe(pipe, QStringLiteral("g1"));
    QTRY_VERIFY_WITH_TIMEOUT(client.state() == PlotClient::State::Connected, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(server.isAttached(QStringLiteral("g1")), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(client.series().size(), 2, 5000);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < 50; ++i) {
        server.append(QStringLiteral("g1:0"), i, now + i);
        server.append(QStringLiteral("g1:1"), i * 2, now + i);
    }
    QTRY_COMPARE_WITH_TIMEOUT(client.totalSamples(), quint64(100), 5000);
    for (const PlotClient::SeriesData &series : client.series())
        QCOMPARE(series.samples.size(), 50);

    // 用户在曲线窗口里改色 -> 回传主程序
    QSignalSpy colorSpy(&server, &PlotServer::seriesColorChanged);
    client.setSeriesColor(client.series().first().id, QColor(0xaa, 0xbb, 0xcc));
    QTRY_COMPARE_WITH_TIMEOUT(colorSpy.count(), 1, 5000);
    QCOMPARE(colorSpy.first().at(1).value<QColor>(), QColor(0xaa, 0xbb, 0xcc));
}

// 客户端掉线再连上：服务端用热窗口补发最近的点，客户端按 index 去重，不会重复也不会缺
void TestPlotRoundTrip::reconnectResyncsRecentSamples()
{
    PlotServer server;
    server.syncSeries({makeGroup()});
    const QString pipe = pipeOf(server, QStringLiteral("g1"));
    QVERIFY(!pipe.isEmpty());

    PlotClient client;
    client.connectToPipe(pipe, QStringLiteral("g1"));
    QTRY_VERIFY_WITH_TIMEOUT(server.isAttached(QStringLiteral("g1")), 5000);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < 20; ++i)
        server.append(QStringLiteral("g1:0"), i, now + i);
    QTRY_COMPARE_WITH_TIMEOUT(client.totalSamples(), quint64(20), 5000);

    client.disconnectFromServer();
    QTRY_VERIFY_WITH_TIMEOUT(!server.isAttached(QStringLiteral("g1")), 5000);

    // 离线期间主程序继续采样
    for (int i = 20; i < 30; ++i)
        server.append(QStringLiteral("g1:0"), i, now + i);

    client.connectToPipe(pipe, QStringLiteral("g1"));
    QTRY_VERIFY_WITH_TIMEOUT(server.isAttached(QStringLiteral("g1")), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(client.totalSamples() > 0, 5000);
    QTest::qWait(300);

    // 各系列共用一张表（一行 = 一次采样），size() 就是行数
    QVERIFY(!client.series().isEmpty());
    QCOMPARE(client.series().first().samples.size(), 30); // 全部 30 个点，没有重复
}

QTEST_GUILESS_MAIN(TestPlotRoundTrip)
#include "test_plotroundtrip.moc"
