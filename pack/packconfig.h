#ifndef PACKCONFIG_H
#define PACKCONFIG_H

#include "unpack/protocolparser.h"

#include <QString>
#include <QVector>

// 发送面板（Pack）的配置模型：一组可复用的组包格式 + 若干条待发送记录

// 发送时追加在末尾的结束符
enum class PackEnding {
    None,
    CrLf, // \r\n
    Lf,   // \n
    Cr,   // \r
};

QByteArray packEndingBytes(PackEnding ending);

// 一个组包格式：协议帧定义，发送时由"数据"里的值填充数据字段
struct PackFormat
{
    QString id;
    QString name;
    QVector<ProtoField> layout;
};

// 一条待发送记录
struct PackRow
{
    bool checked = false;             // 参与"发送已勾选"
    QString name;                     // 备注
    QString text;                     // 无格式：要发送的文本 / Hex 字节；有格式：数据字段的值（空格分隔）
    bool hex = false;                 // 无格式：按十六进制字节发送；有格式：数值按十六进制解释
    int delayMs = 50;                 // 发送后等待多久再发下一条
    QString formatId;                 // 空 = 不组包
    PackEnding ending = PackEnding::CrLf;
};

struct PackConfig
{
    QVector<PackFormat> formats;
    QVector<PackRow> rows;
};

PackConfig loadPackConfig();
void savePackConfig(const PackConfig &config);

// 新建格式用的示例：帧头 AA 55 + 两个 2Byte 数据 + 累加和
QVector<ProtoField> defaultPackLayout();

#endif // PACKCONFIG_H
