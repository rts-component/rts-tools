#include "unpack/protocoldialog.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QBrush>
#include <QColor>
#include <QVBoxLayout>

namespace {

enum Column {
    IndexColumn = 0,
    TypeColumn = 1,
    ValueColumn = 2,
    ColumnCount = 3,
};

struct TypeInfo {
    ProtoFieldType type;
    const char *label;
};

// 顶部"添加字段"按钮的顺序，对应参考界面
const TypeInfo kTypes[] = {
    {ProtoFieldType::Header, QT_TRANSLATE_NOOP("ProtocolDialog", "Header")},
    {ProtoFieldType::Seq, QT_TRANSLATE_NOOP("ProtocolDialog", "Seq")},
    {ProtoFieldType::Id, QT_TRANSLATE_NOOP("ProtocolDialog", "ID")},
    {ProtoFieldType::Length, QT_TRANSLATE_NOOP("ProtocolDialog", "Length")},
    {ProtoFieldType::Byte1, QT_TRANSLATE_NOOP("ProtocolDialog", "1Byte")},
    {ProtoFieldType::Byte2, QT_TRANSLATE_NOOP("ProtocolDialog", "2Byte")},
    {ProtoFieldType::Byte3, QT_TRANSLATE_NOOP("ProtocolDialog", "3Byte")},
    {ProtoFieldType::Byte4, QT_TRANSLATE_NOOP("ProtocolDialog", "4Byte")},
    {ProtoFieldType::Byte8, QT_TRANSLATE_NOOP("ProtocolDialog", "8Byte")},
    {ProtoFieldType::Data, QT_TRANSLATE_NOOP("ProtocolDialog", "Data")},
    {ProtoFieldType::Checksum, QT_TRANSLATE_NOOP("ProtocolDialog", "Checksum")},
    {ProtoFieldType::Tail, QT_TRANSLATE_NOOP("ProtocolDialog", "Tail")},
};

QString typeLabel(ProtoFieldType type)
{
    for (const TypeInfo &info : kTypes) {
        if (info.type == type)
            return QCoreApplication::translate("ProtocolDialog", info.label);
    }
    return QString();
}

bool isTrailer(ProtoFieldType type)
{
    return type == ProtoFieldType::Checksum || type == ProtoFieldType::Tail;
}

bool usesHexValue(ProtoFieldType type)
{
    return type == ProtoFieldType::Header || type == ProtoFieldType::Tail
            || type == ProtoFieldType::Id;
}

bool hasConversion(ProtoFieldType type)
{
    return type == ProtoFieldType::Seq || type == ProtoFieldType::Id
            || type == ProtoFieldType::Length || type == ProtoFieldType::Data
            || protoTypeIsData(type);
}

} // namespace

ProtocolDialog::ProtocolDialog(const QVector<ProtoField> &layout, QWidget *parent)
    : QDialog(parent)
    , m_layout(layout)
{
    setWindowTitle(tr("Frame format"));
    resize(760, 460);

    // 顶部：添加字段按钮
    auto *addLabel = new QLabel(tr("Add field (click a button to add the field)"), this);
    auto *buttonRow = new QHBoxLayout;
    buttonRow->setSpacing(4);
    for (const TypeInfo &info : kTypes) {
        auto *button = new QPushButton(typeLabel(info.type), this);
        const ProtoFieldType type = info.type;
        connect(button, &QPushButton::clicked, this, [this, type] { addField(type); });
        buttonRow->addWidget(button);
    }
    buttonRow->addStretch();

    // 左：字段列表 + 排序/删除
    m_table = new QTableWidget(this);
    m_table->setColumnCount(ColumnCount);
    m_table->setHorizontalHeaderLabels({tr("No."), tr("Field"), tr("Bytes")});
    m_table->horizontalHeader()->setSectionResizeMode(IndexColumn, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(TypeColumn, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ValueColumn, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);

    auto *upButton = new QPushButton(QStringLiteral("▲"), this);
    auto *downButton = new QPushButton(QStringLiteral("▼"), this);
    auto *removeButton = new QPushButton(tr("Remove"), this);
    auto *listButtons = new QHBoxLayout;
    listButtons->addWidget(upButton);
    listButtons->addWidget(downButton);
    listButtons->addWidget(removeButton);
    listButtons->addStretch();

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);

    auto *left = new QVBoxLayout;
    left->addLayout(listButtons);
    left->addWidget(m_table, 1);
    left->addWidget(m_summary);

    // 右：字段属性
    m_nameEdit = new QLineEdit(this);
    m_valueEdit = new QLineEdit(this);
    m_valueEdit->setPlaceholderText(tr("Hex bytes, e.g. AA 55"));
    m_conversionCombo = new QComboBox(this);
    m_checksumCombo = new QComboBox(this);
    m_checksumCombo->addItem(tr("Sum (8-bit)"), int(ProtoChecksum::Sum8));
    m_checksumCombo->addItem(tr("XOR (8-bit)"), int(ProtoChecksum::Xor8));
    m_checksumCombo->addItem(QStringLiteral("CRC-8"), int(ProtoChecksum::Crc8));
    m_checksumCombo->addItem(tr("Sum (16-bit)"), int(ProtoChecksum::Sum16));
    m_checksumCombo->addItem(QStringLiteral("CRC-16/MODBUS"), int(ProtoChecksum::Crc16Modbus));
    m_checksumCombo->addItem(QStringLiteral("CRC-16/CCITT-FALSE"), int(ProtoChecksum::Crc16Ccitt));
    // 范围：从 NO.x 到 NO.y（校验 / 长度共用一行，标签随字段类型变化）
    m_rangeRow = new QWidget(this);
    m_rangeFromCombo = new QComboBox(m_rangeRow);
    m_rangeToCombo = new QComboBox(m_rangeRow);
    auto *rangeLayout = new QHBoxLayout(m_rangeRow);
    rangeLayout->setContentsMargins(0, 0, 0, 0);
    rangeLayout->addWidget(new QLabel(tr("From"), m_rangeRow));
    rangeLayout->addWidget(m_rangeFromCombo, 1);
    rangeLayout->addWidget(new QLabel(tr("to"), m_rangeRow));
    rangeLayout->addWidget(m_rangeToCombo, 1);
    m_invertCheck = new QCheckBox(tr("Invert result (e.g. 0xFF - sum)"), this);
    m_bigEndianCheck = new QCheckBox(tr("High byte first"), this);
    m_showCheck = new QCheckBox(tr("Show on panel"), this);

    // 位命名（仅二进制显示）：每行一组，位写 "1:0" 或 "3"，取值名写 "0=停止;1=运行"
    m_bitRow = new QWidget(this);
    m_bitTable = new QTableWidget(0, 3, m_bitRow);
    m_bitTable->setHorizontalHeaderLabels({tr("Name"), tr("Bits"), tr("Value names")});
    m_bitTable->horizontalHeader()->setStretchLastSection(true);
    m_bitTable->verticalHeader()->setVisible(false);
    m_bitTable->setMinimumHeight(110);
    m_bitTable->setToolTip(tr("Bits: \"1:0\" for bit1..bit0, or \"3\" for one bit.\n"
                              "Value names: \"0=Stop;1=Run;2=Fault\". Unnamed values show the number."));
    auto *addBit = new QPushButton(tr("Add"), m_bitRow);
    auto *removeBit = new QPushButton(tr("Remove"), m_bitRow);
    auto *bitButtons = new QHBoxLayout;
    bitButtons->addWidget(addBit);
    bitButtons->addWidget(removeBit);
    bitButtons->addStretch();
    auto *bitLayout = new QVBoxLayout(m_bitRow);
    bitLayout->setContentsMargins(0, 0, 0, 0);
    bitLayout->addWidget(m_bitTable);
    bitLayout->addLayout(bitButtons);
    connect(addBit, &QPushButton::clicked, this, [this] { addBitGroup(); });
    connect(removeBit, &QPushButton::clicked, this, [this] { removeBitGroup(); });
    connect(m_bitTable, &QTableWidget::cellChanged, this,
            [this](int row, int column) { onBitCellChanged(row, column); });

    m_form = new QFormLayout;
    m_form->addRow(tr("Name"), m_nameEdit);
    m_form->addRow(tr("Value"), m_valueEdit);
    m_form->addRow(tr("Conversion"), m_conversionCombo);
    m_form->addRow(tr("Range"), m_rangeRow);
    m_form->addRow(tr("Algorithm"), m_checksumCombo);
    m_form->addRow(tr("Bit names"), m_bitRow);
    m_form->addRow(tr("Invert"), m_invertCheck);
    m_form->addRow(tr("Byte order"), m_bigEndianCheck);
    m_form->addRow(tr("Panel"), m_showCheck);

    m_propertyHint = new QLabel(this);
    m_propertyHint->setWordWrap(true);
    m_propertyHint->setStyleSheet(QStringLiteral("color: #8a8a8a;"));

    m_rangeHint = new QLabel(this);
    m_rangeHint->setWordWrap(true);

    auto *propertiesTitle = new QLabel(tr("Field properties"), this);
    QFont titleFont = propertiesTitle->font();
    titleFont.setBold(true);
    propertiesTitle->setFont(titleFont);

    auto *right = new QVBoxLayout;
    right->addWidget(propertiesTitle);
    right->addLayout(m_form);
    right->addWidget(m_propertyHint);
    right->addWidget(m_rangeHint);
    right->addStretch();

    auto *body = new QHBoxLayout;
    body->addLayout(left, 3);
    body->addSpacing(8);
    body->addLayout(right, 2);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    auto *layoutMain = new QVBoxLayout(this);
    layoutMain->addWidget(addLabel);
    layoutMain->addLayout(buttonRow);
    layoutMain->addLayout(body, 1);
    layoutMain->addWidget(buttonBox);

    connect(upButton, &QPushButton::clicked, this, [this] { moveCurrent(-1); });
    connect(downButton, &QPushButton::clicked, this, [this] { moveCurrent(1); });
    connect(removeButton, &QPushButton::clicked, this, &ProtocolDialog::removeCurrent);
    connect(m_table, &QTableWidget::currentCellChanged, this, [this] { onCurrentRowChanged(); });
    connect(m_nameEdit, &QLineEdit::textEdited, this, [this] { onNameEdited(); });
    connect(m_valueEdit, &QLineEdit::textEdited, this, [this] { onValueEdited(); });
    connect(m_conversionCombo, &QComboBox::activated, this, [this] { onConversionChanged(); });
    connect(m_checksumCombo, &QComboBox::activated, this, [this] { onOptionChanged(); });
    connect(m_rangeFromCombo, &QComboBox::activated, this, [this] { onOptionChanged(); });
    connect(m_rangeToCombo, &QComboBox::activated, this, [this] { onOptionChanged(); });
    connect(m_invertCheck, &QCheckBox::toggled, this, [this] { onOptionChanged(); });
    connect(m_bigEndianCheck, &QCheckBox::toggled, this, [this] { onOptionChanged(); });
    connect(m_showCheck, &QCheckBox::toggled, this, [this] { onOptionChanged(); });
    connect(buttonBox, &QDialogButtonBox::accepted, this, &ProtocolDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    refreshTable(m_layout.isEmpty() ? -1 : 0);
}

void ProtocolDialog::addField(ProtoFieldType type)
{
    // 帧头只能有一个且在最前；长度只能有一个：已有时直接选中它
    if (type == ProtoFieldType::Header || type == ProtoFieldType::Length
        || type == ProtoFieldType::Data) {
        for (int row = 0; row < m_layout.size(); ++row) {
            if (m_layout.at(row).type == type) {
                m_table->selectRow(row);
                return;
            }
        }
    }

    ProtoField field;
    field.type = type;
    switch (type) {
    case ProtoFieldType::Header:
        field.value = QByteArray::fromHex("AA");
        break;
    case ProtoFieldType::Tail:
        field.value = QByteArray::fromHex("0D0A");
        break;
    case ProtoFieldType::Id:
        field.conversion = ProtoConversion::Hex;
        field.name = tr("ID");
        break;
    case ProtoFieldType::Seq:
        field.name = tr("Seq");
        break;
    case ProtoFieldType::Length:
        field.name = tr("Length");
        break;
    case ProtoFieldType::Data:
        field.name = tr("Payload");
        field.conversion = ProtoConversion::Hex;
        break;
    case ProtoFieldType::Checksum:
        break;
    default: {
        int count = 1;
        for (const ProtoField &item : std::as_const(m_layout))
            count += protoTypeIsData(item.type) ? 1 : 0;
        field.name = tr("Data%1").arg(count);
        break;
    }
    }
    // Id / Seq / Length 默认不显示；数据字段默认显示
    if (type == ProtoFieldType::Seq || type == ProtoFieldType::Length)
        field.show = false;

    int row = int(m_layout.size());
    if (type == ProtoFieldType::Header) {
        row = 0;
    } else if (!isTrailer(type)) {
        // 数据类字段插在校验 / 帧尾之前，保持"校验在最后"
        for (int i = 0; i < m_layout.size(); ++i) {
            if (isTrailer(m_layout.at(i).type)) {
                row = i;
                break;
            }
        }
    }

    remapRanges(Edit::Insert, row);
    m_layout.insert(row, field);
    refreshTable(row);
}

void ProtocolDialog::removeCurrent()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_layout.size())
        return;
    remapRanges(Edit::Remove, row);
    m_layout.removeAt(row);
    refreshTable(qMin(row, int(m_layout.size()) - 1));
}

void ProtocolDialog::moveCurrent(int offset)
{
    const int row = m_table->currentRow();
    const int target = row + offset;
    if (row < 0 || target < 0 || target >= m_layout.size())
        return;
    remapRanges(Edit::Swap, row, target);
    m_layout.swapItemsAt(row, target);
    refreshTable(target);
}

void ProtocolDialog::refreshTable(int selectRow)
{
    const bool wasUpdating = m_updating;
    m_updating = true;
    m_table->setRowCount(0);
    for (int row = 0; row < m_layout.size(); ++row) {
        m_table->insertRow(row);
        for (int column = 0; column < ColumnCount; ++column) {
            auto *item = new QTableWidgetItem;
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            if (column == IndexColumn)
                item->setTextAlignment(Qt::AlignCenter);
            m_table->setItem(row, column, item);
        }
        refreshRow(row);
    }
    if (selectRow >= 0 && selectRow < m_layout.size())
        m_table->selectRow(selectRow);
    m_updating = wasUpdating;
    onCurrentRowChanged();
}

void ProtocolDialog::refreshRow(int row)
{
    const ProtoField &field = m_layout.at(row);
    m_table->item(row, IndexColumn)->setText(QString::number(row));
    QString label = typeLabel(field.type);
    if (protoFieldShowable(field) && !field.name.isEmpty()
        && (protoTypeIsData(field.type) || field.name != label))
        label += QStringLiteral(" · ") + field.name;
    m_table->item(row, TypeColumn)->setText(label);
    m_table->item(row, ValueColumn)->setText(protoFieldPreview(field));

    // 汇总：帧总字节数（带 Length 时按数据区声明字段估算）
    int bytes = 0;
    for (const ProtoField &item : std::as_const(m_layout))
        bytes += protoFieldSize(item);
    bool variable = false;
    for (const ProtoField &item : std::as_const(m_layout))
        variable = variable || item.type == ProtoFieldType::Data;
    m_summary->setText(variable ? tr("Declared frame size: %1 bytes + payload").arg(bytes)
                                : tr("Declared frame size: %1 bytes").arg(bytes));
}

void ProtocolDialog::onCurrentRowChanged()
{
    if (m_updating)
        return;
    loadProperties();
}

void ProtocolDialog::setRowShown(QWidget *editor, bool shown)
{
    m_form->setRowVisible(editor, shown);
}

void ProtocolDialog::populateConversions(const ProtoField &field)
{
    m_conversionCombo->clear();
    if (field.type == ProtoFieldType::Data) {
        // 变长数据段：十六进制字节或文本
        m_conversionCombo->addItem(tr("Hex bytes"), int(ProtoConversion::Hex));
        m_conversionCombo->addItem(tr("Text"), int(ProtoConversion::Text));
        return;
    }
    const int size = protoFieldSize(field);
    m_conversionCombo->addItem(tr("Unsigned"), int(ProtoConversion::UInt));
    m_conversionCombo->addItem(tr("Signed"), int(ProtoConversion::Int));
    if (size == 4)
        m_conversionCombo->addItem(tr("Float (32-bit)"), int(ProtoConversion::Float));
    else if (size == 8)
        m_conversionCombo->addItem(tr("Double (64-bit)"), int(ProtoConversion::Float));
    m_conversionCombo->addItem(tr("Hex"), int(ProtoConversion::Hex));
    m_conversionCombo->addItem(tr("Binary"), int(ProtoConversion::Binary));
}

void ProtocolDialog::loadProperties()
{
    const int row = m_table->currentRow();
    const bool valid = row >= 0 && row < m_layout.size();
    for (QWidget *editor : std::initializer_list<QWidget *>{
             m_nameEdit, m_valueEdit, m_conversionCombo, m_rangeRow, m_checksumCombo,
             m_bitRow, m_invertCheck, m_bigEndianCheck, m_showCheck})
        setRowShown(editor, false);
    m_propertyHint->clear();
    if (!valid)
        return;

    const bool wasUpdating = m_updating;
    m_updating = true;
    const ProtoField &field = m_layout.at(row);
    const int size = protoFieldSize(field);

    const bool showable = protoFieldShowable(field);
    setRowShown(m_nameEdit, showable);
    setRowShown(m_showCheck, showable);
    setRowShown(m_valueEdit, usesHexValue(field.type));
    setRowShown(m_conversionCombo, hasConversion(field.type));
    const bool hasRange = field.type == ProtoFieldType::Length || field.type == ProtoFieldType::Checksum;
    setRowShown(m_rangeRow, hasRange);
    setRowShown(m_checksumCombo, field.type == ProtoFieldType::Checksum);
    setRowShown(m_invertCheck, field.type == ProtoFieldType::Checksum);
    setRowShown(m_bitRow, hasConversion(field.type) && field.conversion == ProtoConversion::Binary);
    loadBitGroups(field);
    setRowShown(m_bigEndianCheck, size > 1 && field.type != ProtoFieldType::Header
                                          && field.type != ProtoFieldType::Tail);

    m_nameEdit->setText(field.name);
    m_valueEdit->setText(formatHexBytes(field.value));
    if (hasConversion(field.type)) {
        populateConversions(field);
        m_conversionCombo->setCurrentIndex(
                qMax(0, m_conversionCombo->findData(int(field.conversion))));
    }
    if (hasRange)
        populateRangeCombos(row);
    m_checksumCombo->setCurrentIndex(qMax(0, m_checksumCombo->findData(int(field.checksum))));
    m_invertCheck->setChecked(field.invertChecksum);
    m_bigEndianCheck->setChecked(field.bigEndian);
    m_showCheck->setChecked(field.show);

    switch (field.type) {
    case ProtoFieldType::Header:
        m_propertyHint->setText(tr("Fixed bytes that mark the start of a frame."));
        break;
    case ProtoFieldType::Id:
        m_propertyHint->setText(tr("Leave Value empty to accept any ID; otherwise only frames "
                                   "with this ID are decoded."));
        break;
    case ProtoFieldType::Length:
        m_propertyHint->setText(tr("The length value is the number of bytes of the fields in this "
                                   "range. With a Data field, the Data field takes whatever the "
                                   "length leaves over."));
        break;
    case ProtoFieldType::Data:
        m_propertyHint->setText(tr("Variable-length payload: everything in the length field's data "
                                   "area that the fixed fields around it do not use. Needs a Length "
                                   "field before it."));
        break;
    case ProtoFieldType::Checksum:
        m_propertyHint->setText(tr("Frames whose checksum does not match are dropped."));
        break;
    case ProtoFieldType::Tail:
        m_propertyHint->setText(tr("Fixed bytes at the end of a frame."));
        break;
    default:
        break;
    }
    m_updating = wasUpdating;
    updateRangeHint();
}

void ProtocolDialog::loadBitGroups(const ProtoField &field)
{
    const bool wasUpdating = m_updating;
    m_updating = true;
    m_bitTable->setRowCount(0);
    for (const ProtoBitGroup &group : field.bitGroups) {
        const int row = m_bitTable->rowCount();
        m_bitTable->insertRow(row);
        m_bitTable->setItem(row, 0, new QTableWidgetItem(group.name));
        m_bitTable->setItem(row, 1, new QTableWidgetItem(protoBitRangeText(group)));
        m_bitTable->setItem(row, 2, new QTableWidgetItem(protoBitValuesText(group)));
    }
    m_updating = wasUpdating;
}

void ProtocolDialog::addBitGroup()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_layout.size())
        return;
    ProtoBitGroup group;
    // 默认从上一组的上一位继续（bit0 起）
    for (const ProtoBitGroup &other : std::as_const(m_layout[row].bitGroups))
        group.low = group.high = qMax(group.low, other.high + 1);
    group.high = qMin(group.high, 63);
    group.low = group.high;
    m_layout[row].bitGroups.append(group);
    loadBitGroups(m_layout[row]);
    refreshRow(row);
}

void ProtocolDialog::removeBitGroup()
{
    const int row = m_table->currentRow();
    const int bitRow = m_bitTable->currentRow();
    if (row < 0 || row >= m_layout.size() || bitRow < 0 || bitRow >= m_layout[row].bitGroups.size())
        return;
    m_layout[row].bitGroups.remove(bitRow);
    loadBitGroups(m_layout[row]);
    refreshRow(row);
}

void ProtocolDialog::onBitCellChanged(int bitRow, int column)
{
    const int row = m_table->currentRow();
    if (m_updating || row < 0 || row >= m_layout.size() || bitRow < 0
        || bitRow >= m_layout[row].bitGroups.size())
        return;
    ProtoBitGroup &group = m_layout[row].bitGroups[bitRow];
    const QString text = m_bitTable->item(bitRow, column)->text();
    if (column == 0) {
        group.name = text.trimmed();
    } else if (column == 1) {
        int low = 0;
        int high = 0;
        const bool ok = protoParseBitRange(text, &low, &high);
        m_bitTable->item(bitRow, column)->setForeground(ok ? QBrush() : QBrush(QColor("#d32f2f")));
        if (ok) {
            group.low = low;
            group.high = high;
        }
    } else {
        group.valueNames = protoParseBitValues(text);
    }
    refreshRow(row);
}

void ProtocolDialog::onNameEdited()
{
    const int row = m_table->currentRow();
    if (m_updating || row < 0 || row >= m_layout.size())
        return;
    m_layout[row].name = m_nameEdit->text().trimmed();
    refreshRow(row);
}

void ProtocolDialog::onValueEdited()
{
    const int row = m_table->currentRow();
    if (m_updating || row < 0 || row >= m_layout.size())
        return;
    ProtoField &field = m_layout[row];
    QByteArray bytes;
    const bool ok = parseHexBytes(m_valueEdit->text(), &bytes);
    m_valueEdit->setStyleSheet(ok ? QString() : QStringLiteral("QLineEdit { color: #d32f2f; }"));
    if (!ok)
        return;
    // ID 只有一个字节
    if (field.type == ProtoFieldType::Id && bytes.size() > 1)
        bytes.truncate(1);
    field.value = bytes;
    refreshRow(row);
}

void ProtocolDialog::onConversionChanged()
{
    const int row = m_table->currentRow();
    if (m_updating || row < 0 || row >= m_layout.size())
        return;
    m_layout[row].conversion = ProtoConversion(m_conversionCombo->currentData().toInt());
    setRowShown(m_bitRow, m_layout[row].conversion == ProtoConversion::Binary);
}

void ProtocolDialog::onOptionChanged()
{
    const int row = m_table->currentRow();
    if (m_updating || row < 0 || row >= m_layout.size())
        return;
    ProtoField &field = m_layout[row];
    field.checksum = ProtoChecksum(m_checksumCombo->currentData().toInt());
    if (field.type == ProtoFieldType::Length || field.type == ProtoFieldType::Checksum) {
        field.rangeFrom = m_rangeFromCombo->currentData().toInt();
        field.rangeTo = m_rangeToCombo->currentData().toInt();
    }
    field.invertChecksum = m_invertCheck->isChecked();
    field.bigEndian = m_bigEndianCheck->isChecked();
    field.show = m_showCheck->isChecked();
    // 校验算法决定字节数，需要重新计算大小 / 预览；高字节选项随之显隐
    refreshRow(row);
    const bool wasUpdating = m_updating;
    m_updating = true;
    setRowShown(m_bigEndianCheck, protoFieldSize(field) > 1 && field.type != ProtoFieldType::Header
                                          && field.type != ProtoFieldType::Tail);
    m_updating = wasUpdating;
    updateRangeHint();
}

void ProtocolDialog::populateRangeCombos(int row)
{
    const ProtoField &field = m_layout.at(row);
    const bool isLength = field.type == ProtoFieldType::Length;
    if (auto *label = qobject_cast<QLabel *>(m_form->labelForField(m_rangeRow)))
        label->setText(isLength ? tr("Length range") : tr("Checksum range"));

    // 校验只能覆盖自己前面的字段；长度可以选任意字段
    const int limit = isLength ? int(m_layout.size()) : row;
    for (QComboBox *combo : {m_rangeFromCombo, m_rangeToCombo}) {
        combo->clear();
        combo->addItem(tr("Auto"), -1);
        for (int i = 0; i < limit; ++i)
            combo->addItem(tr("NO.%1 %2").arg(i).arg(typeLabel(m_layout.at(i).type)), i);
    }
    m_rangeFromCombo->setCurrentIndex(qMax(0, m_rangeFromCombo->findData(field.rangeFrom)));
    m_rangeToCombo->setCurrentIndex(qMax(0, m_rangeToCombo->findData(field.rangeTo)));
}

void ProtocolDialog::updateRangeHint()
{
    m_rangeHint->clear();
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_layout.size())
        return;
    const ProtoField &field = m_layout.at(row);
    if (field.type != ProtoFieldType::Length && field.type != ProtoFieldType::Checksum)
        return;

    int from = 0;
    int to = 0;
    if (!protoResolveRange(m_layout, row, &from, &to)) {
        m_rangeHint->setStyleSheet(QStringLiteral("color: #d32f2f;"));
        m_rangeHint->setText(tr("Invalid range"));
        return;
    }
    m_rangeHint->setStyleSheet(QString());
    if (from > to) {
        m_rangeHint->setText(tr("Currently: no fields (0 bytes)"));
        return;
    }
    int bytes = 0;
    bool variable = false;
    for (int i = from; i <= to; ++i) {
        bytes += protoFieldSize(m_layout.at(i));
        variable = variable || m_layout.at(i).type == ProtoFieldType::Data;
    }
    m_rangeHint->setText((variable ? tr("Currently: NO.%1 ~ NO.%2 (%3 bytes + payload)")
                                   : tr("Currently: NO.%1 ~ NO.%2 (%3 bytes)"))
                                 .arg(from)
                                 .arg(to)
                                 .arg(bytes));
}

void ProtocolDialog::remapRanges(Edit edit, int a, int b)
{
    for (int i = 0; i < m_layout.size(); ++i) {
        ProtoField &field = m_layout[i];
        if (field.type != ProtoFieldType::Length && field.type != ProtoFieldType::Checksum)
            continue;

        for (int pass = 0; pass < 2; ++pass) {
            const bool isTo = pass == 1;
            int &value = isTo ? field.rangeTo : field.rangeFrom;
            if (value < 0)
                continue; // 自动范围不用调整

            const int old = value;
            switch (edit) {
            case Edit::Insert:
                if (value >= a)
                    ++value;
                // 校验原本一直盖到自己前一个字段：在它前面新增的字段也要盖进去
                if (isTo && field.type == ProtoFieldType::Checksum && old == i - 1 && a == i)
                    ++value;
                break;
            case Edit::Remove:
                if (value > a)
                    --value;
                else if (value == a && isTo)
                    value = qMax(0, a - 1);
                break;
            case Edit::Swap:
                if (value == a)
                    value = b;
                else if (value == b)
                    value = a;
                break;
            }
        }
        // 两端都是具体编号时，保证从小到大
        if (field.rangeFrom >= 0 && field.rangeTo >= 0 && field.rangeFrom > field.rangeTo)
            std::swap(field.rangeFrom, field.rangeTo);
    }
}

void ProtocolDialog::accept()
{
    QString problem;
    if (!validateProtocol(m_layout, &problem)) {
        QString text;
        if (problem == QLatin1String("empty") || problem == QLatin1String("header"))
            text = tr("The frame must start with a Header that has at least one byte.");
        else if (problem == QLatin1String("header-position"))
            text = tr("Only one Header is allowed, and it must be the first field.");
        else if (problem == QLatin1String("tail"))
            text = tr("A Tail needs at least one byte.");
        else if (problem == QLatin1String("after-trailer"))
            text = tr("Checksum and Tail must come last; no data fields may follow them.");
        else if (problem == QLatin1String("data-needs-length"))
            text = tr("A Data field needs a Length field before it.");
        else if (problem == QLatin1String("data-count"))
            text = tr("Only one Data field is allowed.");
        else if (problem == QLatin1String("range"))
            text = tr("The Length / Checksum range is not valid: it must go from a lower NO. to a "
                      "higher one, and a checksum can only cover fields before itself.");
        else if (problem == QLatin1String("data-range"))
            text = tr("The Data field must be inside the range of the Length field.");
        else if (problem == QLatin1String("length"))
            text = tr("Only one Length field is allowed.");
        else
            text = problem;
        QMessageBox::warning(this, windowTitle(), text);
        return;
    }
    QDialog::accept();
}
