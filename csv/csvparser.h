#ifndef CSVPARSER_H
#define CSVPARSER_H

#include <QString>
#include <QStringList>
#include <QVector>

// 把用户填写的分隔符文本还原成实际分隔符，支持 "\t" 这样的转义写法。
QString decodeSeparator(const QString &text);

// 按分隔符切分一行文本；支持双引号包裹的字段（"" 表示一个引号字符）。
QStringList splitCsvLine(const QString &line, const QString &separator);

// 是否像一条 CSV 记录：至少两列，且至少一列非空。
bool looksLikeCsvRecord(const QStringList &fields);

// 是否为合法数字（拒绝空串、"nan"/"inf"、带其他字符的内容）
bool isNumericValue(const QString &text);

// 生成一个新的分组 id（曲线系列命名用）
QString newGroupId();

// 一行（一个展示分组）里的一个字段
struct CsvField
{
    QString name;
    QString formula;        // 预留：公式换算
    QString color;          // 曲线颜色 "#rrggbb"，空 = 按 key 自动分配
    bool numeric = true;    // 是否数字：非数字字段不显示 fx / 曲线
    bool curve = false;
};

// 一个展示分组：独立的分隔符与列定义
struct CsvGroup
{
    QString id; // 稳定标识（曲线系列据此命名），缺失时自动生成
    bool enabled = true;
    QString name;
    QString separator = QStringLiteral(",");
    bool curve = false; // 该分组是否打开曲线
    QVector<CsvField> fields;
};

struct CsvParseConfig
{
    bool enabled = false;
    QVector<CsvGroup> groups;
};

CsvParseConfig loadCsvParseConfig();
void saveCsvParseConfig(const CsvParseConfig &config);

#endif // CSVPARSER_H
