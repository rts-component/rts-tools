#ifndef FIRMWAREIMAGE_H
#define FIRMWAREIMAGE_H

#include <QByteArray>
#include <QString>
#include <QVector>

// 固件里一段连续数据
struct FirmwareSegment
{
    quint32 address = 0;
    QByteArray data;
};

namespace FirmwareImage {

// 按扩展名读取 .bin / .hex / .elf；.bin 放在 binAddress。
// 返回按地址排序、已合并相邻段的列表
bool load(const QString &path, quint32 binAddress, QVector<FirmwareSegment> *out, QString *error);

bool parseIntelHex(const QByteArray &text, QVector<FirmwareSegment> *out, QString *error);
bool parseElf(const QByteArray &bytes, QVector<FirmwareSegment> *out, QString *error);

// 排序并合并首尾相接的段
QVector<FirmwareSegment> normalized(QVector<FirmwareSegment> segments);

quint64 totalSize(const QVector<FirmwareSegment> &segments);

} // namespace FirmwareImage

#endif // FIRMWAREIMAGE_H
