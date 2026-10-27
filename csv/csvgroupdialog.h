#ifndef CSVGROUPDIALOG_H
#define CSVGROUPDIALOG_H

#include "csv/csvparser.h"

#include <QDialog>

class QComboBox;
class QLineEdit;
class QTableWidget;

// 编辑一行（分组）：名称、分隔符、列名
class CsvGroupDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CsvGroupDialog(const CsvGroup &group, QWidget *parent = nullptr);

    CsvGroup group() const;

private:
    void addRow(const CsvField &field);
    void removeCurrentRow();
    void resequence();

    CsvGroup m_group;
    QLineEdit *m_nameEdit = nullptr;
    QComboBox *m_separatorCombo = nullptr;
    QTableWidget *m_table = nullptr;
    bool m_updating = false;
};

#endif // CSVGROUPDIALOG_H
