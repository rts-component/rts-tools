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
    // 开启后，日志窗口行数达到 maxLines 时在下一次追加前清空显示（不影响落盘的日志文件）
    void setAutoClear(bool enabled, int maxLines);

private:
    void insertHighlighted(QTextCursor &cursor, const QString &text);
    void rehighlight();
    void scrollToEnd();
    void trim();

    QPlainTextEdit *m_edit = nullptr;
    QVector<HighlightRule> m_rules;
    QTimer *m_rehighlightTimer = nullptr; // 合并连续的规则改动，避免反复重排整篇日志
    QTimer *m_scrollTimer = nullptr; // 节流期内收到的数据，延后补一次滚动
    QElapsedTimer m_scrollThrottle;
    bool m_autoClear = false;
    int m_autoClearLines = 5000;
    bool m_following = true;  // 用户停在底部时为真；只由用户的滚动操作改变
    bool m_appending = false; // append/裁剪期间滚动条的变化不是用户操作，忽略
};

#endif // RECEIVELOGVIEW_H
