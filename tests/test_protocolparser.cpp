#include "unpack/protocolparser.h"

#include <QtTest>

namespace {

ProtoField field(ProtoFieldType type, const QString &name = QString())
{
    ProtoField f;
    f.type = type;
    f.name = name;
    return f;
}

ProtoField headerField(const QByteArray &bytes)
{
    ProtoField f = field(ProtoFieldType::Header);
    f.value = bytes;
    return f;
}

QByteArray withSum8(QByteArray frame)
{
    frame.append(char(computeChecksum(ProtoChecksum::Sum8, frame)));
    return frame;
}

} // namespace

class TestProtocolParser : public QObject
{
    Q_OBJECT

private slots:
    void checksumKnownValues();
    void hexTextParsing();
    void fixedFrameDecodesLittleEndian();
    void bigEndianAndSigned();
    void floatConversion();
    void binaryConversion();
    void badChecksumIsRejectedAndResyncs();
    void frameSplitAcrossReads();
    void leadingGarbageIsSkipped();
    void idFilterIgnoresOtherFrames();
    void lengthFieldDrivesFrameSize();
    void tailMismatchRejected();
    void validationRules();
};

void TestProtocolParser::checksumKnownValues()
{
    const QByteArray check("123456789");
    QCOMPARE(computeChecksum(ProtoChecksum::Crc16Modbus, check), 0x4B37u);
    QCOMPARE(computeChecksum(ProtoChecksum::Crc16Ccitt, check), 0x29B1u);
    QCOMPARE(computeChecksum(ProtoChecksum::Crc8, check), 0xF4u);
    QCOMPARE(computeChecksum(ProtoChecksum::Sum8, QByteArray::fromHex("FF02")), 0x01u);
    QCOMPARE(computeChecksum(ProtoChecksum::Xor8, QByteArray::fromHex("0F F0")), 0xFFu);
    QCOMPARE(computeChecksum(ProtoChecksum::Sum16, QByteArray::fromHex("FF02")), 0x0101u);
}

void TestProtocolParser::hexTextParsing()
{
    QByteArray out;
    QVERIFY(parseHexBytes(QStringLiteral("AA 55"), &out));
    QCOMPARE(out, QByteArray::fromHex("AA55"));
    QVERIFY(parseHexBytes(QStringLiteral("0xAA, 0x55"), &out));
    QCOMPARE(out, QByteArray::fromHex("AA55"));
    QVERIFY(parseHexBytes(QStringLiteral("aa55"), &out));
    QVERIFY(!parseHexBytes(QStringLiteral("A"), &out));
    QVERIFY(!parseHexBytes(QStringLiteral("GG"), &out));
}

void TestProtocolParser::fixedFrameDecodesLittleEndian()
{
    ProtocolParser parser;
    parser.setLayout({headerField(QByteArray::fromHex("AA55")),
                      field(ProtoFieldType::Byte2, "voltage"),
                      field(ProtoFieldType::Checksum)});
    const QByteArray frame = withSum8(QByteArray::fromHex("AA55 3412"));
    const QVector<QStringList> frames = parser.feed(frame);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first(), QStringList({"4660"})); // 0x1234
    QCOMPARE(parser.goodFrames(), quint64(1));
}

void TestProtocolParser::bigEndianAndSigned()
{
    ProtoField value = field(ProtoFieldType::Byte2, "v");
    value.bigEndian = true;
    value.conversion = ProtoConversion::Int;
    ProtocolParser parser;
    parser.setLayout({headerField(QByteArray::fromHex("A5")), value, field(ProtoFieldType::Checksum)});
    const auto frames = parser.feed(withSum8(QByteArray::fromHex("A5 FF FE")));
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first(), QStringList({"-2"}));
}

void TestProtocolParser::floatConversion()
{
    ProtoField value = field(ProtoFieldType::Byte4, "f");
    value.conversion = ProtoConversion::Float;
    ProtocolParser parser;
    parser.setLayout({headerField(QByteArray::fromHex("A5")), value});
    // 1.5f = 0x3FC00000，小端字节序 00 00 C0 3F
    const auto frames = parser.feed(QByteArray::fromHex("A5 0000C03F"));
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first().first().toDouble(), 1.5);
}

void TestProtocolParser::binaryConversion()
{
    ProtoField value = field(ProtoFieldType::Byte2, "b");
    value.conversion = ProtoConversion::Binary;
    ProtocolParser parser;
    parser.setLayout({headerField(QByteArray::fromHex("A5")), value});
    // 小端 34 12 => 0x1234，高位在前
    QCOMPARE(parser.feed(QByteArray::fromHex("A5 3412")).first(),
             QStringList({"00010010 00110100"}));
    QVERIFY(!protoFieldNumeric(value));
    value.bigEndian = true;
    parser.setLayout({headerField(QByteArray::fromHex("A5")), value});
    QCOMPARE(parser.feed(QByteArray::fromHex("A5 3412")).first(),
             QStringList({"00110100 00010010"}));
}

void TestProtocolParser::badChecksumIsRejectedAndResyncs()
{
    ProtocolParser parser;
    parser.setLayout({headerField(QByteArray::fromHex("AA")), field(ProtoFieldType::Byte1, "a"),
                      field(ProtoFieldType::Checksum)});
    QByteArray bad = withSum8(QByteArray::fromHex("AA 01"));
    bad[2] = char(bad.at(2) + 1);
    const auto frames = parser.feed(bad + withSum8(QByteArray::fromHex("AA 07")));
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first(), QStringList({"7"}));
    QVERIFY(parser.badFrames() >= 1);
}

void TestProtocolParser::frameSplitAcrossReads()
{
    ProtocolParser parser;
    parser.setLayout({headerField(QByteArray::fromHex("AA55")), field(ProtoFieldType::Byte2, "a"),
                      field(ProtoFieldType::Checksum)});
    const QByteArray frame = withSum8(QByteArray::fromHex("AA55 0A00"));
    int total = 0;
    for (const char byte : frame)
        total += parser.feed(QByteArray(1, byte)).size();
    QCOMPARE(total, 1);
}

void TestProtocolParser::leadingGarbageIsSkipped()
{
    ProtocolParser parser;
    parser.setLayout({headerField(QByteArray::fromHex("AA55")), field(ProtoFieldType::Byte1, "a"),
                      field(ProtoFieldType::Checksum)});
    const auto frames = parser.feed(QByteArray::fromHex("00 AA 11 22") + withSum8(QByteArray::fromHex("AA55 09")));
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first(), QStringList({"9"}));
}

void TestProtocolParser::idFilterIgnoresOtherFrames()
{
    ProtoField id = field(ProtoFieldType::Id, "id");
    id.value = QByteArray::fromHex("B0");
    ProtocolParser parser;
    parser.setLayout({headerField(QByteArray::fromHex("AA")), id, field(ProtoFieldType::Byte1, "a"),
                      field(ProtoFieldType::Checksum)});
    const auto frames = parser.feed(withSum8(QByteArray::fromHex("AA C1 05"))
                                    + withSum8(QByteArray::fromHex("AA B0 06")));
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first(), QStringList({"176", "6"}));
    QCOMPARE(parser.badFrames(), quint64(0)); // 别的 ID 不算坏帧
}

void TestProtocolParser::lengthFieldDrivesFrameSize()
{
    ProtoField a = field(ProtoFieldType::Byte1, "a");
    ProtoField b = field(ProtoFieldType::Byte2, "b");
    ProtocolParser parser;
    parser.setLayout({headerField(QByteArray::fromHex("AA")), field(ProtoFieldType::Length), a, b,
                      field(ProtoFieldType::Checksum)});
    // 数据区 3 字节
    const auto frames = parser.feed(withSum8(QByteArray::fromHex("AA 03 01 0200")));
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first(), QStringList({"3", "1", "2"}));

    // 数据区只有 1 字节：放不下的字段为空
    const auto shortFrames = parser.feed(withSum8(QByteArray::fromHex("AA 01 07")));
    QCOMPARE(shortFrames.size(), 1);
    QCOMPARE(shortFrames.first(), QStringList({"1", "7", ""}));
}

void TestProtocolParser::tailMismatchRejected()
{
    ProtoField tail = field(ProtoFieldType::Tail);
    tail.value = QByteArray::fromHex("0D0A");
    ProtocolParser parser;
    parser.setLayout({headerField(QByteArray::fromHex("AA")), field(ProtoFieldType::Byte1, "a"), tail});
    QCOMPARE(parser.feed(QByteArray::fromHex("AA 01 0D 0B")).size(), 0);
    QCOMPARE(parser.feed(QByteArray::fromHex("AA 02 0D 0A")).size(), 1);
}

void TestProtocolParser::validationRules()
{
    QVERIFY(!validateProtocol({}));
    QVERIFY(!validateProtocol({field(ProtoFieldType::Byte1)}));
    QVERIFY(!validateProtocol({headerField(QByteArray())}));
    QVERIFY(validateProtocol({headerField(QByteArray::fromHex("AA")), field(ProtoFieldType::Byte1)}));
    // 校验之后不能再有数据
    QVERIFY(!validateProtocol({headerField(QByteArray::fromHex("AA")), field(ProtoFieldType::Checksum),
                               field(ProtoFieldType::Byte1)}));
    // 只能有一个长度字段
    QVERIFY(!validateProtocol({headerField(QByteArray::fromHex("AA")), field(ProtoFieldType::Length),
                               field(ProtoFieldType::Length)}));
}

QTEST_MAIN(TestProtocolParser)
#include "test_protocolparser.moc"
