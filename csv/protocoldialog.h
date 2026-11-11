#ifndef PROTOCOLDIALOG_H
#define PROTOCOLDIALOG_H

#include "csv/protocolparser.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTableWidget;

// 协议帧编辑：顶部按钮添加字段，左侧是字段列表（可上移/下移/删除），右侧是选中字段的属性。
class ProtocolDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProtocolDialog(const QVector<ProtoField> &layout, QWidget *parent = nullptr);

    QVector<ProtoField> layout() const { return m_layout; }

    void accept() override;

private:
    void addField(ProtoFieldType type);
    void removeCurrent();
    void moveCurrent(int offset);
    void refreshTable(int selectRow);
    void refreshRow(int row);
    void onCurrentRowChanged();
    void loadProperties();
    void populateConversions(const ProtoField &field);
    void setRowShown(QWidget *editor, bool shown);

    // 属性编辑 → 写回 m_layout[当前行]
    void onNameEdited();
    void onValueEdited();
    void onConversionChanged();
    void onOptionChanged();

    QVector<ProtoField> m_layout;
    bool m_updating = false;

    QTableWidget *m_table = nullptr;
    QLabel *m_summary = nullptr;
    QFormLayout *m_form = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_valueEdit = nullptr;
    QComboBox *m_conversionCombo = nullptr;
    QComboBox *m_checksumCombo = nullptr;
    QComboBox *m_checksumRangeCombo = nullptr;
    QSpinBox *m_lengthOffsetSpin = nullptr;
    QCheckBox *m_bigEndianCheck = nullptr;
    QCheckBox *m_showCheck = nullptr;
    QLabel *m_propertyHint = nullptr;
};

#endif // PROTOCOLDIALOG_H
