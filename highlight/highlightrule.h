#ifndef HIGHLIGHTRULE_H
#define HIGHLIGHTRULE_H

#include <QColor>
#include <QRegularExpression>
#include <QString>
#include <QVector>

// 一条用户自定义的高亮规则：正则匹配到的文字用指定颜色显示
struct HighlightRule
{
    QString pattern;
    QColor color;
    bool matchCase = false;
    bool enabled = true;      // 关闭后规则保留但不参与着色
    QRegularExpression regex; // 由 compile() 生成，不参与持久化

    void compile();
    bool isValid() const;
};

struct HighlightSpan
{
    int start = 0;
    int length = 0;
    QColor color;
};

// 规则按顺序匹配，先命中的字符优先着色；无效规则被忽略。
QVector<HighlightSpan> findHighlightSpans(const QString &text, const QVector<HighlightRule> &rules);

QVector<HighlightRule> loadHighlightRules();
void saveHighlightRules(const QVector<HighlightRule> &rules);

#endif // HIGHLIGHTRULE_H
