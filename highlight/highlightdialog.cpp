#include "highlight/highlightdialog.h"

#include <QApplication>
#include <QAbstractItemModel>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QStyleOptionButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

enum Column {
    EnabledColumn = 0,
    PatternColumn = 1,
    MatchCaseColumn = 2,
    ColorColumn = 3,
    ColumnCount = 4,
};

const QColor kDefaultColor(0xE5, 0x39, 0x35);

// 交给样式绘制单元格背景（选中/失焦/交替行色都由它决定），
// 子类只在这个背景上叠加自己的内容。
class CellBackgroundDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    // 只画背景与焦点框，不画文本/复选框
    void paintBackground(QPainter *painter, const QStyleOptionViewItem &option,
                         const QModelIndex &index) const
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.text.clear();
        opt.icon = QIcon();
        opt.features &= ~(QStyleOptionViewItem::HasCheckIndicator
                          | QStyleOptionViewItem::HasDisplay
                          | QStyleOptionViewItem::HasDecoration);
        const QWidget *widget = option.widget;
        const QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
    }
};

// 颜色单元格：默认绘制会先铺条目背景、再叠一层选中高亮，
// 导致刚选完色（该行必然处于选中态）看到的并不是实际颜色。
class ColorSwatchDelegate : public CellBackgroundDelegate
{
public:
    using CellBackgroundDelegate::CellBackgroundDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        paintBackground(painter, option, index);

        const QColor color = index.data(Qt::UserRole).value<QColor>();
        const QRect chip = option.rect.adjusted(4, 4, -4, -4);

        painter->save();
        painter->setClipRect(option.rect);
        painter->setRenderHint(QPainter::Antialiasing, true);
        // 填充不带描边、描边画在内侧，避免半像素抗锯齿溢出到单元格背景上
        painter->setPen(Qt::NoPen);
        painter->setBrush(color.isValid() ? color : option.palette.base());
        painter->drawRoundedRect(chip, 3, 3);

        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(QColor(0, 0, 0, 50), 1));
        painter->drawRoundedRect(QRectF(chip).adjusted(0.5, 0.5, -0.5, -0.5), 2.5, 2.5);

        painter->setPen(color.isValid() && color.lightness() < 128 ? Qt::white : Qt::black);
        painter->drawText(chip, Qt::AlignCenter, index.data(Qt::DisplayRole).toString());
        painter->restore();
    }
};

// 勾选单元格：默认代理会在按下时切换、释放时又可能再切换一次，
// 与视图的 clicked 信号叠加后表现为“点了没反应”。这里统一只响应一次释放，
// 并且整格可点（不必精确点在方框上）。
class CheckBoxDelegate : public CellBackgroundDelegate
{
public:
    using CellBackgroundDelegate::CellBackgroundDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        paintBackground(painter, option, index);

        QStyleOptionButton check;
        check.state = QStyle::State_Enabled;
        check.state |= index.data(Qt::CheckStateRole).toInt() == Qt::Checked ? QStyle::State_On
                                                                            : QStyle::State_Off;
        const QWidget *widget = option.widget;
        const QStyle *style = widget ? widget->style() : QApplication::style();
        const int width = style->pixelMetric(QStyle::PM_IndicatorWidth, &check, widget);
        const int height = style->pixelMetric(QStyle::PM_IndicatorHeight, &check, widget);
        check.rect = QRect(option.rect.center() - QPoint(width / 2, height / 2),
                           QSize(width, height));

        painter->save();
        painter->setClipRect(option.rect);
        style->drawPrimitive(QStyle::PE_IndicatorCheckBox, &check, painter);
        painter->restore();
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override
    {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick)
            return false; // 按下不切换，避免一次点击被处理两遍

        if (event->type() == QEvent::MouseButtonRelease) {
            const auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() != Qt::LeftButton
                || !option.rect.contains(mouseEvent->position().toPoint()))
                return false;
            const bool checked = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
            model->setData(index, checked ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
            return true;
        }

        // 键盘（空格）等其它操作沿用基类行为
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
};

} // namespace

HighlightDialog::HighlightDialog(const QVector<HighlightRule> &rules, QWidget *parent)
    : QDialog(parent)
    , m_table(new QTableWidget(this))
{
    setWindowTitle(tr("Highlight rules"));
    resize(600, 340);

    m_table->setColumnCount(ColumnCount);
    m_table->setHorizontalHeaderLabels({tr("Enabled"), tr("Pattern (regular expression)"),
                                        tr("Match case"), tr("Color")});
    m_table->horizontalHeader()->setSectionResizeMode(EnabledColumn, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(PatternColumn, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(MatchCaseColumn, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColorColumn, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setItemDelegateForColumn(ColorColumn, new ColorSwatchDelegate(m_table));
    m_table->setItemDelegateForColumn(EnabledColumn, new CheckBoxDelegate(m_table));
    m_table->setItemDelegateForColumn(MatchCaseColumn, new CheckBoxDelegate(m_table));

    auto *hint = new QLabel(tr("Only enabled rules are applied to the receive log. Earlier rules win."),
                            this);
    hint->setWordWrap(true);

    auto *addButton = new QPushButton(tr("Add"), this);
    auto *removeButton = new QPushButton(tr("Remove"), this);
    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, this);

    auto *buttonLayout = new QHBoxLayout;
    buttonLayout->addWidget(addButton);
    buttonLayout->addWidget(removeButton);
    buttonLayout->addStretch();
    buttonLayout->addWidget(buttonBox);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(hint);
    layout->addWidget(m_table);
    layout->addLayout(buttonLayout);

    for (const HighlightRule &rule : rules)
        addRow(rule);

    connect(addButton, &QPushButton::clicked, this, [this] {
        HighlightRule rule;
        rule.color = kDefaultColor;
        addRow(rule);
        const int row = m_table->rowCount() - 1;
        m_table->setCurrentCell(row, PatternColumn);
        m_table->editItem(m_table->item(row, PatternColumn));
    });
    connect(removeButton, &QPushButton::clicked, this, &HighlightDialog::removeCurrentRow);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(m_table, &QTableWidget::cellClicked, this, [this](int row, int column) {
        if (column == ColorColumn)
            chooseColor(row);
    });
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_updating || !item)
            return;
        updateRowState(item->row());
        notifyRulesChanged();
    });
}

QVector<HighlightRule> HighlightDialog::rules() const
{
    QVector<HighlightRule> result;
    result.reserve(m_table->rowCount());
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QTableWidgetItem *enabledItem = m_table->item(row, EnabledColumn);
        const QTableWidgetItem *patternItem = m_table->item(row, PatternColumn);
        const QTableWidgetItem *caseItem = m_table->item(row, MatchCaseColumn);
        const QTableWidgetItem *colorItem = m_table->item(row, ColorColumn);
        if (!enabledItem || !patternItem || !caseItem || !colorItem)
            continue;

        HighlightRule rule;
        rule.enabled = enabledItem->checkState() == Qt::Checked;
        rule.pattern = patternItem->text().trimmed();
        rule.matchCase = caseItem->checkState() == Qt::Checked;
        rule.color = colorItem->data(Qt::UserRole).value<QColor>();
        rule.compile();
        if (rule.isValid() && rule.color.isValid())
            result.append(rule);
    }
    return result;
}

void HighlightDialog::addRow(const HighlightRule &rule)
{
    const int row = m_table->rowCount();
    const QColor color = rule.color.isValid() ? rule.color : kDefaultColor;

    m_updating = true;
    m_table->insertRow(row);

    auto *enabledItem = new QTableWidgetItem;
    enabledItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    enabledItem->setCheckState(rule.enabled ? Qt::Checked : Qt::Unchecked);
    enabledItem->setTextAlignment(Qt::AlignCenter);
    auto *patternItem = new QTableWidgetItem(rule.pattern);
    auto *caseItem = new QTableWidgetItem;
    caseItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    caseItem->setCheckState(rule.matchCase ? Qt::Checked : Qt::Unchecked);
    caseItem->setTextAlignment(Qt::AlignCenter);
    auto *colorItem = new QTableWidgetItem;
    colorItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    colorItem->setData(Qt::UserRole, color);
    colorItem->setTextAlignment(Qt::AlignCenter);

    m_table->setItem(row, EnabledColumn, enabledItem);
    m_table->setItem(row, PatternColumn, patternItem);
    m_table->setItem(row, MatchCaseColumn, caseItem);
    m_table->setItem(row, ColorColumn, colorItem);
    m_updating = false;

    updateRowState(row);
}

void HighlightDialog::removeCurrentRow()
{
    const int row = m_table->currentRow();
    if (row < 0)
        return;
    m_table->removeRow(row);
    notifyRulesChanged();
}

void HighlightDialog::chooseColor(int row)
{
    QTableWidgetItem *item = m_table->item(row, ColorColumn);
    if (!item)
        return;

    const QColor current = item->data(Qt::UserRole).value<QColor>();
    const QColor color = QColorDialog::getColor(
            current.isValid() ? current : kDefaultColor, this, tr("Highlight color"));
    if (!color.isValid())
        return;

    item->setData(Qt::UserRole, color);
    updateRowState(row);
    notifyRulesChanged();
}

void HighlightDialog::updateRowState(int row)
{
    QTableWidgetItem *patternItem = m_table->item(row, PatternColumn);
    QTableWidgetItem *caseItem = m_table->item(row, MatchCaseColumn);
    QTableWidgetItem *colorItem = m_table->item(row, ColorColumn);
    if (!patternItem || !caseItem || !colorItem)
        return;

    // 色块与文字颜色由 ColorSwatchDelegate 绘制，这里只更新数据。
    // setText 会再次触发 itemChanged，用 m_updating 挡住这轮重入。
    const bool wasUpdating = m_updating;
    m_updating = true;
    colorItem->setText(colorItem->data(Qt::UserRole).value<QColor>().name());
    m_updating = wasUpdating;

    const QString pattern = patternItem->text().trimmed();
    const QRegularExpression regex(
            pattern, caseItem->checkState() == Qt::Checked
                             ? QRegularExpression::NoPatternOption
                             : QRegularExpression::CaseInsensitiveOption);
    const bool valid = regex.isValid();
    if (!pattern.isEmpty() && !valid)
        patternItem->setForeground(QBrush(Qt::red));
    else if (!m_table->item(row, EnabledColumn)
                      || m_table->item(row, EnabledColumn)->checkState() != Qt::Checked)
        patternItem->setForeground(QBrush(QColor(0x9e, 0x9e, 0x9e))); // 停用的规则灰显
    else
        patternItem->setForeground(QBrush());

    patternItem->setToolTip(pattern.isEmpty()
                                    ? QString()
                                    : (valid ? tr("Matches every occurrence of this expression.")
                                             : regex.errorString()));
}

void HighlightDialog::notifyRulesChanged()
{
    if (!m_updating)
        emit rulesChanged(rules());
}
