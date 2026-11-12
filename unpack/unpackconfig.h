#ifndef UNPACKCONFIG_H
#define UNPACKCONFIG_H

#include "unpack/protocolparser.h"

#include <QString>
#include <QVector>

// 解包面板的配置模型：若干"行"（UnpackGroup），每行用文本 CSV 或二进制协议帧之一解析

// 生成一个新的分组 id（曲线系列命名用）
QString newGroupId();

// 一行里的一个显示字段
struct UnpackField
{
    QString name;
    QString formula;        // 预留：公式换算
    QString color;          // 曲线颜色 "#rrggbb"，空 = 按 key 自动分配
    bool numeric = true;    // 是否数字：非数字字段不显示 fx / 曲线
    bool curve = false;
};

enum class UnpackMode {
    Csv,      // 按分隔符切分文本行
    Protocol, // 按帧头/长度/校验等从字节流里切二进制帧
};

// 一个展示分组（面板上的一行）：独立的解析方式与字段定义
struct UnpackGroup
{
    QString id; // 稳定标识（曲线系列据此命名），缺失时自动生成
    bool enabled = true;
    QString name;
    bool curve = false; // 该分组是否打开曲线
    UnpackMode mode = UnpackMode::Csv;

    // Csv 模式：fields 由用户逐个定义
    QString separator = QStringLiteral(",");
    // Protocol 模式：fields 由 layout 派生，只含"显示到面板"的字段
    QVector<ProtoField> layout;

    QVector<UnpackField> fields;

    bool isProtocol() const { return mode == UnpackMode::Protocol; }
};

struct UnpackConfig
{
    bool enabled = false;
    QVector<UnpackGroup> groups;
};

UnpackConfig loadUnpackConfig();
void saveUnpackConfig(const UnpackConfig &config);

// 由协议定义生成面板字段；oldFields 里同位置的颜色 / 曲线勾选保留下来
QVector<UnpackField> protocolFields(const QVector<ProtoField> &layout,
                                    const QVector<UnpackField> &oldFields = {});

#endif // UNPACKCONFIG_H
