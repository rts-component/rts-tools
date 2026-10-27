#ifndef RECEIVELOGVIEW_H
#define RECEIVELOGVIEW_H

#include "highlight/highlightrule.h"

#include <QElapsedTimer>
#include <QObject>
#include <QVector>

class QPlainTextEdit;
class QTextCursor;
class QTimer;

// 接收日志的显示层：往 QPlainTextEdit 里追加文本（带高亮、长行切块、自动滚动节流、超限裁剪）。
// 只管“怎么显示”；收到的字节如何变成文本、是否落盘由 SerialPanel 决定。
class ReceiveLogView : public QObject
{
    Q_OBJECT

public:
    explicit ReceiveLogView(QPlainTextEdit *edit, QObject *parent = nullptr);

    const QVector<HighlightRule> &rules() const { return m_rules; }
    // 换一套高亮规则；rehighlight 为真时按 150ms 合并后重放已有日志的颜色
    void setRules(const QVector<HighlightRule> &rules, bool rehighlight = true);

    // continueLine 为真时第一段文本接在最后一行后面，否则另起一行
    void append(const QString &text, bool continueLine);
    void clear();

private:
    void insertHighlighted(QTextCursor &cursor, const QString &text);
    void rehighlight();
    void trim();

    QPlainTextEdit *m_edit = nullptr;
    QVector<HighlightRule> m_rules;
    QTimer *m_rehighlightTimer = nullptr; // 合并连续的规则改动，避免反复重排整篇日志
    QElapsedTimer m_scrollThrottle;
};

#endif // RECEIVELOGVIEW_H
