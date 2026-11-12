#ifndef CSVPARSER_H
#define CSVPARSER_H

#include <QString>
#include <QStringList>

// 把用户填写的分隔符文本还原成实际分隔符，支持 "\t" 这样的转义写法。
QString decodeSeparator(const QString &text);

// 按分隔符切分一行文本；支持双引号包裹的字段（"" 表示一个引号字符）。
QStringList splitCsvLine(const QString &line, const QString &separator);

// 是否像一条 CSV 记录：至少两列，且至少一列非空。
bool looksLikeCsvRecord(const QStringList &fields);

// 是否为合法数字（拒绝空串、"nan"/"inf"、带其他字符的内容）
bool isNumericValue(const QString &text);

#endif // CSVPARSER_H
