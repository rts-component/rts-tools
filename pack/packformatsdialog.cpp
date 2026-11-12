#include "pack/packformatsdialog.h"

#include "unpack/protocoldialog.h"
#include "unpack/unpackconfig.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

PackFormatsDialog::PackFormatsDialog(const QVector<PackFormat> &formats, QWidget *parent)
    : QDialog(parent)
    , m_formats(formats)
    , m_list(new QListWidget(this))
{
    setWindowTitle(tr("Pack formats"));
    resize(360, 360);

    auto *hint = new QLabel(tr("A pack format describes a frame (header, length, data fields, "
                               "checksum, ...). Choose it on a row to send that row's values "
                               "as a frame. Double-click a name to rename it."),
                            this);
    hint->setWordWrap(true);

    auto *addButton = new QPushButton(tr("Add"), this);
    auto *editButton = new QPushButton(tr("Edit frame..."), this);
    auto *removeButton = new QPushButton(tr("Remove"), this);
    auto *buttons = new QVBoxLayout;
    buttons->addWidget(addButton);
    buttons->addWidget(editButton);
    buttons->addWidget(removeButton);
    buttons->addStretch();

    auto *body = new QHBoxLayout;
    body->addWidget(m_list, 1);
    body->addLayout(buttons);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(hint);
    layout->addLayout(body, 1);
    layout->addWidget(buttonBox);

    connect(addButton, &QPushButton::clicked, this, &PackFormatsDialog::addFormat);
    connect(editButton, &QPushButton::clicked, this, &PackFormatsDialog::editFormat);
    connect(removeButton, &QPushButton::clicked, this, &PackFormatsDialog::removeFormat);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this] { renameCurrent(); });
    connect(m_list, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        const int row = m_list->row(item);
        if (m_updating || row < 0 || row >= m_formats.size())
            return;
        const QString name = item->text().trimmed();
        if (!name.isEmpty())
            m_formats[row].name = name;
        else
            item->setText(m_formats.at(row).name); // 不允许空名字
    });
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    refreshList(m_formats.isEmpty() ? -1 : 0);
}

QString PackFormatsDialog::uniqueName(const QString &base) const
{
    auto used = [this](const QString &name) {
        for (const PackFormat &format : m_formats) {
            if (format.name == name)
                return true;
        }
        return false;
    };
    if (!used(base))
        return base;
    for (int i = 2;; ++i) {
        const QString candidate = QStringLiteral("%1 %2").arg(base).arg(i);
        if (!used(candidate))
            return candidate;
    }
}

void PackFormatsDialog::refreshList(int select)
{
    m_updating = true;
    m_list->clear();
    for (const PackFormat &format : std::as_const(m_formats)) {
        auto *item = new QListWidgetItem(format.name, m_list);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
    }
    if (select >= 0 && select < m_list->count())
        m_list->setCurrentRow(select);
    m_updating = false;
}

void PackFormatsDialog::addFormat()
{
    PackFormat format;
    format.id = newGroupId();
    format.name = uniqueName(tr("Format"));
    format.layout = defaultPackLayout();
    m_formats.append(format);
    refreshList(int(m_formats.size()) - 1);
    editFormat(); // 新建后直接进入帧定义，取消则保留示例定义
}

void PackFormatsDialog::editFormat()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_formats.size())
        return;
    ProtocolDialog dialog(m_formats.at(row).layout, this);
    if (dialog.exec() == QDialog::Accepted)
        m_formats[row].layout = dialog.layout();
}

void PackFormatsDialog::removeFormat()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_formats.size())
        return;
    m_formats.removeAt(row);
    refreshList(qMin(row, int(m_formats.size()) - 1));
}

void PackFormatsDialog::renameCurrent()
{
    if (QListWidgetItem *item = m_list->currentItem())
        m_list->editItem(item);
}
