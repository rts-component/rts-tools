#ifndef HIGHLIGHTDIALOG_H
#define HIGHLIGHTDIALOG_H

#include "highlight/highlightrule.h"

#include <QDialog>

class QTableWidget;

// 编辑“正则 + 颜色”的高亮规则；改动即时发出，供主窗口实时应用
class HighlightDialog : public QDialog
{
    Q_OBJECT

public:
    explicit HighlightDialog(const QVector<HighlightRule> &rules, QWidget *parent = nullptr);

    QVector<HighlightRule> rules() const;

signals:
    void rulesChanged(const QVector<HighlightRule> &rules);

private:
    void addRow(const HighlightRule &rule);
    void removeCurrentRow();
    void chooseColor(int row);
    void updateRowState(int row);
    void notifyRulesChanged();

    QTableWidget *m_table = nullptr;
    bool m_updating = false;
};

#endif // HIGHLIGHTDIALOG_H
