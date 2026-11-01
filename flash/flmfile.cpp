#include "flash/flmfile.h"

#include "flash/elf32.h"

#include <QObject>
#include <QFile>
#include <QtEndian>

#include <algorithm>

namespace {

constexpr quint32 kMaxImageBytes = 64 * 1024;

// struct FlashDevice 的字段偏移（Vers u16 / DevName[128] / DevType u16 / 之后都是 32 位对齐）
constexpr quint32 kDevNameOffset = 2;
constexpr quint32 kDevNameSize = 128;
constexpr quint32 kDevAdrOffset = 132;
constexpr quint32 kSzDevOffset = 136;
constexpr quint32 kSzPageOffset = 140;
constexpr quint32 kValEmptyOffset = 148;
constexpr quint32 kToProgOffset = 152;
constexpr quint32 kToEraseOffset = 156;
constexpr quint32 kSectorsOffset = 160;
constexpr quint32 kSectorEnd = 0xFFFFFFFFu;

quint32 readU32(const QByteArray &data, quint32 offset)
{
    return qFromLittleEndian<quint32>(data.constData() + offset);
}

} // namespace

bool FlmFile::load(const QString &path, FlmFile *out, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QObject::tr("Cannot open %1: %2").arg(path, file.errorString());
        return false;
    }
    return parse(file.readAll(), out, error);
}

bool FlmFile::parse(const QByteArray &bytes, FlmFile *out, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    Elf32 elf;
    QString elfError;
    if (!Elf32::parse(bytes, &elf, &elfError))
        return fail(elfError);

    FlmFile flm;

    // 镜像：优先用 PT_LOAD 段；没有程序头时退回到带 ALLOC 标志的节
    struct Piece { quint32 address; quint32 fileOffset; quint32 fileSize; quint32 memSize; };
    QVector<Piece> pieces;
    for (const ElfSegment &seg : elf.segments) {
        if (seg.type == ElfSegment::kTypeLoad && seg.memorySize > 0)
            pieces.append({seg.virtualAddress, seg.offset, seg.fileSize, seg.memorySize});
    }
    if (pieces.isEmpty()) {
        for (const ElfSection &sec : elf.sections) {
            if (!(sec.flags & ElfSection::kFlagAlloc) || sec.size == 0)
                continue;
            const quint32 fileSize = sec.type == ElfSection::kTypeNoBits ? 0 : sec.size;
            pieces.append({sec.address, sec.offset, fileSize, sec.size});
        }
    }
    // DevDscr（FlashDevice 描述和扇区表，可能有几 KB）只是给上位机读的，不下载到 RAM
    if (const ElfSection *devDscr = elf.findSection(QStringLiteral("DevDscr"))) {
        pieces.erase(std::remove_if(pieces.begin(), pieces.end(),
                                    [devDscr](const Piece &piece) { return piece.address >= devDscr->address; }),
                     pieces.end());
    }
    if (pieces.isEmpty())
        return fail(QObject::tr("FLM file has no loadable code"));

    quint32 extent = 0;
    for (const Piece &piece : pieces) {
        if (quint64(piece.address) + piece.memSize > kMaxImageBytes)
            return fail(QObject::tr("FLM algorithm is too large or not linked at address 0"));
        extent = std::max(extent, piece.address + piece.memSize);
    }
    flm.image = QByteArray(qsizetype(extent), '\0');
    for (const Piece &piece : pieces) {
        if (piece.fileSize > 0)
            flm.image.replace(qsizetype(piece.address), qsizetype(piece.fileSize),
                              elf.data.constData() + piece.fileOffset, qsizetype(piece.fileSize));
    }

    // R9：PrgData 起始；没有这个节时放在镜像末尾（算法没有静态数据）
    const ElfSection *prgData = elf.findSection(QStringLiteral("PrgData"));
    flm.staticBase = prgData ? prgData->address : extent;

    auto entry = [&](const char *name, bool required, int *target) {
        const ElfSymbol *symbol = elf.findSymbol(QLatin1String(name));
        if (!symbol) {
            if (required)
                return false;
            return true;
        }
        const quint32 offset = symbol->value & ~1u; // 去掉 Thumb 位
        if (offset >= extent)
            return false;
        *target = int(offset);
        return true;
    };
    if (!entry("Init", true, &flm.initOffset) || !entry("UnInit", true, &flm.uninitOffset)
        || !entry("EraseSector", true, &flm.eraseSectorOffset)
        || !entry("ProgramPage", true, &flm.programPageOffset))
        return fail(QObject::tr("FLM file is missing required functions "
                                "(Init, UnInit, EraseSector, ProgramPage)"));
    entry("EraseChip", false, &flm.eraseChipOffset);

    // FlashDevice 描述符
    const ElfSymbol *device = elf.findSymbol(QStringLiteral("FlashDevice"));
    if (!device || device->sectionIndex >= elf.sections.size())
        return fail(QObject::tr("FLM file has no FlashDevice description"));
    const ElfSection &devSection = elf.sections[device->sectionIndex];
    if (device->value < devSection.address)
        return fail(QObject::tr("FLM FlashDevice description is invalid"));
    const quint32 devFileOffset = devSection.offset + (device->value - devSection.address);
    if (quint64(devFileOffset) + kSectorsOffset + 8 > quint64(elf.data.size()))
        return fail(QObject::tr("FLM FlashDevice description is truncated"));

    const QByteArray &d = elf.data;
    const char *base = d.constData() + devFileOffset;
    flm.deviceName = QString::fromLatin1(QByteArray(base + kDevNameOffset, kDevNameSize).constData());
    flm.deviceAddress = readU32(d, devFileOffset + kDevAdrOffset);
    flm.deviceSize = readU32(d, devFileOffset + kSzDevOffset);
    flm.pageSize = readU32(d, devFileOffset + kSzPageOffset);
    flm.emptyValue = quint8(base[kValEmptyOffset]);
    flm.programTimeoutMs = readU32(d, devFileOffset + kToProgOffset);
    flm.eraseTimeoutMs = readU32(d, devFileOffset + kToEraseOffset);

    for (quint32 pos = devFileOffset + kSectorsOffset; quint64(pos) + 8 <= quint64(d.size()); pos += 8) {
        const quint32 size = readU32(d, pos);
        const quint32 offset = readU32(d, pos + 4);
        if (size == kSectorEnd && offset == kSectorEnd)
            break;
        flm.sectors.append({size, offset});
    }

    if (flm.deviceSize == 0 || flm.pageSize == 0 || flm.sectors.isEmpty())
        return fail(QObject::tr("FLM FlashDevice description is invalid"));
    for (const FlmSector &sector : std::as_const(flm.sectors)) {
        if (sector.size == 0)
            return fail(QObject::tr("FLM FlashDevice description is invalid"));
    }

    *out = flm;
    return true;
}

bool FlmFile::contains(quint32 address, quint32 length) const
{
    const quint64 start = address;
    const quint64 end = start + length;
    return start >= deviceAddress && end <= quint64(deviceAddress) + deviceSize;
}

QVector<quint32> FlmFile::sectorsCovering(quint64 start, quint64 end) const
{
    QVector<quint32> result;
    const quint64 deviceStart = deviceAddress;
    if (start >= end || start < deviceStart || end > deviceStart + deviceSize)
        return result;

    const quint64 deviceEnd = quint64(deviceAddress) + deviceSize;
    for (int i = 0; i < sectors.size(); ++i) {
        const quint64 regionStart = quint64(deviceAddress) + sectors[i].offset;
        const quint64 regionEnd = i + 1 < sectors.size()
                ? quint64(deviceAddress) + sectors[i + 1].offset
                : deviceEnd;
        if (regionEnd <= start || regionStart >= end)
            continue;
        const quint64 size = sectors[i].size;
        quint64 first = regionStart;
        if (start > regionStart)
            first += (start - regionStart) / size * size; // 落在区域中间时对齐到所在扇区
        for (quint64 addr = first; addr < regionEnd && addr < end; addr += size)
            result.append(quint32(addr));
    }
    return result;
}
