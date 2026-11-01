#ifndef ELF32_H
#define ELF32_H

#include <QByteArray>
#include <QString>
#include <QVector>

// 最小的 ELF32 小端 ARM 只读解析：FLM 算法文件和 .elf 固件共用
struct ElfSection
{
    QString name;
    quint32 type = 0;
    quint32 flags = 0;
    quint32 address = 0;
    quint32 offset = 0;
    quint32 size = 0;

    static constexpr quint32 kTypeNoBits = 8;
    static constexpr quint32 kFlagAlloc = 2;
};

struct ElfSegment
{
    quint32 type = 0;
    quint32 offset = 0;
    quint32 virtualAddress = 0;
    quint32 physicalAddress = 0;
    quint32 fileSize = 0;
    quint32 memorySize = 0;

    static constexpr quint32 kTypeLoad = 1;
};

struct ElfSymbol
{
    QString name;
    quint32 value = 0;
    quint32 size = 0;
    quint16 sectionIndex = 0;
};

struct Elf32
{
    QByteArray data;
    QVector<ElfSection> sections;
    QVector<ElfSegment> segments;
    QVector<ElfSymbol> symbols;

    // 不是 ELF32 / 小端 / ARM，或者结构被截断时返回 false 并写出原因
    static bool parse(const QByteArray &bytes, Elf32 *out, QString *error);

    const ElfSection *findSection(const QString &name) const;
    const ElfSymbol *findSymbol(const QString &name) const;
};

#endif // ELF32_H
