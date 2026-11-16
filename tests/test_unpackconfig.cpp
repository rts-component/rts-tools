#include "unpack/unpackconfig.h"

#include <QtTest>

class TestUnpackConfig : public QObject
{
    Q_OBJECT

private slots:
    void groupIdsAreUnique();
    void protocolFieldsFollowLayout();
    void linChecksumConfigRoundTrips();
    void bitCurvesFollowBitGroups();
};

void TestUnpackConfig::groupIdsAreUnique()
{
    QVERIFY(newGroupId() != newGroupId());
    QVERIFY(!newGroupId().contains(QLatin1Char('{')));
}

// 面板字段 = 协议里"显示到面板"的字段；同位置的颜色 / 曲线勾选沿用
void TestUnpackConfig::protocolFieldsFollowLayout()
{
    ProtoField header;
    header.type = ProtoFieldType::Header;
    header.value = QByteArray::fromHex("AA");
    ProtoField hidden;
    hidden.type = ProtoFieldType::Byte1;
    hidden.name = "hidden";
    hidden.show = false;
    ProtoField value;
    value.type = ProtoFieldType::Byte2;
    value.name = "v";
    ProtoField text = value;
    text.name = "bits";
    text.conversion = ProtoConversion::Binary;

    UnpackField old;
    old.color = "#112233";
    old.curve = true;

    const QVector<UnpackField> fields = protocolFields({header, hidden, value, text}, {old});
    QCOMPARE(fields.size(), 2);
    QCOMPARE(fields.at(0).name, QStringLiteral("v"));
    QCOMPARE(fields.at(0).color, QStringLiteral("#112233"));
    QVERIFY(fields.at(0).curve);
    QVERIFY(!fields.at(1).numeric); // 二进制是文本，不能画曲线
}

void TestUnpackConfig::linChecksumConfigRoundTrips()
{
    ProtoField checksum;
    checksum.type = ProtoFieldType::Checksum;
    checksum.checksum = ProtoChecksum::Lin8;

    const QVector<ProtoField> restored = protoLayoutFromJson(protoLayoutToJson({checksum}));
    QCOMPARE(restored.size(), 1);
    QCOMPARE(restored.first().checksum, ProtoChecksum::Lin8);
}


// 二进制字段的每个位分组各带一个曲线开关：名称取自协议，颜色 / 勾选按位置沿用
void TestUnpackConfig::bitCurvesFollowBitGroups()
{
    ProtoField bits;
    bits.type = ProtoFieldType::Byte1;
    bits.name = "b";
    bits.conversion = ProtoConversion::Binary;
    bits.bitGroups = {ProtoBitGroup{"mode", 0, 1, {}}, ProtoBitGroup{"level", 2, 5, {}}};

    UnpackField old;
    old.bits = {UnpackBitCurve{"old", "#112233", true}};

    const QVector<UnpackField> fields = protocolFields({bits}, {old});
    QCOMPARE(fields.size(), 1);
    QCOMPARE(fields.first().bits.size(), 2);
    QCOMPARE(fields.first().bits.at(0).name, QStringLiteral("mode"));
    QCOMPARE(fields.first().bits.at(0).color, QStringLiteral("#112233"));
    QVERIFY(fields.first().bits.at(0).curve);
    QCOMPARE(fields.first().bits.at(1).name, QStringLiteral("level"));
    QVERIFY(!fields.first().bits.at(1).curve);
}

QTEST_APPLESS_MAIN(TestUnpackConfig)
#include "test_unpackconfig.moc"
