#include "flash/firmwareimage.h"
#include "flash/flmfile.h"
#include "tests/elfbuilder.h"

#include <QtTest>

class TestFlmFile : public QObject
{
    Q_OBJECT

private slots:
    void parsesDeviceAndEntries()
    {
        FlmFile flm;
        QString error;
        QVERIFY2(FlmFile::parse(ElfBuilder::buildTestFlm(), &flm, &error), qPrintable(error));
        QCOMPARE(flm.deviceName, QStringLiteral("Test Flash 64KB"));
        QCOMPARE(flm.deviceAddress, 0x08000000u);
        QCOMPARE(flm.deviceSize, 0x10000u);
        QCOMPARE(flm.pageSize, 256u);
        QCOMPARE(flm.emptyValue, quint8(0xFF));
        QCOMPARE(flm.programTimeoutMs, 100u);
        QCOMPARE(flm.eraseTimeoutMs, 500u);
        QCOMPARE(flm.sectors.size(), 1);
        QCOMPARE(flm.sectors[0].size, 0x400u);

        QCOMPARE(flm.initOffset, int(ElfBuilder::FlmLayout::init)); // Thumb 位已去掉
        QCOMPARE(flm.uninitOffset, int(ElfBuilder::FlmLayout::uninit));
        QCOMPARE(flm.eraseSectorOffset, int(ElfBuilder::FlmLayout::eraseSector));
        QCOMPARE(flm.programPageOffset, int(ElfBuilder::FlmLayout::programPage));
        QCOMPARE(flm.eraseChipOffset, int(ElfBuilder::FlmLayout::eraseChip));
        QCOMPARE(flm.staticBase, ElfBuilder::FlmLayout::staticBase);
        QCOMPARE(quint32(flm.image.size()), ElfBuilder::FlmLayout::imageSize);
        QCOMPARE(flm.image[ElfBuilder::FlmLayout::staticBase], char(0x5A));
    }

    void eraseChipIsOptional()
    {
        FlmFile flm;
        QString error;
        QVERIFY2(FlmFile::parse(ElfBuilder::buildTestFlm(false), &flm, &error), qPrintable(error));
        QCOMPARE(flm.eraseChipOffset, FlmFile::kNoEntry);
    }

    void rejectsBadInput()
    {
        FlmFile flm;
        QString error;
        QVERIFY(!FlmFile::parse("not an elf", &flm, &error));
        QVERIFY(!error.isEmpty());

        QByteArray notArm = ElfBuilder::buildTestFlm();
        notArm[18] = char(62); // x86-64
        QVERIFY(!FlmFile::parse(notArm, &flm, &error));

        // 缺少必需的函数
        const QByteArray noFunctions = ElfBuilder::build(
                {{QStringLiteral("PrgCode"), 1, 6, 0, QByteArray(16, '\0')}}, {}, {{0, 0, QByteArray(16, '\0'), 0}});
        QVERIFY(!FlmFile::parse(noFunctions, &flm, &error));
        QVERIFY(error.contains(QStringLiteral("Init")));

        // 文件被截断
        QVERIFY(!FlmFile::parse(ElfBuilder::buildTestFlm().left(100), &flm, &error));
    }

    void sectorsCovering()
    {
        FlmFile flm;
        flm.deviceAddress = 0x08000000;
        flm.deviceSize = 0x10000;
        // 前 16KB 是 4 个 4KB 扇区，后面是 4 个 12KB 扇区
        flm.sectors = {{0x1000, 0}, {0x3000, 0x4000}};

        QCOMPARE(flm.sectorsCovering(0x08000000, 0x08000001), QVector<quint32>({0x08000000}));
        QCOMPARE(flm.sectorsCovering(0x08000FFF, 0x08001001), QVector<quint32>({0x08000000, 0x08001000}));
        QCOMPARE(flm.sectorsCovering(0x08003800, 0x08004800),
                 QVector<quint32>({0x08003000, 0x08004000}));
        QCOMPARE(flm.sectorsCovering(0x08004001, 0x08008000), QVector<quint32>({0x08004000, 0x08007000}));
        QVERIFY(flm.sectorsCovering(0x08010000, 0x08010010).isEmpty()); // 超出设备
        QVERIFY(flm.sectorsCovering(0x08000010, 0x08000010).isEmpty()); // 空范围
    }

    void sectorsCoveringDeviceEndingAtFourGigabytes()
    {
        FlmFile flm;
        flm.deviceAddress = 0xF0000000;
        flm.deviceSize = 0x10000000; // 结束在 0x1_0000_0000，32 位的 end 会回绕成 0
        flm.sectors = {{0x01000000, 0}};
        const QVector<quint32> sectors = flm.sectorsCovering(flm.deviceAddress, quint64(flm.deviceAddress) + flm.deviceSize);
        QCOMPARE(sectors.size(), 16);
        QCOMPARE(sectors.first(), 0xF0000000u);
        QCOMPARE(sectors.last(), 0xFF000000u);
    }

    void firmwareIntelHex()
    {
        const QByteArray hex =
                ":020000040800F2\n"
                ":04000000DEADBEEFC4\n"
                ":0400040001020304EE\n"
                ":0400100000000000EC\n"
                ":00000001FF\n";
        QVector<FirmwareSegment> segments;
        QString error;
        QVERIFY2(FirmwareImage::parseIntelHex(hex, &segments, &error), qPrintable(error));
        segments = FirmwareImage::normalized(segments);
        QCOMPARE(segments.size(), 2); // 前两条相邻合并，第三条不连续
        QCOMPARE(segments[0].address, 0x08000000u);
        QCOMPARE(segments[0].data, QByteArray::fromHex("DEADBEEF01020304"));
        QCOMPARE(segments[1].address, 0x08000010u);

        // 校验和错
        QByteArray bad = hex;
        bad.replace("C4", "C5");
        QVERIFY(!FirmwareImage::parseIntelHex(bad, &segments, &error));
        QVERIFY(error.contains(QStringLiteral("checksum")));

        // 缺少结束记录
        QVERIFY(!FirmwareImage::parseIntelHex(":04000000DEADBEEFC4\n", &segments, &error));
    }

    void firmwareElf()
    {
        ElfBuilder::Segment seg;
        seg.virtualAddress = 0x20000000; // RAM 里的运行地址
        seg.physicalAddress = 0x08000000; // 烧录地址
        seg.data = QByteArray("firmware!");
        const QByteArray elf = ElfBuilder::build({}, {}, {seg});
        QVector<FirmwareSegment> segments;
        QString error;
        QVERIFY2(FirmwareImage::parseElf(elf, &segments, &error), qPrintable(error));
        QCOMPARE(segments.size(), 1);
        QCOMPARE(segments[0].address, 0x08000000u);
        QCOMPARE(segments[0].data, QByteArray("firmware!"));
    }

    void normalizedMergesAndSorts()
    {
        const auto result = FirmwareImage::normalized({{0x100, "cd"}, {0xFE, "ab"}, {0x300, ""}, {0x200, "x"}});
        QCOMPARE(result.size(), 2);
        QCOMPARE(result[0].address, 0xFEu);
        QCOMPARE(result[0].data, QByteArray("abcd"));
        QCOMPARE(result[1].address, 0x200u);
    }
};

QTEST_APPLESS_MAIN(TestFlmFile)
#include "test_flmfile.moc"
