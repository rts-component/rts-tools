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

ProtoField hiddenLength()
{
    ProtoField f;
    f.type = ProtoFieldType::Length;
    f.show = false;
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
    void encodeRoundTrip();
    void encodeWithLengthAndSeq();
    void encodeRejectsBadInput();
    void variableDataBlock();
    void variableDataBetweenFixedFields();
    void textDataBlock();
    void dataBlockValidation();
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

// 组包出来的帧必须能被解析器原样解出（覆盖多种类型、字节序、校验）
void TestProtocolParser::encodeRoundTrip()
{
    ProtoField u16 = field(ProtoFieldType::Byte2, "u");
    ProtoField i16 = field(ProtoFieldType::Byte2, "i");
    i16.conversion = ProtoConversion::Int;
    i16.bigEndian = true;
    ProtoField f32 = field(ProtoFieldType::Byte4, "f");
    f32.conversion = ProtoConversion::Float;
    ProtoField crc = field(ProtoFieldType::Checksum);
    crc.checksum = ProtoChecksum::Crc16Modbus;
    ProtoField tail = field(ProtoFieldType::Tail);
    tail.value = QByteArray::fromHex("0D0A");
    const QVector<ProtoField> layout{headerField(QByteArray::fromHex("AA55")), u16, i16, f32, crc, tail};

    QByteArray frame;
    QString error;
    QVERIFY2(encodeFrame(layout, QStringLiteral("4660 -2 1.5"), false, 0, &frame, &error), qPrintable(error));
    QVERIFY(frame.startsWith(QByteArray::fromHex("AA55 3412 FFFE 0000C03F")));
    QVERIFY(frame.endsWith(QByteArray::fromHex("0D0A")));

    ProtocolParser parser;
    parser.setLayout(layout);
    const auto frames = parser.feed(frame);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first(), QStringList({"4660", "-2", "1.5"}));
}

void TestProtocolParser::encodeWithLengthAndSeq()
{
    ProtoField seq = field(ProtoFieldType::Seq, "seq");
    ProtoField id = field(ProtoFieldType::Id, "id");
    id.value = QByteArray::fromHex("B0");
    ProtoField hexValue = field(ProtoFieldType::Byte2, "h");
    hexValue.conversion = ProtoConversion::Hex;
    const QVector<ProtoField> layout{headerField(QByteArray::fromHex("AA")), seq, id,
                                     field(ProtoFieldType::Length), hexValue,
                                     field(ProtoFieldType::Byte1, "b"), field(ProtoFieldType::Checksum)};
    QByteArray frame;
    QVERIFY(encodeFrame(layout, QStringLiteral("1234 7"), false, 5, &frame));
    // 头 AA, 序号 05, ID B0, 长度 03, 0x1234 小端 34 12, 数据 07, 校验
    QVERIFY(frame.startsWith(QByteArray::fromHex("AA 05 B0 03 3412 07")));
    QCOMPARE(frame.size(), 8);

    // "Hex" 选项：十进制字段的文本按十六进制解释
    QVERIFY(encodeFrame(layout, QStringLiteral("1234 0A"), true, 0, &frame));
    QCOMPARE(quint8(frame.at(6)), quint8(0x0A));
}

void TestProtocolParser::encodeRejectsBadInput()
{
    const QVector<ProtoField> layout{headerField(QByteArray::fromHex("AA")), field(ProtoFieldType::Byte1, "a")};
    QByteArray frame;
    QString error;
    QVERIFY(!encodeFrame(layout, QStringLiteral(""), false, 0, &frame, &error));       // 值个数不对
    QVERIFY(!encodeFrame(layout, QStringLiteral("256"), false, 0, &frame, &error));  // 放不下
    QVERIFY(!encodeFrame(layout, QStringLiteral("-1"), false, 0, &frame, &error));   // 无符号不能为负
    QVERIFY(!encodeFrame(layout, QStringLiteral("abc"), false, 0, &frame, &error));  // 不是数字
    QVERIFY(!error.isEmpty());
    QVERIFY(!encodeFrame({}, QStringLiteral(""), false, 0, &frame, &error));           // 定义本身无效
}

// 变长 Data 段：数据区长度由 Length 决定，不同长度的帧都能解出
void TestProtocolParser::variableDataBlock()
{
    const QVector<ProtoField> layout{headerField(QByteArray::fromHex("AA")), hiddenLength(),
                                     field(ProtoFieldType::Data, "payload"), field(ProtoFieldType::Checksum)};
    QVERIFY(validateProtocol(layout));

    ProtocolParser parser;
    parser.setLayout(layout);
    QByteArray frame;
    QVERIFY(encodeFrame(layout, QStringLiteral("11 22 33"), false, 0, &frame));
    QCOMPARE(frame.left(5), QByteArray::fromHex("AA 03 11 22 33"));
    QByteArray longer;
    QVERIFY(encodeFrame(layout, QStringLiteral("01 02 03 04 05 06"), false, 0, &longer));
    QByteArray empty;
    QVERIFY(encodeFrame(layout, QString(), false, 0, &empty));

    const auto frames = parser.feed(frame + longer + empty);
    QCOMPARE(frames.size(), 3);
    QCOMPARE(frames.at(0), QStringList({"11 22 33"}));
    QCOMPARE(frames.at(1), QStringList({"01 02 03 04 05 06"}));
    QCOMPARE(frames.at(2), QStringList({""}));
    QCOMPARE(parser.badFrames(), quint64(0));
}

// Data 前后各有固定字段：前面的取开头的数值，后面的取结尾的数值
void TestProtocolParser::variableDataBetweenFixedFields()
{
    ProtoField id = field(ProtoFieldType::Id, "id");
    id.value = QByteArray::fromHex("B0");
    id.show = false;
    const QVector<ProtoField> layout{headerField(QByteArray::fromHex("AA55")), id,
                                     hiddenLength(), field(ProtoFieldType::Byte1, "a"),
                                     field(ProtoFieldType::Data, "d"), field(ProtoFieldType::Byte2, "b"),
                                     field(ProtoFieldType::Checksum)};
    QByteArray frame;
    QString error;
    QVERIFY2(encodeFrame(layout, QStringLiteral("7 AA BB CC 4660"), false, 0, &frame, &error), qPrintable(error));
    // 头 AA55, ID B0, 长度 = 1 + 3 + 2 = 6, a = 07, 数据 AA BB CC, b = 0x1234 小端
    QVERIFY(frame.startsWith(QByteArray::fromHex("AA55 B0 06 07 AABBCC 3412")));

    ProtocolParser parser;
    parser.setLayout(layout);
    const auto frames = parser.feed(frame);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first(), QStringList({"7", "AA BB CC", "4660"}));

    // 数据区比固定字段还短：丢弃
    QVERIFY(parser.feed(QByteArray::fromHex("AA55 B0 02 07 00 00")).isEmpty());
    // 数值个数不够
    QVERIFY(!encodeFrame(layout, QStringLiteral("7"), false, 0, &frame, &error));
}

void TestProtocolParser::textDataBlock()
{
    ProtoField text = field(ProtoFieldType::Data, "msg");
    text.conversion = ProtoConversion::Text;
    const QVector<ProtoField> layout{headerField(QByteArray::fromHex("AA")), hiddenLength(), text,
                                     field(ProtoFieldType::Checksum)};
    QByteArray frame;
    // 文本里的空格和逗号原样保留
    QVERIFY(encodeFrame(layout, QStringLiteral("hello, world"), false, 0, &frame));
    QCOMPARE(frame.mid(1, 13), QByteArray("\x0c" "hello, world"));

    ProtocolParser parser;
    parser.setLayout(layout);
    const auto frames = parser.feed(frame);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first(), QStringList({"hello, world"}));
    QVERIFY(!protoFieldNumeric(text));
}

void TestProtocolParser::dataBlockValidation()
{
    // Data 必须在 Length 之后，且只能有一个
    QVERIFY(!validateProtocol({headerField(QByteArray::fromHex("AA")), field(ProtoFieldType::Data)}));
    QVERIFY(!validateProtocol({headerField(QByteArray::fromHex("AA")), hiddenLength(),
                               field(ProtoFieldType::Data), field(ProtoFieldType::Data)}));
    QVERIFY(validateProtocol({headerField(QByteArray::fromHex("AA")), hiddenLength(),
                              field(ProtoFieldType::Data)}));
}

QTEST_MAIN(TestProtocolParser)
#include "test_protocolparser.moc"
