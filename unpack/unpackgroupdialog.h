#ifndef UNPACKGROUPDIALOG_H
#define UNPACKGROUPDIALOG_H

#include "unpack/unpackconfig.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QPushButton;
class QLineEdit;
class QTableWidget;

// 编辑一行（分组）：名称、分隔符、列名
class UnpackGroupDialog : public QDialog
{
    Q_OBJECT

public:
    explicit UnpackGroupDialog(const UnpackGroup &group, QWidget *parent = nullptr);

    UnpackGroup group() const;

private:
    void addRow(const UnpackField &field);
    void removeCurrentRow();
    void resequence();
    void updateMode();
    void editProtocol();

    UnpackGroup m_group;
    QLineEdit *m_nameEdit = nullptr;
    QComboBox *m_separatorCombo = nullptr;
    QComboBox *m_modeCombo = nullptr;
    QWidget *m_textPage = nullptr;
    QWidget *m_protocolPage = nullptr;
    QLabel *m_protocolSummary = nullptr;
    QVector<ProtoField> m_layout;
    QTableWidget *m_table = nullptr;
    bool m_updating = false;
};

#endif // UNPACKGROUPDIALOG_H
