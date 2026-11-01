#include "flash/elf32.h"

#include <QObject>
#include <QtEndian>

namespace {

constexpr int kHeaderSize = 52;
constexpr quint16 kMachineArm = 40;
constexpr quint32 kSectionSymtab = 2;

// 带边界检查的小端读取
class Reader
{
public:
    explicit Reader(const QByteArray &data) : m_data(data) {}

    bool has(quint64 offset, quint64 length) const
    {
        return offset + length <= quint64(m_data.size());
    }
    quint16 u16(quint64 offset) const
    {
        return qFromLittleEndian<quint16>(m_data.constData() + offset);
    }
    quint32 u32(quint64 offset) const
    {
        return qFromLittleEndian<quint32>(m_data.constData() + offset);
    }
    QString cString(quint64 offset, quint64 limitEnd) const
    {
        QByteArray text;
        for (quint64 pos = offset; pos < limitEnd && pos < quint64(m_data.size()); ++pos) {
            if (m_data[qsizetype(pos)] == '\0')
                break;
            text.append(m_data[qsizetype(pos)]);
        }
        return QString::fromLatin1(text);
    }

private:
    const QByteArray &m_data;
};

} // namespace

bool Elf32::parse(const QByteArray &bytes, Elf32 *out, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    Elf32 elf;
    elf.data = bytes;
    const Reader r(elf.data);

    if (!r.has(0, kHeaderSize) || bytes.left(4) != QByteArray("\x7f" "ELF", 4))
        return fail(QObject::tr("Not an ELF file"));
    if (bytes[4] != 1 || bytes[5] != 1)
        return fail(QObject::tr("Only 32-bit little-endian ELF is supported"));
    if (r.u16(18) != kMachineArm)
        return fail(QObject::tr("ELF file is not for ARM"));

    const quint32 phOffset = r.u32(28);
    const quint32 shOffset = r.u32(32);
    const quint16 phEntSize = r.u16(42);
    const quint16 phNum = r.u16(44);
    const quint16 shEntSize = r.u16(46);
    const quint16 shNum = r.u16(48);
    const quint16 shStrIndex = r.u16(50);

    if (phNum > 0) {
        if (phEntSize < 32 || !r.has(phOffset, quint64(phEntSize) * phNum))
            return fail(QObject::tr("ELF program header table is truncated"));
        for (int i = 0; i < phNum; ++i) {
            const quint64 base = phOffset + quint64(i) * phEntSize;
            ElfSegment seg;
            seg.type = r.u32(base);
            seg.offset = r.u32(base + 4);
            seg.virtualAddress = r.u32(base + 8);
            seg.physicalAddress = r.u32(base + 12);
            seg.fileSize = r.u32(base + 16);
            seg.memorySize = r.u32(base + 20);
            if (seg.fileSize > 0 && !r.has(seg.offset, seg.fileSize))
                return fail(QObject::tr("ELF segment is outside the file"));
            elf.segments.append(seg);
        }
    }

    if (shNum > 0) {
        if (shEntSize < 40 || !r.has(shOffset, quint64(shEntSize) * shNum))
            return fail(QObject::tr("ELF section header table is truncated"));

        QVector<quint32> nameOffsets;
        for (int i = 0; i < shNum; ++i) {
            const quint64 base = shOffset + quint64(i) * shEntSize;
            ElfSection sec;
            nameOffsets.append(r.u32(base));
            sec.type = r.u32(base + 4);
            sec.flags = r.u32(base + 8);
            sec.address = r.u32(base + 12);
            sec.offset = r.u32(base + 16);
            sec.size = r.u32(base + 20);
            if (sec.type != ElfSection::kTypeNoBits && sec.size > 0 && !r.has(sec.offset, sec.size))
                return fail(QObject::tr("ELF section is outside the file"));
            elf.sections.append(sec);
        }

        if (shStrIndex < shNum) {
            const ElfSection &strtab = elf.sections[shStrIndex];
            for (int i = 0; i < shNum; ++i)
                elf.sections[i].name = r.cString(quint64(strtab.offset) + nameOffsets[i],
                                                 quint64(strtab.offset) + strtab.size);
        }

        for (int i = 0; i < shNum; ++i) {
            if (elf.sections[i].type != kSectionSymtab)
                continue;
            const quint64 base = shOffset + quint64(i) * shEntSize;
            const quint32 link = r.u32(base + 24);
            if (link >= quint32(shNum))
                break;
            const ElfSection &symtab = elf.sections[i];
            const ElfSection &strtab = elf.sections[int(link)];
            for (quint32 pos = 0; pos + 16 <= symtab.size; pos += 16) {
                const quint64 sym = quint64(symtab.offset) + pos;
                ElfSymbol symbol;
                symbol.name = r.cString(quint64(strtab.offset) + r.u32(sym),
                                        quint64(strtab.offset) + strtab.size);
                symbol.value = r.u32(sym + 4);
                symbol.size = r.u32(sym + 8);
                symbol.sectionIndex = r.u16(sym + 14);
                if (!symbol.name.isEmpty())
                    elf.symbols.append(symbol);
            }
            break;
        }
    }

    *out = elf;
    return true;
}

const ElfSection *Elf32::findSection(const QString &name) const
{
    for (const ElfSection &section : sections) {
        if (section.name == name)
            return &section;
    }
    return nullptr;
}

const ElfSymbol *Elf32::findSymbol(const QString &name) const
{
    for (const ElfSymbol &symbol : symbols) {
        if (symbol.name == name)
            return &symbol;
    }
    return nullptr;
}
