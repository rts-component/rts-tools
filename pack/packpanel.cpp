#include "pack/packpanel.h"

#include "pack/packformatsdialog.h"
#include "unpack/unpackicons.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr int kSaveDelayMs = 500;

// 把控件放进居中的容器，方便放进单元格
QWidget *centered(QWidget *widget)
{
    auto *holder = new QWidget;
    auto *layout = new QHBoxLayout(holder);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setAlignment(Qt::AlignCenter);
    layout->addWidget(widget);
    return holder;
}

} // namespace

PackPanel::PackPanel(QWidget *parent)
    : QWidget(parent)
    , m_table(new QTableWidget(this))
    , m_sendCheckedButton(new QPushButton(this))
    , m_formatsButton(new QPushButton(this))
    , m_copyButton(new QPushButton(this))
    , m_addButton(new QPushButton(QStringLiteral("+"), this))
    , m_removeButton(new QPushButton(QStringLiteral("−"), this))
    , m_upButton(new QPushButton(QStringLiteral("▲"), this))
    , m_downButton(new QPushButton(QStringLiteral("▼"), this))
    , m_status(new QLabel(this))
{
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(kSaveDelayMs);
    connect(m_saveTimer, &QTimer::timeout, this, &PackPanel::save);

    m_sequenceTimer = new QTimer(this);
    m_sequenceTimer->setSingleShot(true);
    connect(m_sequenceTimer, &QTimer::timeout, this, &PackPanel::sendNextInSequence);

    m_table->setColumnCount(ColumnCount);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    QHeaderView *header = m_table->horizontalHeader();
    header->setSectionResizeMode(IndexColumn, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(NameColumn, QHeaderView::Interactive);
    header->setSectionResizeMode(DataColumn, QHeaderView::Stretch);
    header->setSectionResizeMode(HexColumn, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(DelayColumn, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(FormatColumn, QHeaderView::Interactive);
    header->setSectionResizeMode(EndingColumn, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(SendColumn, QHeaderView::ResizeToContents);
    m_table->setColumnWidth(NameColumn, 110);
    m_table->setColumnWidth(FormatColumn, 120);

    m_copyButton->setIcon(UnpackIcons::copy());
    m_copyButton->setIconSize(QSize(14, 14));
    for (QPushButton *button : {m_copyButton, m_addButton, m_removeButton, m_upButton, m_downButton}) {
        button->setObjectName(QStringLiteral("unpackToolButton"));
        button->setCursor(Qt::PointingHandCursor);
    }
    setStyleSheet(QStringLiteral("QPushButton#unpackToolButton { min-height: 0px; padding: 1px 6px; }"));
    m_status->setObjectName(QStringLiteral("packStatus"));

    auto *footer = new QHBoxLayout;
    footer->setSpacing(4);
    footer->addWidget(m_sendCheckedButton);
    footer->addWidget(m_formatsButton);
    footer->addWidget(m_status, 1);
    footer->addWidget(m_copyButton);
    footer->addWidget(m_addButton);
    footer->addWidget(m_removeButton);
    footer->addWidget(m_upButton);
    footer->addWidget(m_downButton);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    layout->addWidget(m_table, 1);
    layout->addLayout(footer);

    connect(m_addButton, &QPushButton::clicked, this, &PackPanel::addRow);
    connect(m_copyButton, &QPushButton::clicked, this, &PackPanel::copyRow);
    connect(m_removeButton, &QPushButton::clicked, this, &PackPanel::removeRow);
    connect(m_upButton, &QPushButton::clicked, this, [this] { moveRow(-1); });
    connect(m_downButton, &QPushButton::clicked, this, [this] { moveRow(1); });
    connect(m_formatsButton, &QPushButton::clicked, this, &PackPanel::editFormats);
    connect(m_sendCheckedButton, &QPushButton::clicked, this, &PackPanel::toggleSequence);
    connect(m_table, &QTableWidget::itemChanged, this, [this] {
        if (!m_loading)
            scheduleSave();
    });

    m_config = loadPackConfig();
    retranslateUi();
    QVector<PackRow> rows = m_config.rows;
    if (rows.isEmpty()) {
        PackRow row;
        row.name = tr("Send");
        rows.append(row);
    }
    setRows(rows, 0);
}

PackPanel::~PackPanel()
{
    if (m_saveTimer->isActive()) {
        m_saveTimer->stop();
        save();
    }
}

void PackPanel::retranslateUi()
{
    m_table->setHorizontalHeaderLabels({tr("No."), tr("Name"), tr("Data"), tr("Hex"), tr("Delay (ms)"),
                                        tr("Pack format"), tr("Line ending"), tr("Send")});
    m_sendCheckedButton->setText(m_sequenceTimer && m_sequenceTimer->isActive() ? tr("Stop")
                                                                                : tr("Send checked"));
    m_sendCheckedButton->setToolTip(tr("Send the checked rows in order, waiting each row's delay "
                                       "before the next one"));
    m_formatsButton->setText(tr("Formats..."));
    m_formatsButton->setToolTip(tr("Create and edit pack formats"));
    m_copyButton->setToolTip(tr("Duplicate the selected row"));
    m_addButton->setToolTip(tr("Add a row"));
    m_removeButton->setToolTip(tr("Delete the selected row"));
    m_upButton->setToolTip(tr("Move up"));
    m_downButton->setToolTip(tr("Move down"));

    // 行内的下拉文字也要刷新：保留当前行内容重建
    if (m_table->rowCount() > 0) {
        const int current = m_table->currentRow();
        setRows(allRows(), current);
    }
}

void PackPanel::setRows(const QVector<PackRow> &rows, int select)
{
    m_loading = true;
    m_table->setRowCount(0);
    m_table->setRowCount(int(rows.size()));
    for (int row = 0; row < rows.size(); ++row)
        fillRow(row, rows.at(row));
    if (select >= 0 && select < rows.size())
        m_table->selectRow(select);
    m_loading = false;
}

void PackPanel::fillRow(int row, const PackRow &data)
{
    auto *indexItem = new QTableWidgetItem(QString::number(row));
    indexItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    indexItem->setCheckState(data.checked ? Qt::Checked : Qt::Unchecked);
    m_table->setItem(row, IndexColumn, indexItem);
    m_table->setItem(row, NameColumn, new QTableWidgetItem(data.name));
    m_table->setItem(row, DataColumn, new QTableWidgetItem(data.text));

    auto *hex = new QCheckBox;
    hex->setChecked(data.hex);
    connect(hex, &QCheckBox::toggled, this, [this] { scheduleSave(); });
    m_table->setCellWidget(row, HexColumn, centered(hex));

    auto *delay = new QSpinBox;
    delay->setRange(0, 600000);
    delay->setValue(data.delayMs);
    delay->setFrame(false);
    connect(delay, &QSpinBox::valueChanged, this, [this] { scheduleSave(); });
    m_table->setCellWidget(row, DelayColumn, delay);

    auto *format = new QComboBox;
    format->addItem(tr("None"), QString());
    for (const PackFormat &item : std::as_const(m_config.formats))
        format->addItem(item.name, item.id);
    const int formatIndex = format->findData(data.formatId);
    format->setCurrentIndex(formatIndex >= 0 ? formatIndex : 0); // 格式被删掉时回到"无"
    connect(format, &QComboBox::currentIndexChanged, this, [this, format] {
        for (int r = 0; r < m_table->rowCount(); ++r) {
            if (m_table->cellWidget(r, FormatColumn) == format)
                updateDataHint(r);
        }
        scheduleSave();
    });
    m_table->setCellWidget(row, FormatColumn, format);

    auto *ending = new QComboBox;
    ending->addItem(tr("None"), int(PackEnding::None));
    ending->addItem(QStringLiteral("\\r\\n"), int(PackEnding::CrLf));
    ending->addItem(QStringLiteral("\\n"), int(PackEnding::Lf));
    ending->addItem(QStringLiteral("\\r"), int(PackEnding::Cr));
    ending->setCurrentIndex(ending->findData(int(data.ending)));
    connect(ending, &QComboBox::currentIndexChanged, this, [this] { scheduleSave(); });
    m_table->setCellWidget(row, EndingColumn, ending);

    auto *send = new QPushButton(tr("Send"));
    send->setCursor(Qt::PointingHandCursor);
    connect(send, &QPushButton::clicked, this, [this, send] {
        for (int r = 0; r < m_table->rowCount(); ++r) {
            if (m_table->cellWidget(r, SendColumn) == send) {
                sendRow(r, true);
                return;
            }
        }
    });
    m_table->setCellWidget(row, SendColumn, send);
    updateDataHint(row);
}

PackRow PackPanel::rowAt(int row) const
{
    PackRow data;
    if (const QTableWidgetItem *item = m_table->item(row, IndexColumn))
        data.checked = item->checkState() == Qt::Checked;
    if (const QTableWidgetItem *item = m_table->item(row, NameColumn))
        data.name = item->text();
    if (const QTableWidgetItem *item = m_table->item(row, DataColumn))
        data.text = item->text();
    if (const auto *hex = m_table->cellWidget(row, HexColumn)->findChild<QCheckBox *>())
        data.hex = hex->isChecked();
    if (const auto *delay = qobject_cast<QSpinBox *>(m_table->cellWidget(row, DelayColumn)))
        data.delayMs = delay->value();
    if (const auto *format = qobject_cast<QComboBox *>(m_table->cellWidget(row, FormatColumn)))
        data.formatId = format->currentData().toString();
    if (const auto *ending = qobject_cast<QComboBox *>(m_table->cellWidget(row, EndingColumn)))
        data.ending = PackEnding(ending->currentData().toInt());
    return data;
}

QVector<PackRow> PackPanel::allRows() const
{
    QVector<PackRow> rows;
    for (int row = 0; row < m_table->rowCount(); ++row)
        rows.append(rowAt(row));
    return rows;
}

void PackPanel::updateDataHint(int row)
{
    QTableWidgetItem *item = m_table->item(row, DataColumn);
    if (!item)
        return;
    const QString formatId = rowAt(row).formatId;
    for (const PackFormat &format : std::as_const(m_config.formats)) {
        if (format.id != formatId)
            continue;
        if (protoHasDataBlock(format.layout)) {
            item->setToolTip(tr("%1 value(s) for the fixed fields of \"%2\" (before / after the "
                                "payload), with the payload (hex bytes or text) in between")
                                     .arg(protoDataFieldCount(format.layout))
                                     .arg(format.name));
        } else {
            item->setToolTip(tr("%1 value(s) for the data fields of \"%2\", separated by spaces")
                                     .arg(protoDataFieldCount(format.layout))
                                     .arg(format.name));
        }
        return;
    }
    item->setToolTip(tr("Text to send (or hex bytes when Hex is checked)"));
}

void PackPanel::refreshFormatCombos()
{
    const QVector<PackRow> rows = allRows();
    setRows(rows, m_table->currentRow());
}

void PackPanel::addRow()
{
    QVector<PackRow> rows = allRows();
    rows.append(PackRow());
    setRows(rows, int(rows.size()) - 1);
    m_table->scrollToBottom();
    scheduleSave();
}

void PackPanel::copyRow()
{
    const int current = m_table->currentRow();
    if (current < 0)
        return;
    QVector<PackRow> rows = allRows();
    rows.insert(current + 1, rows.at(current));
    setRows(rows, current + 1);
    scheduleSave();
}

void PackPanel::removeRow()
{
    const int current = m_table->currentRow();
    if (current < 0)
        return;
    QVector<PackRow> rows = allRows();
    rows.removeAt(current);
    setRows(rows, qMin(current, int(rows.size()) - 1));
    scheduleSave();
}

void PackPanel::moveRow(int offset)
{
    const int current = m_table->currentRow();
    const int target = current + offset;
    if (current < 0 || target < 0 || target >= m_table->rowCount())
        return;
    QVector<PackRow> rows = allRows();
    rows.swapItemsAt(current, target);
    setRows(rows, target);
    scheduleSave();
}

void PackPanel::editFormats()
{
    PackFormatsDialog dialog(m_config.formats, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    m_config.formats = dialog.formats();
    // 被删除的格式：序号计数也清掉；行里引用它的会回到"无"
    for (auto it = m_seq.begin(); it != m_seq.end();) {
        bool exists = false;
        for (const PackFormat &format : std::as_const(m_config.formats))
            exists = exists || format.id == it.key();
        it = exists ? std::next(it) : m_seq.erase(it);
    }
    refreshFormatCombos();
    scheduleSave();
}

bool PackPanel::buildPayload(const PackRow &row, QByteArray *payload, QString *error,
                             QString *formatIdUsed) const
{
    if (formatIdUsed)
        formatIdUsed->clear();

    const PackFormat *format = nullptr;
    for (const PackFormat &item : m_config.formats) {
        if (!row.formatId.isEmpty() && item.id == row.formatId)
            format = &item;
    }

    QByteArray bytes;
    if (format) {
        if (!encodeFrame(format->layout, row.text, row.hex, m_seq.value(format->id), &bytes, error))
            return false;
        if (formatIdUsed)
            *formatIdUsed = format->id;
    } else if (row.hex) {
        if (!parseHexBytes(row.text, &bytes) || bytes.isEmpty()) {
            if (error)
                *error = tr("Enter valid hex bytes, e.g. 01 0A FF.");
            return false;
        }
    } else {
        bytes = row.text.toLocal8Bit();
        if (bytes.isEmpty() && row.ending == PackEnding::None) {
            if (error)
                *error = tr("Enter the data to send first.");
            return false;
        }
    }
    bytes.append(packEndingBytes(row.ending));
    *payload = bytes;
    return true;
}

bool PackPanel::sendRow(int row, bool showErrors)
{
    if (row < 0 || row >= m_table->rowCount())
        return false;
    const PackRow data = rowAt(row);

    QByteArray payload;
    QString error;
    QString formatId;
    if (!buildPayload(data, &payload, &error, &formatId)) {
        if (showErrors)
            QMessageBox::warning(this, tr("Send"), error);
        return false;
    }
    if (!m_sender || !m_sender(payload)) {
        if (showErrors)
            QMessageBox::information(this, tr("Serial port"), tr("Open the port first."));
        return false;
    }
    if (!formatId.isEmpty())
        m_seq[formatId] = quint8(m_seq.value(formatId) + 1);
    showStatus(tr("Sent %1 byte(s) from row %2").arg(payload.size()).arg(row));
    return true;
}

void PackPanel::toggleSequence()
{
    if (m_sequenceTimer->isActive() || !m_queue.isEmpty()) {
        stopSequence();
        return;
    }
    m_queue.clear();
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (m_table->item(row, IndexColumn)->checkState() == Qt::Checked)
            m_queue.append(row);
    }
    if (m_queue.isEmpty()) {
        showStatus(tr("Check the rows to send first"));
        return;
    }
    m_queuePos = 0;
    m_sendCheckedButton->setText(tr("Stop"));
    sendNextInSequence();
}

void PackPanel::sendNextInSequence()
{
    if (m_queuePos >= m_queue.size()) {
        stopSequence(tr("All checked rows sent"));
        return;
    }
    const int row = m_queue.at(m_queuePos++);
    if (!sendRow(row, false)) {
        // 出错就停下，并提示是哪一行
        QByteArray unused;
        QString error;
        if (!buildPayload(rowAt(row), &unused, &error, nullptr))
            QMessageBox::warning(this, tr("Send"), tr("Row %1: %2").arg(row).arg(error));
        else
            QMessageBox::information(this, tr("Serial port"), tr("Open the port first."));
        stopSequence(tr("Stopped at row %1").arg(row));
        return;
    }
    m_sequenceTimer->start(rowAt(row).delayMs);
}

void PackPanel::stopSequence(const QString &message)
{
    m_sequenceTimer->stop();
    m_queue.clear();
    m_queuePos = 0;
    m_sendCheckedButton->setText(tr("Send checked"));
    showStatus(message.isEmpty() ? tr("Stopped") : message);
}

void PackPanel::scheduleSave()
{
    if (!m_loading)
        m_saveTimer->start();
}

void PackPanel::save()
{
    m_config.rows = allRows();
    savePackConfig(m_config);
}

void PackPanel::showStatus(const QString &text)
{
    m_status->setText(text);
}
