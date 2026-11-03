#include "flash/flashalgo.h"
#include "flash/flmfile.h"
#include "tests/elfbuilder.h"

#include <QtTest>

namespace {

constexpr quint32 kRam = 0x20000000;
constexpr quint32 kRamSize = 0x4000;
constexpr quint32 kFlash = 0x08000000;
constexpr quint32 kFlashSize = 0x10000;

// 假探针：内存 + 寄存器 + 一个"执行"函数——resume() 时按 PC 模拟 FLM 里各函数的行为
class FakeProbe : public Probe
{
public:
    FakeProbe()
        : ram(kRamSize, '\0')
        , flash(kFlashSize, char(0xFF))
    {
    }

    bool reset() override { return true; }
    bool halt() override { return true; }
    bool resume() override
    {
        const quint32 pc = regs[PC];
        const quint32 code = kRam + 8;
        calls.append(QString::number(pc - code, 16));
        // 调用约定有任何一项不对就记下来，由测试用例统一断言
        if (regs[LR] != (kRam | 1u))
            violations.append(QStringLiteral("LR"));
        if (regs[R9] != code + ElfBuilder::FlmLayout::staticBase)
            violations.append(QStringLiteral("R9"));
        if (regs[SP] % 8 != 0)
            violations.append(QStringLiteral("SP alignment"));
        if (regs[XPSR] != 0x01000000u)
            violations.append(QStringLiteral("xPSR"));

        quint32 result = 0;
        if (failProgramAt && pc == code + ElfBuilder::FlmLayout::programPage && regs[R0] == failProgramAt) {
            result = 1;
        } else if (pc == code + ElfBuilder::FlmLayout::eraseSector) {
            const quint32 offset = regs[R0] - kFlash;
            flash.replace(int(offset), 0x400, QByteArray(0x400, char(0xFF)));
            erasedSectors.append(regs[R0]);
        } else if (pc == code + ElfBuilder::FlmLayout::eraseChip) {
            flash.fill(char(0xFF));
            chipErased = true;
        } else if (pc == code + ElfBuilder::FlmLayout::programPage) {
            const quint32 offset = regs[R0] - kFlash;
            const QByteArray page = ram.mid(int(regs[R2] - kRam), int(regs[R1]));
            for (int i = 0; i < page.size(); ++i) // Flash 只能把 1 写成 0
                flash[int(offset) + i] = char(flash[int(offset) + i] & page[i]);
            programmedPages.append(regs[R0]);
        } else if (pc != code + ElfBuilder::FlmLayout::init && pc != code + ElfBuilder::FlmLayout::uninit) {
            return false;
        }
        regs[R0] = result;
        regs[PC] = kRam; // 停在返回落点的 BKPT 上
        return true;
    }
    bool waitHalted(int) override { return true; }

    bool readMemory(quint32 address, int length, QByteArray *out) override
    {
        if (address >= kFlash && address + length <= kFlash + kFlashSize)
            *out = flash.mid(int(address - kFlash), length);
        else if (address >= kRam && address + length <= kRam + kRamSize)
            *out = ram.mid(int(address - kRam), length);
        else
            return setError(QStringLiteral("bad read"));
        return true;
    }
    bool writeMemory(quint32 address, const QByteArray &data) override
    {
        if (address < kRam || address + data.size() > kRam + kRamSize)
            return setError(QStringLiteral("bad write"));
        ram.replace(int(address - kRam), int(data.size()), data);
        return true;
    }
    bool readRegister(int index, quint32 *value) override
    {
        *value = regs[index];
        return true;
    }
    bool writeRegister(int index, quint32 value) override
    {
        regs[index] = value;
        return true;
    }

    quint32 regs[32] = {};
    QByteArray ram;
    QByteArray flash;
    QStringList calls;
    QStringList violations;
    QVector<quint32> erasedSectors;
    QVector<quint32> programmedPages;
    bool chipErased = false;
    quint32 failProgramAt = 0;
};

QByteArray pattern(int size)
{
    QByteArray data(size, '\0');
    for (int i = 0; i < size; ++i)
        data[i] = char(0x10 + i % 0xE0);
    return data;
}

} // namespace

class TestFlashAlgo : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QString error;
        QVERIFY2(FlmFile::parse(ElfBuilder::buildTestFlm(), &m_flm, &error), qPrintable(error));
    }

    void programErasesThenWritesPages()
    {
        FlmFile flm; // 没有 EraseChip 的算法：只擦固件覆盖的扇区
        QVERIFY(FlmFile::parse(ElfBuilder::buildTestFlm(false), &flm, nullptr));
        FakeProbe probe;
        probe.flash.fill('\0'); // 先全部置 0：不擦除就写不进去，能证明擦除生效
        FlashAlgo algo(probe, flm, kRam, kRamSize, {});
        QVERIFY(algo.load());

        const QByteArray data = pattern(0x300);
        QVERIFY2(algo.program({{kFlash + 0x100, data}}), qPrintable(algo.error()));

        QCOMPARE(probe.erasedSectors, QVector<quint32>({kFlash}));
        QCOMPARE(probe.programmedPages, QVector<quint32>({kFlash + 0x100, kFlash + 0x200, kFlash + 0x300}));
        QCOMPARE(probe.flash.mid(0x100, 0x300), data);
        QCOMPARE(probe.flash.left(0x100), QByteArray(0x100, char(0xFF))); // 同扇区其余部分被擦成了空
        QCOMPARE(probe.flash.at(0x400), '\0');                            // 扇区外不受影响

        // 调用顺序：Init → EraseSector → UnInit → Init → ProgramPage… → UnInit
        QCOMPARE(probe.calls.first(), QStringLiteral("10"));
        QCOMPARE(probe.calls.last(), QStringLiteral("20"));
        QCOMPARE(probe.calls.size(), 1 + 1 + 1 + 1 + 3 + 1); // Init, Erase, UnInit, Init, 3×Program, UnInit
        QVERIFY2(probe.violations.isEmpty(), qPrintable(probe.violations.join(QLatin1Char(','))));
    }

    void partialPagesAreFilledWithEmptyValue()
    {
        FakeProbe probe;
        FlashAlgo algo(probe, m_flm, kRam, kRamSize, {});
        QVERIFY(algo.load());
        // 从页中间开始、到页中间结束
        const QByteArray data = pattern(0x20);
        QVERIFY2(algo.program({{kFlash + 0x1F0, data}}), qPrintable(algo.error()));
        QCOMPARE(probe.programmedPages, QVector<quint32>({kFlash + 0x100, kFlash + 0x200}));
        QCOMPARE(probe.flash.mid(0x1F0, 0x20), data);
        QCOMPARE(probe.flash.at(0x1EF), char(0xFF));
    }

    void skipsEmptyPages()
    {
        FakeProbe probe;
        FlashAlgo algo(probe, m_flm, kRam, kRamSize, {});
        QVERIFY(algo.load());
        QByteArray data = QByteArray(0x200, char(0xFF));
        data[0x1FF] = 1; // 只有第二页有内容
        QVERIFY(algo.program({{kFlash, data}}));
        QCOMPARE(probe.programmedPages, QVector<quint32>({kFlash + 0x100}));
    }

    void rejectsFirmwareOutsideDevice()
    {
        FakeProbe probe;
        FlashAlgo algo(probe, m_flm, kRam, kRamSize, {});
        QVERIFY(algo.load());
        QVERIFY(!algo.program({{kFlash + kFlashSize - 4, pattern(8)}}));
        QVERIFY(algo.error().contains(QStringLiteral("outside")));
        QVERIFY(probe.calls.isEmpty());
    }

    void reportsProgramFailure()
    {
        FakeProbe probe;
        probe.failProgramAt = kFlash + 0x200;
        FlashAlgo algo(probe, m_flm, kRam, kRamSize, {});
        QVERIFY(algo.load());
        QVERIFY(!algo.program({{kFlash + 0x100, pattern(0x300)}}));
        QVERIFY(algo.error().contains(QStringLiteral("0x08000200")));
    }

    void ramTooSmall()
    {
        FakeProbe probe;
        FlashAlgo algo(probe, m_flm, kRam, 0x400, {});
        QVERIFY(!algo.load());
        QVERIFY(algo.error().contains(QStringLiteral("RAM")));
    }

    void programUsesChipEraseWhenAvailable()
    {
        FakeProbe probe;
        probe.flash.fill('\0');
        FlashAlgo algo(probe, m_flm, kRam, kRamSize, {});
        QVERIFY(algo.load());
        const QByteArray data = pattern(0x300);
        QVERIFY2(algo.program({{kFlash + 0x100, data}}), qPrintable(algo.error()));
        QVERIFY(probe.chipErased);
        QVERIFY(probe.erasedSectors.isEmpty());
        QCOMPARE(probe.flash.mid(0x100, 0x300), data);
        QCOMPARE(probe.flash.at(0x400), char(0xFF)); // 整片擦除：固件之外也被擦了
    }

    void eraseChipUsesAlgorithmFunction()
    {
        FakeProbe probe;
        probe.flash.fill('\0');
        FlashAlgo algo(probe, m_flm, kRam, kRamSize, {});
        QVERIFY(algo.load());
        QVERIFY2(algo.eraseChip(), qPrintable(algo.error()));
        QVERIFY(probe.chipErased);
        QCOMPARE(probe.flash, QByteArray(kFlashSize, char(0xFF)));
    }

    void eraseChipFallsBackToSectors()
    {
        FlmFile flm;
        QVERIFY(FlmFile::parse(ElfBuilder::buildTestFlm(false), &flm, nullptr));
        FakeProbe probe;
        FlashAlgo algo(probe, flm, kRam, kRamSize, {});
        QVERIFY(algo.load());
        QVERIFY2(algo.eraseChip(), qPrintable(algo.error()));
        QVERIFY(!probe.chipErased);
        QCOMPARE(probe.erasedSectors.size(), int(kFlashSize / 0x400));
    }

    void cancelStopsBetweenSteps()
    {
        FakeProbe probe;
        FlashCallbacks callbacks;
        callbacks.cancelled = [] { return true; };
        FlashAlgo algo(probe, m_flm, kRam, kRamSize, callbacks);
        QVERIFY(algo.load());
        QVERIFY(!algo.program({{kFlash, pattern(0x100)}}));
        QVERIFY(algo.error().contains(QStringLiteral("Cancelled")));
        QVERIFY(probe.programmedPages.isEmpty());
    }

    void progressReachesHundred()
    {
        FakeProbe probe;
        int last = -1;
        FlashCallbacks callbacks;
        callbacks.progress = [&last](int percent) { last = percent; };
        FlashAlgo algo(probe, m_flm, kRam, kRamSize, callbacks);
        QVERIFY(algo.load());
        QVERIFY(algo.program({{kFlash, pattern(0x500)}}));
        QCOMPARE(last, 100);
    }

    void verifyReportsFirstMismatch()
    {
        FakeProbe probe;
        const QByteArray data = pattern(0x20);
        probe.flash.replace(0x10, 0x20, data);
        QString error;
        QVERIFY2(FlashAlgo::verify(probe, {{kFlash + 0x10, data}}, {}, &error), qPrintable(error));

        probe.flash[0x15] = char(data[5] ^ 0xFF);
        QVERIFY(!FlashAlgo::verify(probe, {{kFlash + 0x10, data}}, {}, &error));
        QVERIFY(error.contains(QStringLiteral("0x08000015")));
    }

private:
    FlmFile m_flm;
};

QTEST_APPLESS_MAIN(TestFlashAlgo)
#include "test_flashalgo.moc"
