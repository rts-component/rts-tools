#include "common/plotprotocol.h"

#include <QtTest>

using namespace PlotProtocol;

class TestPlotProtocol : public QObject
{
    Q_OBJECT

private slots:
    void frameRoundTrip();
    void frameSplitAcrossReads();
    void multipleFramesInOneRead();
    void oversizedFrameSetsError();
    void clearResetsError();
    void samplesRoundTrip();
    void samplesRejectCorruptCount();
    void seriesInfoRoundTrip();
    void truncatedPayloadsAreRejected();
};

void TestPlotProtocol::frameRoundTrip()
{
    const QByteArray payload = encodeHello(kVersion);
    FrameReader reader;
    reader.append(encodeFrame(MessageType::Hello, payload));

    MessageType type;
    QByteArray out;
    QVERIFY(reader.next(&type, &out));
    QCOMPARE(type, MessageType::Hello);
    QCOMPARE(out, payload);
    QVERIFY(!reader.next(&type, &out));
    QVERIFY(!reader.hasError());

    quint16 version = 0;
    QVERIFY(decodeHello(out, &version));
    QCOMPARE(version, kVersion);
}

// 半包：一次只喂 1 字节，拆帧器必须攒到整帧才吐出
void TestPlotProtocol::frameSplitAcrossReads()
{
    const QByteArray frame = encodeFrame(MessageType::HelloAck, encodeHelloAck(kVersion, 12345));
    FrameReader reader;
    MessageType type;
    QByteArray payload;

    for (int i = 0; i < frame.size() - 1; ++i) {
        reader.append(frame.mid(i, 1));
        QVERIFY2(!reader.next(&type, &payload), "frame must not be delivered early");
    }
    reader.append(frame.right(1));
    QVERIFY(reader.next(&type, &payload));
    QCOMPARE(type, MessageType::HelloAck);

    quint16 version = 0;
    qint64 start = 0;
    QVERIFY(decodeHelloAck(payload, &version, &start));
    QCOMPARE(start, qint64(12345));
}

// 粘包：两帧连在一起到达
void TestPlotProtocol::multipleFramesInOneRead()
{
    FrameReader reader;
    reader.append(encodeFrame(MessageType::Heartbeat, QByteArray())
                  + encodeFrame(MessageType::Resync, encodeResync(777)));

    MessageType type;
    QByteArray payload;
    QVERIFY(reader.next(&type, &payload));
    QCOMPARE(type, MessageType::Heartbeat);
    QVERIFY(payload.isEmpty());
    QVERIFY(reader.next(&type, &payload));
    QCOMPARE(type, MessageType::Resync);
    qint64 from = 0;
    QVERIFY(decodeResync(payload, &from));
    QCOMPARE(from, qint64(777));
    QVERIFY(!reader.next(&type, &payload));
}

void TestPlotProtocol::oversizedFrameSetsError()
{
    // 长度字段超过 kMaxFrameBytes：必须报错而不是等待/分配这么多内存
    QByteArray header;
    QDataStream stream(&header, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint32(kMaxFrameBytes + 1) << quint8(MessageType::Samples);

    FrameReader reader;
    reader.append(header);
    MessageType type;
    QByteArray payload;
    QVERIFY(!reader.next(&type, &payload));
    QVERIFY(reader.hasError());
}

// 回归：换新连接时 clear() 必须同时清掉错误状态，否则新连接的任何数据都会被当成协议错误
void TestPlotProtocol::clearResetsError()
{
    QByteArray header;
    QDataStream stream(&header, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint32(kMaxFrameBytes + 1) << quint8(MessageType::Samples);

    FrameReader reader;
    reader.append(header);
    MessageType type;
    QByteArray payload;
    reader.next(&type, &payload);
    QVERIFY(reader.hasError());

    reader.clear();
    QVERIFY(!reader.hasError());
    reader.append(encodeFrame(MessageType::Heartbeat, QByteArray()));
    QVERIFY(reader.next(&type, &payload));
    QVERIFY(!reader.hasError());
}

void TestPlotProtocol::samplesRoundTrip()
{
    QVector<Sample> in;
    for (quint32 i = 1; i <= 3; ++i) {
        Sample sample;
        sample.series = 2;
        sample.index = i;
        sample.timestampMs = 1000 + i;
        sample.value = float(i) * 1.5f;
        in.append(sample);
    }

    QVector<Sample> out;
    QVERIFY(decodeSamples(encodeSamples(in), &out));
    QCOMPARE(out.size(), in.size());
    for (int i = 0; i < in.size(); ++i) {
        QCOMPARE(out.at(i).series, in.at(i).series);
        QCOMPARE(out.at(i).index, in.at(i).index);
        QCOMPARE(out.at(i).timestampMs, in.at(i).timestampMs);
        QCOMPARE(out.at(i).value, in.at(i).value);
    }

    // ResyncData 额外带 truncated 标志
    bool truncated = false;
    QVERIFY(decodeResyncData(encodeResyncData(in, true), &out, &truncated));
    QVERIFY(truncated);
    QCOMPARE(out.size(), in.size());
}

// 声称有很多点、实际载荷很短：必须拒绝，不能按声称的数量去分配内存
void TestPlotProtocol::samplesRejectCorruptCount()
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint32(0x7fffffffu);

    QVector<Sample> out;
    QVERIFY(!decodeSamples(payload, &out));
    QVERIFY(!decodeResyncData(payload, &out, nullptr));
}

void TestPlotProtocol::seriesInfoRoundTrip()
{
    SeriesMeta in;
    in.series = 3;
    in.key = QStringLiteral("group:2");
    in.title = QStringLiteral("温度");
    in.unit = QStringLiteral("°C");
    in.color = QColor(0x12, 0x34, 0x56);
    in.lastIndex = 99;

    SeriesMeta out;
    QVERIFY(decodeSeriesInfo(encodeSeriesInfo(in), &out));
    QCOMPARE(out.series, in.series);
    QCOMPARE(out.key, in.key);
    QCOMPARE(out.title, in.title);
    QCOMPARE(out.unit, in.unit);
    QCOMPARE(out.color, in.color);
    QCOMPARE(out.lastIndex, in.lastIndex);
}

void TestPlotProtocol::truncatedPayloadsAreRejected()
{
    quint16 version = 0;
    QVERIFY(!decodeHello(QByteArray(), &version));
    QVERIFY(!decodeHello(QByteArray(1, '\x01'), &version));

    qint64 value = 0;
    QVERIFY(!decodeResync(QByteArray(3, '\x01'), &value));

    LinkMeta meta;
    QVERIFY(!decodeLinkMeta(QByteArray(2, '\x00'), &meta));
}

QTEST_APPLESS_MAIN(TestPlotProtocol)
#include "test_plotprotocol.moc"
