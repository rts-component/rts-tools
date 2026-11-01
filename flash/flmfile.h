#ifndef FLMFILE_H
#define FLMFILE_H

#include <QByteArray>
#include <QString>
#include <QVector>

// FLM 里 FlashDevice 的扇区表项：从 offset 开始（相对设备起始地址）每个扇区 size 字节
struct FlmSector
{
    quint32 size = 0;
    quint32 offset = 0;
};

// 解析后的 Keil Flash 算法（.FLM）：要下载到 RAM 的镜像、函数入口和 Flash 布局。
// 镜像按 ELF 虚拟地址排布（起始 0），函数入口/数据基址都是镜像内的偏移，
// 烧录时整体搬到 RAM 里某个地址即可（算法是位置无关的，靠 R9 访问数据）。
struct FlmFile
{
    static constexpr int kNoEntry = -1;

    QByteArray image;
    quint32 staticBase = 0; // R9 指向的数据段起始偏移
    int initOffset = kNoEntry;
    int uninitOffset = kNoEntry;
    int eraseChipOffset = kNoEntry;
    int eraseSectorOffset = kNoEntry;
    int programPageOffset = kNoEntry;

    QString deviceName;
    quint32 deviceAddress = 0;
    quint32 deviceSize = 0;
    quint32 pageSize = 0;
    quint8 emptyValue = 0xFF;
    quint32 programTimeoutMs = 0;
    quint32 eraseTimeoutMs = 0;
    QVector<FlmSector> sectors;

    static bool load(const QString &path, FlmFile *out, QString *error);
    static bool parse(const QByteArray &bytes, FlmFile *out, QString *error);

    // 地址是否在设备范围内
    bool contains(quint32 address, quint32 length) const;
    // 覆盖 [start, end) 的所有扇区起始地址（升序）；范围超出设备时返回空
    // 用 64 位，设备正好结束在 4GB 边界时 end 才不会回绕成 0
    QVector<quint32> sectorsCovering(quint64 start, quint64 end) const;
};

#endif // FLMFILE_H
