#include "app/receivelogview.h"

#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>

namespace {

constexpr int kMaxLogChars = 1000000;       // 日志窗口保留字符上限
constexpr int kLogTrimTargetChars = 400000; // 超限后回落到该字符数，摊薄裁剪开销
constexpr int kMaxLogBlocks = 20000;        // 日志窗口保留行数上限
constexpr int kMaxBlockChars = 2048;        // 单个文本块最大长度（限制重排版开销）
constexpr int kScrollThrottleMs = 150;      // 自动滚动的节流间隔
constexpr int kRehighlightDelayMs = 150;

} // namespace

ReceiveLogView::ReceiveLogView(QPlainTextEdit *edit, QObject *parent)
    : QObject(parent)
    , m_edit(edit)
{
    m_edit->setMaximumBlockCount(kMaxLogBlocks);
    // 接收区不出现水平滚动条：WidgetWidth 自动换行（超长串会在行尾断开）
    m_edit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_edit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_rehighlightTimer = new QTimer(this);
    m_rehighlightTimer->setSingleShot(true);
    m_rehighlightTimer->setInterval(kRehighlightDelayMs);
    connect(m_rehighlightTimer, &QTimer::timeout, this, &ReceiveLogView::rehighlight);

    m_scrollThrottle.start();
}

void ReceiveLogView::setRules(const QVector<HighlightRule> &rules, bool rehighlight)
{
    m_rules = rules;
    if (rehighlight)
        m_rehighlightTimer->start();
}

void ReceiveLogView::clear()
{
    m_edit->clear();
}

void ReceiveLogView::append(const QString &text, bool continueLine)
{
    if (text.isEmpty())
        return;

    QScrollBar *scrollBar = m_edit->verticalScrollBar();
    const bool followOutput = scrollBar->value() >= scrollBar->maximum() - 4;

    m_edit->setUpdatesEnabled(false);

    // 按行插入，并把超长行切成固定长度，避免单个 QTextBlock 越滚越大导致整块重排版
    QTextDocument *document = m_edit->document();
    QTextCursor cursor(document);
    cursor.movePosition(QTextCursor::End);
    cursor.beginEditBlock();

    int position = 0;
    while (position < text.size()) {
        const int newline = text.indexOf(QLatin1Char('\n'), position);
        const bool hasNewline = newline >= 0;
        const int lineEnd = hasNewline ? newline : text.size();

        int offset = position;
        while (true) {
            const int length = qMin(kMaxBlockChars, lineEnd - offset);
            if (!continueLine && !document->isEmpty()) {
                cursor.setCharFormat(QTextCharFormat());
                cursor.insertBlock();
            }
            insertHighlighted(cursor, text.mid(offset, length));
            continueLine = false;
            offset += length;
            if (offset >= lineEnd)
                break;
        }

        if (!hasNewline)
            break;
        position = lineEnd + 1;
    }
    cursor.endEditBlock();

    trim();
    m_edit->setUpdatesEnabled(true);

    // 滚动到底会触发全文高度排版，按 ~150ms 节流，既跟手又不拖慢接收
    if (followOutput && m_scrollThrottle.elapsed() >= kScrollThrottleMs) {
        scrollBar->setValue(scrollBar->maximum());
        m_scrollThrottle.restart();
    }
}

void ReceiveLogView::insertHighlighted(QTextCursor &cursor, const QString &text)
{
    const QVector<HighlightSpan> spans = findHighlightSpans(text, m_rules);
    if (spans.isEmpty()) {
        cursor.setCharFormat(QTextCharFormat());
        cursor.insertText(text);
        return;
    }

    int position = 0;
    for (const HighlightSpan &span : spans) {
        if (span.start > position) {
            cursor.setCharFormat(QTextCharFormat());
            cursor.insertText(text.mid(position, span.start - position));
        }
        QTextCharFormat format;
        format.setForeground(span.color);
        cursor.setCharFormat(format);
        cursor.insertText(text.mid(span.start, span.length));
        position = span.start + span.length;
    }

    if (position < text.size()) {
        cursor.setCharFormat(QTextCharFormat());
        cursor.insertText(text.mid(position));
    }
}

// 规则改动后重放已有日志：只改字符格式、不动文本，避免整篇重排
void ReceiveLogView::rehighlight()
{
    QTextDocument *document = m_edit->document();
    if (document->isEmpty())
        return;

    QScrollBar *scrollBar = m_edit->verticalScrollBar();
    const int scrollValue = scrollBar->value();

    m_edit->setUpdatesEnabled(false);

    QTextCursor clearCursor(document);
    clearCursor.beginEditBlock();
    clearCursor.movePosition(QTextCursor::Start);
    clearCursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    clearCursor.setCharFormat(QTextCharFormat());
    clearCursor.endEditBlock();

    QTextCursor cursor(document);
    cursor.beginEditBlock();
    for (QTextBlock block = document->firstBlock(); block.isValid(); block = block.next()) {
        const QString text = block.text();
        if (text.isEmpty())
            continue;

        const QVector<HighlightSpan> spans = findHighlightSpans(text, m_rules);
        for (const HighlightSpan &span : spans) {
            QTextCharFormat format;
            format.setForeground(span.color);
            cursor.setPosition(block.position() + span.start);
            cursor.setPosition(block.position() + span.start + span.length, QTextCursor::KeepAnchor);
            cursor.setCharFormat(format);
        }
    }
    cursor.endEditBlock();

    m_edit->setUpdatesEnabled(true);
    scrollBar->setValue(qMin(scrollValue, scrollBar->maximum()));
}

void ReceiveLogView::trim()
{
    if (m_edit->document()->characterCount() <= kMaxLogChars)
        return;

    // 一次性删除文档开头的一段，比 setPlainText 重建更便宜，且保留剩余文本的高亮格式
    const QString content = m_edit->toPlainText();
    int cut = content.size() - kLogTrimTargetChars;
    const int newline = content.indexOf(QLatin1Char('\n'), cut);
    if (newline >= 0)
        cut = newline + 1;

    QTextCursor cursor(m_edit->document());
    cursor.setPosition(0);
    cursor.setPosition(cut, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
}
