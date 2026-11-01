#include "flash/armflashpanel.h"

#include "common/jlinkdevicelist.h"
#include "common/jsonstore.h"

#include <QCheckBox>
#include <QCompleter>
#include <QStringListModel>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardItemModel>
#include <QThread>
#include <QVBoxLayout>

namespace {

QString configPath()
{
    return JsonStore::settingsPath(QStringLiteral("arm_flash.json"));
}

QString hexText(quint32 value)
{
    return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0'));
}

// 支持 0x 前缀的十六进制和十进制
bool parseNumber(const QString &text, quint32 *value)
{
    bool ok = false;
    const QString trimmed = text.trimmed();
    const quint32 parsed = trimmed.toUInt(&ok, trimmed.startsWith(QLatin1String("0x"), Qt::CaseInsensitive) ? 16 : 10);
    if (ok)
        *value = parsed;
    return ok;
}

} // namespace

ArmFlashPanel::ArmFlashPanel(QWidget *parent)
    : QWidget(parent)
{
    qRegisterMetaType<FlashRequest>("FlashRequest");

    m_firmwareLabel = new QLabel(this);
    m_firmwareEdit = new QLineEdit(this);
    m_browseButton = new QPushButton(this);

    m_flmLabel = new QLabel(this);
    m_flmEdit = new QLineEdit(this);
    m_flmBrowseButton = new QPushButton(this);

    m_probeLabel = new QLabel(this);
    m_probeCombo = new QComboBox(this);
    m_probeCombo->addItems({QStringLiteral("J-Link"), QStringLiteral("CMSIS-DAP"),
                            QStringLiteral("ST-Link")});
    // 目前只有 J-Link 后端；其余先灰掉
    if (auto *model = qobject_cast<QStandardItemModel *>(m_probeCombo->model())) {
        for (int i = 1; i < model->rowCount(); ++i)
            model->item(i)->setEnabled(false);
    }

    m_chipLabel = new QLabel(this);
    m_chipEdit = new QLineEdit(this);
    m_chipRefreshButton = new QPushButton(this);
    m_addressLabel = new QLabel(this);
    m_addressEdit = new QLineEdit(this);
    m_ramAddressLabel = new QLabel(this);
    m_ramSizeLabel = new QLabel(this);
    m_ramAddressEdit = new QLineEdit(this);
    m_ramSizeEdit = new QLineEdit(this);
    m_runAfterCheck = new QCheckBox(this);
    m_runAfterCheck->setChecked(true);

    m_programButton = new QPushButton(this);
    m_eraseButton = new QPushButton(this);
    m_verifyButton = new QPushButton(this);
    m_cancelButton = new QPushButton(this);
    m_cancelButton->setEnabled(false);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);

    auto *form = new QGridLayout;
    form->setColumnStretch(1, 1);
    form->addWidget(m_firmwareLabel, 0, 0);
    form->addWidget(m_firmwareEdit, 0, 1, 1, 2);
    form->addWidget(m_browseButton, 0, 3);
    form->addWidget(m_flmLabel, 1, 0);
    form->addWidget(m_flmEdit, 1, 1, 1, 2);
    form->addWidget(m_flmBrowseButton, 1, 3);
    form->addWidget(m_probeLabel, 2, 0);
    form->addWidget(m_probeCombo, 2, 1, 1, 3);
    form->addWidget(m_chipLabel, 3, 0);
    form->addWidget(m_chipEdit, 3, 1, 1, 2);
    form->addWidget(m_chipRefreshButton, 3, 3);
    form->addWidget(m_addressLabel, 4, 0);
    form->addWidget(m_addressEdit, 4, 1, 1, 3);
    form->addWidget(m_ramAddressLabel, 5, 0);
    form->addWidget(m_ramAddressEdit, 5, 1, 1, 3);
    form->addWidget(m_ramSizeLabel, 6, 0);
    form->addWidget(m_ramSizeEdit, 6, 1, 1, 3);
    form->addWidget(m_runAfterCheck, 7, 1, 1, 3);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_programButton);
    buttons->addWidget(m_eraseButton);
    buttons->addWidget(m_verifyButton);
    buttons->addWidget(m_cancelButton);
    buttons->addStretch(1);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addLayout(buttons);
    layout->addWidget(m_progress);
    layout->addWidget(m_log, 1);

    // 输入时按包含关系过滤 J-Link 支持的芯片（和 RTT 设置共用同一份列表）
    m_deviceList = new JLinkDeviceList(this);
    auto *completer = new QCompleter(m_deviceList->model(), this);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setMaxVisibleItems(12);
    m_chipEdit->setCompleter(completer);
    connect(m_chipRefreshButton, &QPushButton::clicked, m_deviceList, &JLinkDeviceList::refresh);
    connect(m_deviceList, &JLinkDeviceList::refreshingChanged, this, [this](bool refreshing) {
        m_chipRefreshButton->setEnabled(!refreshing);
        m_chipRefreshButton->setText(refreshing ? tr("Updating...") : tr("Update list"));
    });
    m_deviceList->load();
    connect(m_chipEdit, &QLineEdit::textChanged, this, &ArmFlashPanel::updateRamFields);
    connect(m_ramAddressEdit, &QLineEdit::textEdited, this, [this](const QString &text) { m_manualRamAddress = text; });
    connect(m_ramSizeEdit, &QLineEdit::textEdited, this, [this](const QString &text) { m_manualRamSize = text; });
    connect(m_deviceList->model(), &QAbstractItemModel::modelReset, this, &ArmFlashPanel::updateRamFields);

    m_thread = new QThread(this);
    m_worker = new FlashWorker;
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(this, &ArmFlashPanel::startRequested, m_worker, &FlashWorker::run);
    connect(m_worker, &FlashWorker::log, this, &ArmFlashPanel::appendLog);
    connect(m_worker, &FlashWorker::progress, m_progress, &QProgressBar::setValue);
    connect(m_worker, &FlashWorker::finished, this, &ArmFlashPanel::onFinished);
    m_thread->start();

    connect(m_firmwareEdit, &QLineEdit::textChanged, this, &ArmFlashPanel::updateAddressVisible);
    connect(m_browseButton, &QPushButton::clicked, this, &ArmFlashPanel::browseFirmware);
    connect(m_flmBrowseButton, &QPushButton::clicked, this, &ArmFlashPanel::browseFlm);
    connect(m_programButton, &QPushButton::clicked, this,
            [this] { startOperation(FlashRequest::Operation::Program); });
    connect(m_eraseButton, &QPushButton::clicked, this,
            [this] { startOperation(FlashRequest::Operation::Erase); });
    connect(m_verifyButton, &QPushButton::clicked, this,
            [this] { startOperation(FlashRequest::Operation::Verify); });
    connect(m_cancelButton, &QPushButton::clicked, this, [this] {
        m_cancelButton->setEnabled(false);
        m_worker->cancel();
    });

    loadConfig();
    updateAddressVisible();
    updateRamFields();
    retranslateUi();
}

ArmFlashPanel::~ArmFlashPanel()
{
    saveConfig();
    if (m_busy)
        m_worker->cancel();
    m_thread->quit();
    // 取消在连接、等内核停下和擦写循环里都会很快响应；等不到就强制结束，免得带着运行中的线程析构直接中止程序
    if (!m_thread->wait(30000)) {
        m_thread->terminate();
        m_thread->wait(5000);
    }
}

// 已选了固件、且不是自带地址的 .hex/.elf/.axf，就当 .bin 处理，需要用户给出写入地址
bool ArmFlashPanel::isBinFirmware() const
{
    const QString path = m_firmwareEdit->text().trimmed();
    if (path.isEmpty())
        return false;
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix != QLatin1String("hex") && suffix != QLatin1String("elf")
            && suffix != QLatin1String("axf");
}

// 起始地址只在 .bin 时才显示
void ArmFlashPanel::updateAddressVisible()
{
    const bool needed = isBinFirmware();
    m_addressLabel->setVisible(needed);
    m_addressEdit->setVisible(needed);
}

void ArmFlashPanel::browseFirmware()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select firmware"), m_firmwareEdit->text(),
        tr("Firmware (*.bin *.hex *.elf *.axf);;All files (*)"));
    if (!path.isEmpty())
        m_firmwareEdit->setText(path);
}

void ArmFlashPanel::browseFlm()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select flash algorithm"), m_flmEdit->text(),
        tr("Flash algorithm (*.FLM *.flm);;All files (*)"));
    if (!path.isEmpty())
        m_flmEdit->setText(path);
}

bool ArmFlashPanel::buildRequest(FlashRequest::Operation operation, FlashRequest *request,
                                 QString *error) const
{
    using Operation = FlashRequest::Operation;
    FlashRequest r;
    r.operation = operation;
    r.firmwarePath = m_firmwareEdit->text().trimmed();
    r.flmPath = m_flmEdit->text().trimmed();
    r.device = m_chipEdit->text().trimmed();
    r.runAfter = m_runAfterCheck->isChecked();

    if (operation != Operation::Erase && !QFile::exists(r.firmwarePath)) {
        *error = tr("Select an existing firmware file");
        return false;
    }
    if (operation != Operation::Verify && !QFile::exists(r.flmPath)) {
        *error = tr("Select an existing FLM file");
        return false;
    }
    if (r.device.isEmpty()) {
        *error = tr("Enter the chip name (for example STM32F103C8)");
        return false;
    }
    // 只有 .bin 才需要起始地址；.hex/.elf 自带地址；擦除不用固件
    if (operation != Operation::Erase && isBinFirmware()) {
        if (m_addressEdit->text().trimmed().isEmpty()) {
            *error = tr("Enter the flash address for the .bin file");
            return false;
        }
        if (!parseNumber(m_addressEdit->text(), &r.binAddress)) {
            *error = tr("BIN flash address is not a valid number");
            return false;
        }
    }

    // RAM：芯片在 J-Link 列表里就用列表的值（界面上不可改）；否则用手填的，
    // 没填起始地址就用默认起点，没填大小就不限（按算法实际需要）
    const JLinkDeviceList::RamArea listed = m_deviceList->ramArea(r.device);
    if (listed.isValid()) {
        r.ramAddress = listed.address;
        r.ramSize = listed.size;
    } else {
        const QString addressText = m_manualRamAddress.trimmed();
        const QString sizeText = m_manualRamSize.trimmed();
        if (!addressText.isEmpty() && !parseNumber(addressText, &r.ramAddress)) {
            *error = tr("RAM start address is not a valid number");
            return false;
        }
        if (!sizeText.isEmpty() && (!parseNumber(sizeText, &r.ramSize) || r.ramSize == 0)) {
            *error = tr("RAM size is not a valid number");
            return false;
        }
    }
    *request = r;
    return true;
}

void ArmFlashPanel::updateRamFields()
{
    const JLinkDeviceList::RamArea listed = m_deviceList->ramArea(m_chipEdit->text().trimmed());
    if (listed.isValid()) {
        m_ramAddressEdit->setText(hexText(listed.address));
        m_ramSizeEdit->setText(hexText(listed.size));
    } else {
        m_ramAddressEdit->setText(m_manualRamAddress);
        m_ramSizeEdit->setText(m_manualRamSize);
    }
    m_ramAddressEdit->setPlaceholderText(tr("Default: %1").arg(hexText(0x20000000)));
    m_ramSizeEdit->setPlaceholderText(tr("Default: as needed"));
    const bool editable = !m_busy && !listed.isValid();
    m_ramAddressEdit->setEnabled(editable);
    m_ramSizeEdit->setEnabled(editable);
    m_ramAddressLabel->setEnabled(editable);
    m_ramSizeLabel->setEnabled(editable);
}

void ArmFlashPanel::startOperation(FlashRequest::Operation operation)
{
    if (m_busy)
        return;
    FlashRequest request;
    QString error;
    if (!buildRequest(operation, &request, &error)) {
        QMessageBox::warning(this, tr("ARM Flash"), error);
        return;
    }
    saveConfig();
    m_log->clear();
    m_progress->setValue(0);
    m_worker->resetCancel(); // 必须在入队之前，这样排队期间点的取消不会被 run() 清掉
    m_elapsed.start();
    setBusy(true);
    emit startRequested(request);
}

void ArmFlashPanel::onFinished(bool success, const QString &message)
{
    setBusy(false);
    appendLog((success ? tr("Done: ") : tr("Failed: ")) + message);
    appendLog(tr("Elapsed time: %1 s").arg(m_elapsed.elapsed() / 1000.0, 0, 'f', 2));
}

void ArmFlashPanel::setBusy(bool busy)
{
    m_busy = busy;
    for (QWidget *widget : {static_cast<QWidget *>(m_programButton), static_cast<QWidget *>(m_eraseButton),
                            static_cast<QWidget *>(m_verifyButton), static_cast<QWidget *>(m_browseButton),
                            static_cast<QWidget *>(m_flmBrowseButton), static_cast<QWidget *>(m_firmwareEdit),
                            static_cast<QWidget *>(m_flmEdit), static_cast<QWidget *>(m_chipEdit),
                            static_cast<QWidget *>(m_chipRefreshButton),
                            static_cast<QWidget *>(m_addressEdit), static_cast<QWidget *>(m_runAfterCheck)})
        widget->setEnabled(!busy);
    updateAddressVisible();
    updateRamFields();
    m_cancelButton->setEnabled(busy);
}

void ArmFlashPanel::appendLog(const QString &text)
{
    m_log->appendPlainText(text);
}

void ArmFlashPanel::loadConfig()
{
    const QJsonObject o = JsonStore::read(configPath()).object();
    if (o.isEmpty())
        return;
    m_firmwareEdit->setText(o.value(QStringLiteral("firmware")).toString());
    m_flmEdit->setText(o.value(QStringLiteral("flm")).toString());
    m_chipEdit->setText(o.value(QStringLiteral("chip")).toString());
    m_addressEdit->setText(o.value(QStringLiteral("address")).toString(m_addressEdit->text()));
    m_manualRamAddress = o.value(QStringLiteral("ramAddress")).toString();
    m_manualRamSize = o.value(QStringLiteral("ramSize")).toString();
    m_runAfterCheck->setChecked(o.value(QStringLiteral("runAfter")).toBool(true));
}

void ArmFlashPanel::saveConfig() const
{
    QJsonObject o;
    o.insert(QStringLiteral("firmware"), m_firmwareEdit->text());
    o.insert(QStringLiteral("flm"), m_flmEdit->text());
    o.insert(QStringLiteral("chip"), m_chipEdit->text());
    o.insert(QStringLiteral("address"), m_addressEdit->text());
    o.insert(QStringLiteral("ramAddress"), m_manualRamAddress);
    o.insert(QStringLiteral("ramSize"), m_manualRamSize);
    o.insert(QStringLiteral("runAfter"), m_runAfterCheck->isChecked());
    JsonStore::write(configPath(), QJsonDocument(o), "ARM Flash setting");
}

void ArmFlashPanel::retranslateUi()
{
    m_firmwareLabel->setText(tr("Firmware"));
    m_firmwareEdit->setPlaceholderText(tr("Path to .bin / .hex / .elf"));
    m_browseButton->setText(tr("Browse..."));
    m_flmLabel->setText(tr("FLM file"));
    m_flmEdit->setPlaceholderText(tr("Path to flash algorithm (.FLM)"));
    m_flmBrowseButton->setText(tr("Browse..."));
    m_probeLabel->setText(tr("Debug probe"));
    m_chipLabel->setText(tr("Chip"));
    m_chipEdit->setPlaceholderText(tr("e.g. STM32F103C8, or Cortex-M0+ if not listed"));
    m_chipRefreshButton->setText(m_deviceList->isRefreshing() ? tr("Updating...") : tr("Update list"));
    m_chipRefreshButton->setToolTip(tr("Get the supported device list from J-Link"));
    m_addressLabel->setText(tr("BIN flash address"));
    m_addressEdit->setPlaceholderText(hexText(0x08000000));
    m_addressEdit->setToolTip(tr("Address in flash where the .bin file is written"));
    m_ramAddressLabel->setText(tr("RAM start address"));
    m_ramSizeLabel->setText(tr("RAM size"));
    const QString ramTip = tr("RAM used to run the flash algorithm. Taken from the J-Link device list when "
                              "the chip is listed (read-only); otherwise enter it manually");
    m_ramAddressEdit->setToolTip(ramTip);
    m_ramSizeEdit->setToolTip(ramTip);
    updateRamFields();
    m_runAfterCheck->setText(tr("Reset and run after finishing"));
    m_programButton->setText(tr("Program"));
    m_eraseButton->setText(tr("Erase"));
    m_verifyButton->setText(tr("Verify"));
    m_cancelButton->setText(tr("Cancel"));
    m_log->setPlaceholderText(tr("Flash output will appear here"));
}
