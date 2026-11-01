#include "flash/flashalgo.h"

#include <QObject>
#include <QMap>
#include <QSet>

#include <algorithm>

namespace {

constexpr quint32 kHeaderSize = 8;       // 两组 "BKPT; B ." 作为函数的返回落点
constexpr quint32 kStackSize = 0x400;
constexpr int kInitTimeoutMs = 5000;
constexpr int kMinTimeoutMs = 1000;
constexpr int kChipEraseTimeoutMs = 120000;
constexpr int kVerifyBlock = 4096;
constexpr quint32 kThumbState = 0x01000000; // xPSR 的 T 位：Cortex-M 只能跑 Thumb

quint32 alignUp(quint32 value, quint32 alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

QString hex(quint32 value)
{
    return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0'));
}

} // namespace

FlashAlgo::FlashAlgo(Probe &probe, const FlmFile &flm, quint32 ramAddress, quint32 ramSize,
                     const FlashCallbacks &callbacks)
    : m_probe(probe)
    , m_flm(flm)
    , m_ramAddress(ramAddress)
    , m_ramSize(ramSize)
    , m_callbacks(callbacks)
{
}

void FlashAlgo::log(const QString &text) const
{
    if (m_callbacks.log)
        m_callbacks.log(text);
}

void FlashAlgo::reportProgress(int done, int total) const
{
    if (m_callbacks.progress && total > 0)
        m_callbacks.progress(int(qint64(done) * 100 / total));
}

bool FlashAlgo::isCancelled() const
{
    return m_callbacks.cancelled && m_callbacks.cancelled();
}

bool FlashAlgo::fail(const QString &message)
{
    m_error = message;
    return false;
}

bool FlashAlgo::load()
{
    m_codeAddress = m_ramAddress + kHeaderSize;
    const quint32 imageEnd = m_codeAddress + alignUp(quint32(m_flm.image.size()), 4);
    const quint32 stackStart = alignUp(imageEnd, 8);
    m_stackTop = stackStart + kStackSize;
    m_bufferAddress = m_stackTop;
    m_staticBase = m_codeAddress + m_flm.staticBase;

    const quint64 needed = quint64(m_bufferAddress) + m_flm.pageSize - m_ramAddress;
    // ramSize 为 0 表示不限制：FLM 不带 RAM 信息，按算法实际需要的大小使用
    if (m_ramSize != 0 && needed > m_ramSize)
        return fail(QObject::tr("RAM is too small: the algorithm needs %1 bytes (code + stack + page "
                                "buffer) but only %2 bytes are configured")
                            .arg(needed).arg(m_ramSize));

    QByteArray block;
    block.append("\x00\xBE\xFE\xE7\x00\xBE\xFE\xE7", int(kHeaderSize)); // BKPT #0; B .
    block.append(m_flm.image);
    log(QObject::tr("Downloading flash algorithm to RAM at %1 (%2 bytes)")
                .arg(hex(m_ramAddress)).arg(block.size()));
    if (!m_probe.halt() || !m_probe.writeMemory(m_ramAddress, block))
        return fail(m_probe.lastError());

    QByteArray readBack;
    if (!m_probe.readMemory(m_ramAddress, int(block.size()), &readBack) || readBack != block)
        return fail(QObject::tr("The flash algorithm in target RAM does not read back correctly; "
                                "check the RAM address/size"));
    return true;
}

bool FlashAlgo::call(int entryOffset, const QVector<quint32> &args, int timeoutMs, quint32 *result)
{
    const quint32 entry = (m_codeAddress + quint32(entryOffset)) & ~1u;
    // 只写这个函数用到的参数寄存器：每写一个都是一次和探针的往返，调用又非常频繁（每页一次）
    QVector<QPair<int, quint32>> registers;
    for (int i = 0; i < args.size() && i < 4; ++i)
        registers.append({Probe::R0 + i, args[i]});
    registers.append({{Probe::R9, m_staticBase},
                      {Probe::SP, m_stackTop},
                      {Probe::LR, m_ramAddress | 1u},
                      {Probe::PC, entry},
                      {Probe::XPSR, kThumbState}});
    const bool ok = m_probe.writeRegisters(registers);
    if (!ok || !m_probe.resume())
        return fail(m_probe.lastError());
    if (!m_probe.waitHalted(timeoutMs))
        return fail(m_probe.lastError());

    QVector<quint32> values;
    if (!m_probe.readRegisters({Probe::PC, Probe::R0}, &values) || values.size() != 2)
        return fail(m_probe.lastError());
    const quint32 pc = values[0];
    *result = values[1];
    if ((pc & ~1u) != m_ramAddress && (pc & ~1u) != m_ramAddress + 4)
        return fail(QObject::tr("The flash algorithm stopped abnormally at PC=%1").arg(hex(pc)));
    return true;
}

bool FlashAlgo::initAlgo(Function function)
{
    quint32 result = 0;
    if (!call(m_flm.initOffset, {m_flm.deviceAddress, 0, quint32(function)}, kInitTimeoutMs, &result))
        return false;
    if (result != 0)
        return fail(QObject::tr("Flash algorithm Init failed (code %1)").arg(result));
    return true;
}

bool FlashAlgo::uninitAlgo(Function function)
{
    quint32 result = 0;
    if (!call(m_flm.uninitOffset, {quint32(function)}, kInitTimeoutMs, &result))
        return false;
    if (result != 0)
        return fail(QObject::tr("Flash algorithm UnInit failed (code %1)").arg(result));
    return true;
}

bool FlashAlgo::eraseSectors(const QVector<quint32> &sectors, int *done, int total)
{
    const int timeout = int(qMax<quint32>(m_flm.eraseTimeoutMs, kMinTimeoutMs));
    for (quint32 sector : sectors) {
        if (isCancelled())
            return fail(QObject::tr("Cancelled"));
        quint32 result = 0;
        if (!call(m_flm.eraseSectorOffset, {sector}, timeout, &result))
            return false;
        if (result != 0)
            return fail(QObject::tr("Erase failed at %1 (code %2)").arg(hex(sector)).arg(result));
        reportProgress(++*done, total);
    }
    return true;
}

bool FlashAlgo::program(const QVector<FirmwareSegment> &segments)
{
    const QVector<FirmwareSegment> image = FirmwareImage::normalized(segments);

    // 固件落到的扇区和页
    QSet<quint32> sectorSet;
    QMap<quint32, QByteArray> pages;
    for (const FirmwareSegment &segment : image) {
        const quint32 length = quint32(segment.data.size());
        if (!m_flm.contains(segment.address, length))
            return fail(QObject::tr("Firmware range %1 + %2 bytes is outside the flash device "
                                    "(%3 – %4)")
                                .arg(hex(segment.address)).arg(length)
                                .arg(hex(m_flm.deviceAddress))
                                .arg(hex(m_flm.deviceAddress + m_flm.deviceSize - 1)));
        const auto covering = m_flm.sectorsCovering(segment.address, quint64(segment.address) + length);
        for (quint32 sector : covering)
            sectorSet.insert(sector);

        quint32 address = segment.address;
        qsizetype pos = 0;
        while (pos < segment.data.size()) {
            const quint32 pageStart = m_flm.deviceAddress
                    + (address - m_flm.deviceAddress) / m_flm.pageSize * m_flm.pageSize;
            QByteArray &page = pages[pageStart];
            if (page.isEmpty())
                page = QByteArray(qsizetype(m_flm.pageSize), char(m_flm.emptyValue));
            const quint32 inPage = address - pageStart;
            const qsizetype count = qMin<qsizetype>(qsizetype(m_flm.pageSize - inPage),
                                                    segment.data.size() - pos);
            page.replace(qsizetype(inPage), count, segment.data.constData() + pos, count);
            address += quint32(count);
            pos += count;
        }
    }

    // 全空的页擦除后已经是空的，不用再写
    for (auto it = pages.begin(); it != pages.end();) {
        const QByteArray empty(it.value().size(), char(m_flm.emptyValue));
        it = it.value() == empty ? pages.erase(it) : std::next(it);
    }

    QVector<quint32> sectors(sectorSet.begin(), sectorSet.end());
    std::sort(sectors.begin(), sectors.end());
    const int total = int(sectors.size() + pages.size());
    int done = 0;

    log(QObject::tr("Erasing %1 sector(s)").arg(sectors.size()));
    if (!initAlgo(Erase) || !eraseSectors(sectors, &done, total) || !uninitAlgo(Erase))
        return false;

    log(QObject::tr("Programming %1 page(s) of %2 bytes").arg(pages.size()).arg(m_flm.pageSize));
    if (!initAlgo(Program))
        return false;
    const int timeout = int(qMax<quint32>(m_flm.programTimeoutMs, kMinTimeoutMs));
    for (auto it = pages.cbegin(); it != pages.cend(); ++it) {
        if (isCancelled())
            return fail(QObject::tr("Cancelled"));
        if (!m_probe.writeMemory(m_bufferAddress, it.value()))
            return fail(m_probe.lastError());
        quint32 result = 0;
        if (!call(m_flm.programPageOffset, {it.key(), m_flm.pageSize, m_bufferAddress}, timeout, &result))
            return false;
        if (result != 0)
            return fail(QObject::tr("Programming failed at %1 (code %2)").arg(hex(it.key())).arg(result));
        reportProgress(++done, total);
    }
    return uninitAlgo(Program);
}

bool FlashAlgo::eraseChip()
{
    if (!initAlgo(Erase))
        return false;

    quint32 result = 0;
    if (m_flm.eraseChipOffset != FlmFile::kNoEntry) {
        log(QObject::tr("Erasing the whole chip"));
        if (!call(m_flm.eraseChipOffset, {}, kChipEraseTimeoutMs, &result))
            return false;
        if (result != 0)
            return fail(QObject::tr("Chip erase failed (code %1)").arg(result));
        reportProgress(1, 1);
    } else {
        const QVector<quint32> sectors =
                m_flm.sectorsCovering(m_flm.deviceAddress, quint64(m_flm.deviceAddress) + m_flm.deviceSize);
        log(QObject::tr("The algorithm has no EraseChip; erasing %1 sector(s)").arg(sectors.size()));
        int done = 0;
        if (!eraseSectors(sectors, &done, int(sectors.size())))
            return false;
    }
    return uninitAlgo(Erase);
}

bool FlashAlgo::verify(Probe &probe, const QVector<FirmwareSegment> &segments,
                       const FlashCallbacks &callbacks, QString *error)
{
    const quint64 total = FirmwareImage::totalSize(segments);
    quint64 done = 0;
    for (const FirmwareSegment &segment : segments) {
        for (qsizetype pos = 0; pos < segment.data.size(); pos += kVerifyBlock) {
            if (callbacks.cancelled && callbacks.cancelled()) {
                *error = QObject::tr("Cancelled");
                return false;
            }
            const int count = int(qMin<qsizetype>(kVerifyBlock, segment.data.size() - pos));
            const quint32 address = segment.address + quint32(pos);
            QByteArray actual;
            if (!probe.readMemory(address, count, &actual)) {
                *error = probe.lastError();
                return false;
            }
            const QByteArray expected = segment.data.mid(pos, count);
            if (actual != expected) {
                int index = 0;
                while (index < count && actual[index] == expected[index])
                    ++index;
                *error = QObject::tr("Verify failed at %1: target has 0x%2, expected 0x%3")
                                 .arg(hex(address + quint32(index)))
                                 .arg(quint8(actual[index]), 2, 16, QLatin1Char('0'))
                                 .arg(quint8(expected[index]), 2, 16, QLatin1Char('0'));
                return false;
            }
            done += quint64(count);
            if (callbacks.progress && total > 0)
                callbacks.progress(int(done * 100 / total));
        }
    }
    return true;
}
