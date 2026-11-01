#include "app/rttsettingsdialog.h"

#include "common/jlinkdevicelist.h"

#include <QCompleter>
#include <QStringListModel>
#include <QPushButton>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>

namespace {

bool parseHex(const QString &text, quint32 *value)
{
    bool ok = false;
    const quint32 parsed = text.trimmed().toUInt(&ok, 16); // 带不带 0x 前缀都能解析
    if (ok)
        *value = parsed;
    return ok;
}

QString formatHex(quint32 value)
{
    return QStringLiteral("0x%1").arg(value, 0, 16).toUpper().replace(QLatin1String("0X"),
                                                                       QLatin1String("0x"));
}

} // namespace

RttSettingsDialog::RttSettingsDialog(const RttSettings &settings, QWidget *parent)
    : QDialog(parent)
    , m_deviceEdit(new QLineEdit(settings.device, this))
    , m_searchCombo(new QComboBox(this))
    , m_addressEdit(new QLineEdit(formatHex(settings.address), this))
    , m_sizeEdit(new QLineEdit(formatHex(settings.size), this))
{
    setWindowTitle(tr("RTT Settings"));
    m_deviceEdit->setPlaceholderText(tr("Device, e.g. STM32F407VG"));
    m_searchCombo->addItem(tr("Auto search"));
    m_searchCombo->addItem(tr("Specified address"));
    m_searchCombo->addItem(tr("Specified address and range"));
    m_searchCombo->setCurrentIndex(static_cast<int>(settings.search));

    auto *form = new QFormLayout;
    m_refreshButton = new QPushButton(tr("Update list"), this);
    m_refreshButton->setToolTip(tr("Get the supported device list from J-Link"));
    auto *deviceRow = new QHBoxLayout;
    deviceRow->setContentsMargins(0, 0, 0, 0);
    deviceRow->addWidget(m_deviceEdit, 1);
    deviceRow->addWidget(m_refreshButton);
    form->addRow(tr("MCU model"), deviceRow);
    form->addRow(tr("Control block"), m_searchCombo);
    form->addRow(tr("Address (hex)"), m_addressEdit);
    form->addRow(tr("Range size (hex)"), m_sizeEdit);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &RttSettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &RttSettingsDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);

    connect(m_searchCombo, &QComboBox::currentIndexChanged, this,
            [this] { updateEnabled(); });
    updateEnabled();
    setMinimumWidth(420);

    // 输入时按包含关系过滤 J-Link 支持的芯片
    m_deviceList = new JLinkDeviceList(this);
    auto *completer = new QCompleter(m_deviceList->model(), this);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setMaxVisibleItems(12);
    m_deviceEdit->setCompleter(completer);
    connect(m_refreshButton, &QPushButton::clicked, m_deviceList, &JLinkDeviceList::refresh);
    connect(m_deviceList, &JLinkDeviceList::refreshingChanged, this, [this](bool refreshing) {
        m_refreshButton->setEnabled(!refreshing);
        m_refreshButton->setText(refreshing ? tr("Updating...") : tr("Update list"));
    });
    m_deviceList->load();
}

void RttSettingsDialog::updateEnabled()
{
    const int mode = m_searchCombo->currentIndex();
    m_addressEdit->setEnabled(mode != static_cast<int>(RttSettings::Search::Auto));
    m_sizeEdit->setEnabled(mode == static_cast<int>(RttSettings::Search::Range));
}

RttSettings RttSettingsDialog::settings() const
{
    RttSettings result;
    result.device = m_deviceEdit->text().trimmed();
    result.search = static_cast<RttSettings::Search>(m_searchCombo->currentIndex());
    parseHex(m_addressEdit->text(), &result.address);
    parseHex(m_sizeEdit->text(), &result.size);
    return result;
}

void RttSettingsDialog::accept()
{
    quint32 value = 0;
    const auto mode = static_cast<RttSettings::Search>(m_searchCombo->currentIndex());
    if (mode != RttSettings::Search::Auto && !parseHex(m_addressEdit->text(), &value)) {
        QMessageBox::warning(this, windowTitle(), tr("Invalid address. Enter a hex value."));
        m_addressEdit->setFocus();
        return;
    }
    if (mode == RttSettings::Search::Range
        && (!parseHex(m_sizeEdit->text(), &value) || value == 0)) {
        QMessageBox::warning(this, windowTitle(), tr("Invalid range size. Enter a hex value."));
        m_sizeEdit->setFocus();
        return;
    }
    QDialog::accept();
}
