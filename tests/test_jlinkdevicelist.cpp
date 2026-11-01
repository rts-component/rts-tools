#include "common/jlinkdevicelist.h"

#include <QtTest>

class TestJLinkDeviceList : public QObject
{
    Q_OBJECT

private slots:
    void parsesRamOfSingleAreas()
    {
        QString device;
        JLinkDeviceList::RamArea ram;
        QVERIFY(JLinkDeviceList::parseLine(
                QStringLiteral("\"ST\", \"STM32F103C8\", \"Cortex-M3\", {0x08000000, 0x00010000}, "
                               "{0x20000000, 0x00005000}"),
                &device, &ram));
        QCOMPARE(device, QStringLiteral("STM32F103C8"));
        QVERIFY(ram.isValid());
        QCOMPARE(ram.address, 0x20000000u);
        QCOMPARE(ram.size, 0x5000u);
    }

    void parsesMultiAreaFlashAndRam()
    {
        QString device;
        JLinkDeviceList::RamArea ram;
        // Flash 有两段；RAM 有两段时取第一个非空的
        QVERIFY(JLinkDeviceList::parseLine(
                QStringLiteral("\"ST\", \"STM32F407VG\", \"Cortex-M4\", { {0x08000000, 0x00100000}, "
                               "{0x1FFF7800, 0x00000210} }, { {0x00000000, 0x00000000}, {0x20000000, 0x00020000} }"),
                &device, &ram));
        QCOMPARE(device, QStringLiteral("STM32F407VG"));
        QCOMPARE(ram.address, 0x20000000u);
        QCOMPARE(ram.size, 0x20000u);
    }

    void genericCoreHasNoRam()
    {
        QString device;
        JLinkDeviceList::RamArea ram;
        QVERIFY(JLinkDeviceList::parseLine(
                QStringLiteral("\"Unspecified\", \"Cortex-M0+\", \"Cortex-M0\", {0x00000000, 0x00000000}, "
                               "{0x00000000, 0x00000000}"),
                &device, &ram));
        QCOMPARE(device, QStringLiteral("Cortex-M0+"));
        QVERIFY(!ram.isValid());
    }

    void skipsHeaderAndGarbage()
    {
        QString device;
        JLinkDeviceList::RamArea ram;
        QVERIFY(!JLinkDeviceList::parseLine(
                QStringLiteral("\"Manufacturer\", \"Device\", \"Core\", {Flash areas}, {RAM areas}"), &device, &ram));
        QVERIFY(!JLinkDeviceList::parseLine(QStringLiteral("garbage"), &device, &ram));
        QVERIFY(!JLinkDeviceList::parseLine(QString(), &device, &ram));
    }
};

QTEST_APPLESS_MAIN(TestJLinkDeviceList)
#include "test_jlinkdevicelist.moc"
