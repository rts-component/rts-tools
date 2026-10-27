#include "csv/csvgroupdialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

enum Column {
    IndexColumn = 0,
    NameColumn = 1,
    NumericColumn = 2,
    ColumnCount = 3,
};

} // namespace

CsvGroupDialog::CsvGroupDialog(const CsvGroup &group, QWidget *parent)
    : QDialog(parent)
    , m_group(group)
    , m_nameEdit(new QLineEdit(group.name, this))
    , m_separatorCombo(new QComboBox(this))
    , m_table(new QTableWidget(this))
{
    setWindowTitle(tr("Edit data row"));
    resize(380, 420);

    m_separatorCombo->setEditable(true);
    m_separatorCombo->setInsertPolicy(QComboBox::NoInsert);
    m_separatorCombo->addItems({QStringLiteral(","), QStringLiteral(";"),
                                QStringLiteral("\\t"), QStringLiteral("|"),
                                QStringLiteral(" ")});
    const int separatorIndex = m_separatorCombo->findText(group.separator);
    if (separatorIndex >= 0)
        m_separatorCombo->setCurrentIndex(separatorIndex);
    else
        m_separatorCombo->setEditText(group.separator);

    auto *form = new QFormLayout;
    form->addRow(tr("Name"), m_nameEdit);
    form->addRow(tr("Separator"), m_separatorCombo);

    m_table->setColumnCount(ColumnCount);
    m_table->setHorizontalHeaderLabels({tr("No."), tr("Field name"), tr("Numeric")});
    m_table->horizontalHeader()->setSectionResizeMode(IndexColumn, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(NumericColumn, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);

    auto *hint = new QLabel(tr("Field names are matched to CSV columns by order. "
                               "A row only updates when the field count matches."),
                            this);
    hint->setWordWrap(true);

    auto *addButton = new QPushButton(tr("Add"), this);
    auto *removeButton = new QPushButton(tr("Remove"), this);
    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    auto *buttonLayout = new QHBoxLayout;
    buttonLayout->addWidget(addButton);
    buttonLayout->addWidget(removeButton);
    buttonLayout->addStretch();
    buttonLayout->addWidget(buttonBox);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(hint);
    layout->addWidget(m_table, 1);
    layout->addLayout(buttonLayout);

    for (const CsvField &field : group.fields)
        addRow(field);

    connect(addButton, &QPushButton::clicked, this, [this] {
        CsvField field;
        field.name = tr("Column %1").arg(m_table->rowCount() + 1);
        field.numeric = true;
        addRow(field);
        const int row = m_table->rowCount() - 1;
        m_table->setCurrentCell(row, NameColumn);
        m_table->editItem(m_table->item(row, NameColumn));
    });
    connect(removeButton, &QPushButton::clicked, this, &CsvGroupDialog::removeCurrentRow);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

CsvGroup CsvGroupDialog::group() const
{
    CsvGroup result = m_group;
    result.name = m_nameEdit->text().trimmed();
    result.separator = m_separatorCombo->currentText().isEmpty()
            ? QStringLiteral(",")
            : m_separatorCombo->currentText();

    result.fields.clear();
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QTableWidgetItem *nameItem = m_table->item(row, NameColumn);
        const QTableWidgetItem *numericItem = m_table->item(row, NumericColumn);

        CsvField field;
        field.name = nameItem ? nameItem->text().trimmed() : QString();
        field.numeric = !numericItem || numericItem->checkState() == Qt::Checked;
        if (row < m_group.fields.size()) {
            field.formula = m_group.fields.at(row).formula;
            // 非数字字段不能有曲线
            field.curve = field.numeric && m_group.fields.at(row).curve;
        }
        result.fields.append(field);
    }
    return result;
}

void CsvGroupDialog::addRow(const CsvField &field)
{
    const bool wasUpdating = m_updating;
    m_updating = true;

    const int row = m_table->rowCount();
    m_table->insertRow(row);

    auto *indexItem = new QTableWidgetItem;
    indexItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    indexItem->setTextAlignment(Qt::AlignCenter);
    m_table->setItem(row, IndexColumn, indexItem);

    m_table->setItem(row, NameColumn, new QTableWidgetItem(field.name));

    auto *numericItem = new QTableWidgetItem;
    numericItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    numericItem->setCheckState(field.numeric ? Qt::Checked : Qt::Unchecked);
    numericItem->setTextAlignment(Qt::AlignCenter);
    m_table->setItem(row, NumericColumn, numericItem);

    m_updating = wasUpdating;
    resequence();
}

void CsvGroupDialog::removeCurrentRow()
{
    const int row = m_table->currentRow();
    if (row < 0)
        return;
    m_table->removeRow(row);
    resequence();
}

void CsvGroupDialog::resequence()
{
    const bool wasUpdating = m_updating;
    m_updating = true;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (QTableWidgetItem *item = m_table->item(row, IndexColumn))
            item->setText(QString::number(row + 1));
    }
    m_updating = wasUpdating;
}
