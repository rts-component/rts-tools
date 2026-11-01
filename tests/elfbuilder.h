#ifndef ELFBUILDER_H
#define ELFBUILDER_H

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtEndian>

// 测试用：拼一个最小的 ELF32 小端 ARM 文件（可带符号表和 PT_LOAD 段）
namespace ElfBuilder {

struct Section
{
    QString name;
    quint32 type = 1; // PROGBITS
    quint32 flags = 2;
    quint32 address = 0;
    QByteArray data;
};

struct Symbol
{
    QString name;
    quint32 value = 0;
    int sectionIndex = 1; // 1 起算（0 是空节）
};

struct Segment
{
    quint32 virtualAddress = 0;
    quint32 physicalAddress = 0;
    QByteArray data;
    quint32 memorySize = 0; // 0 = 和数据一样大
};

inline void put16(QByteArray &b, quint16 v) { b.append(char(v & 0xFF)).append(char(v >> 8)); }
inline void put32(QByteArray &b, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        b.append(char((v >> (8 * i)) & 0xFF));
}
inline void set32(QByteArray &b, int pos, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        b[pos + i] = char((v >> (8 * i)) & 0xFF);
}

inline QByteArray build(const QVector<Section> &sections, const QVector<Symbol> &symbols,
                        const QVector<Segment> &segments, quint16 machine = 40)
{
    QByteArray out;
    out.append("\x7f" "ELF", 4);
    out.append(char(1)).append(char(1)).append(char(1)).append(QByteArray(9, '\0'));
    put16(out, 2);       // ET_EXEC
    put16(out, machine); // e_machine
    put32(out, 1);       // e_version
    put32(out, 0);       // e_entry
    put32(out, 0);       // e_phoff（稍后填）
    put32(out, 0);       // e_shoff（稍后填）
    put32(out, 0);       // e_flags
    put16(out, 52);
    put16(out, 32);
    put16(out, quint16(segments.size()));
    put16(out, 40);
    put16(out, 0);       // e_shnum（稍后填）
    put16(out, 0);       // e_shstrndx（稍后填）

    // 程序头
    const quint32 phOffset = quint32(out.size());
    const quint32 segDataStart = phOffset + quint32(segments.size()) * 32;
    quint32 segData = segDataStart;
    for (const Segment &seg : segments) {
        put32(out, 1);
        put32(out, segData);
        put32(out, seg.virtualAddress);
        put32(out, seg.physicalAddress);
        put32(out, quint32(seg.data.size()));
        put32(out, seg.memorySize ? seg.memorySize : quint32(seg.data.size()));
        put32(out, 7);
        put32(out, 4);
        segData += quint32(seg.data.size());
    }
    for (const Segment &seg : segments)
        out.append(seg.data);

    // 节数据
    QVector<quint32> sectionOffsets;
    for (const Section &sec : sections) {
        sectionOffsets.append(quint32(out.size()));
        out.append(sec.data);
    }

    // 字符串表和符号表
    QByteArray shstr(1, '\0');
    QVector<quint32> nameOffsets;
    auto addName = [&shstr](const QString &name) {
        const quint32 pos = quint32(shstr.size());
        shstr.append(name.toLatin1()).append('\0');
        return pos;
    };
    for (const Section &sec : sections)
        nameOffsets.append(addName(sec.name));
    const quint32 symtabName = addName(QStringLiteral(".symtab"));
    const quint32 strtabName = addName(QStringLiteral(".strtab"));
    const quint32 shstrName = addName(QStringLiteral(".shstrtab"));

    QByteArray strtab(1, '\0');
    QByteArray symtab(16, '\0');
    for (const Symbol &sym : symbols) {
        put32(symtab, quint32(strtab.size()));
        strtab.append(sym.name.toLatin1()).append('\0');
        put32(symtab, sym.value);
        put32(symtab, 0);
        symtab.append(char(0x12)).append(char(0));
        put16(symtab, quint16(sym.sectionIndex));
    }

    const quint32 symtabOffset = quint32(out.size());
    out.append(symtab);
    const quint32 strtabOffset = quint32(out.size());
    out.append(strtab);
    const quint32 shstrOffset = quint32(out.size());
    out.append(shstr);

    // 节头
    while (out.size() % 4)
        out.append('\0');
    const quint32 shOffset = quint32(out.size());
    auto header = [&out](quint32 name, quint32 type, quint32 flags, quint32 addr, quint32 offset,
                         quint32 size, quint32 link) {
        put32(out, name);
        put32(out, type);
        put32(out, flags);
        put32(out, addr);
        put32(out, offset);
        put32(out, size);
        put32(out, link);
        put32(out, 0);
        put32(out, 4);
        put32(out, type == 2 ? 16 : 0);
    };
    header(0, 0, 0, 0, 0, 0, 0);
    for (int i = 0; i < sections.size(); ++i)
        header(nameOffsets[i], sections[i].type, sections[i].flags, sections[i].address,
               sectionOffsets[i], quint32(sections[i].data.size()), 0);
    const quint32 strtabIndex = quint32(sections.size()) + 2;
    header(symtabName, 2, 0, 0, symtabOffset, quint32(symtab.size()), strtabIndex);
    header(strtabName, 3, 0, 0, strtabOffset, quint32(strtab.size()), 0);
    header(shstrName, 3, 0, 0, shstrOffset, quint32(shstr.size()), 0);

    set32(out, 28, phOffset);
    set32(out, 32, shOffset);
    const quint16 shNum = quint16(sections.size() + 4);
    out[48] = char(shNum & 0xFF);
    out[49] = char(shNum >> 8);
    const quint16 shStrIndex = quint16(sections.size() + 3);
    out[50] = char(shStrIndex & 0xFF);
    out[51] = char(shStrIndex >> 8);
    return out;
}

// 一个符合 Keil 约定的测试 FLM：函数偏移固定，设备 0x08000000 / 64KB / 页 256 / 扇区 1KB
struct FlmLayout
{
    static constexpr int init = 0x10;
    static constexpr int uninit = 0x20;
    static constexpr int eraseSector = 0x30;
    static constexpr int programPage = 0x40;
    static constexpr int eraseChip = 0x50;
    static constexpr quint32 staticBase = 0x80;
    static constexpr quint32 imageSize = 0xA0; // 代码 0x80 + 数据 0x20
};

inline QByteArray buildTestFlm(bool withEraseChip = true)
{
    QByteArray code(0x80, '\0');
    QByteArray data(0x20, '\x5A');

    QByteArray device(160, '\0');
    put16(device, 0); // 占位，下面整体重写
    device.clear();
    put16(device, 1);
    QByteArray name("Test Flash 64KB");
    name.resize(128);
    device.append(name);
    put16(device, 1);
    put32(device, 0x08000000);
    put32(device, 0x10000);
    put32(device, 256);
    put32(device, 0);
    device.append(char(0xFF)).append(QByteArray(3, '\0'));
    put32(device, 100);
    put32(device, 500);
    put32(device, 0x400);
    put32(device, 0);
    put32(device, 0xFFFFFFFF);
    put32(device, 0xFFFFFFFF);

    QVector<Section> sections = {
        {QStringLiteral("PrgCode"), 1, 6, 0, code},
        {QStringLiteral("PrgData"), 1, 3, FlmLayout::staticBase, data},
        {QStringLiteral("DevDscr"), 1, 2, 0x200, device},
    };
    QVector<Symbol> symbols = {
        {QStringLiteral("Init"), FlmLayout::init | 1, 1},
        {QStringLiteral("UnInit"), FlmLayout::uninit | 1, 1},
        {QStringLiteral("EraseSector"), FlmLayout::eraseSector | 1, 1},
        {QStringLiteral("ProgramPage"), FlmLayout::programPage | 1, 1},
        {QStringLiteral("FlashDevice"), 0x200, 3},
    };
    if (withEraseChip)
        symbols.append({QStringLiteral("EraseChip"), FlmLayout::eraseChip | 1, 1});

    Segment load;
    load.data = code + data;
    // 和真实 FLM 一样，描述符在单独的只读段里，不应该被下载到 RAM
    Segment descriptor;
    descriptor.virtualAddress = descriptor.physicalAddress = 0x200;
    descriptor.data = device;
    return build(sections, symbols, {load, descriptor});
}

} // namespace ElfBuilder

#endif // ELFBUILDER_H
