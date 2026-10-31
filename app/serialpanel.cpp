#include "app/serialpanel.h"

#include "common/jsonstore.h"
#include "common/plotpaths.h"
#include "common/translation.h"
#include "app/receivelogview.h"
#include "app/rttsession.h"
#include "app/rttsettingsdialog.h"
#include "app/updatedialog.h"
#include "highlight/highlightdialog.h"
#include "ui_serialpanel.h"

#ifndef RTS_TOOLS_GIT_SHA
#define RTS_TOOLS_GIT_SHA ""
#endif

#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QIcon>
#include <QAction>
#include <QActionGroup>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTcpSocket>
#include <QMessageBox>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QSaveFile>
#include <QScrollBar>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QToolButton>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextStream>
#include <QTime>
#include <QTimer>

namespace {

constexpr int kFlushIntervalMs = 40;                    // 渲染节流：每 40ms 刷新一次
constexpr int kMaxPendingBytes = 4 * 1024 * 1024;       // 待渲染缓冲上限 4MB
constexpr int kMaxBytesPerFlush = 128 * 1024;           // 单次渲染上限，避免长卡顿
constexpr int kMinPeriodicIntervalMs = 10;              // 定时发送最小间隔
constexpr int kDefaultPeriodicIntervalMs = 1000;        // 定时发送默认间隔
constexpr int kHexBytesPerLine = 16;
constexpr int kMinPortComboWidth = 100;                 // 端口下拉框最小宽度
constexpr int kMaxPortComboWidth = 480;                 // 端口下拉框最大宽度，超长描述交给省略显示
const char kRttSource[] = "rtt";                      // 端口下拉框里 RTT 条目的数据值
constexpr int kSendStateSaveDelayMs = 600;              // 发送区状态延迟落盘，避免每敲一个字都写文件

QString rttFilePath()
{
    return JsonStore::settingsPath(QStringLiteral("rtt.json"));
}

// 发送区的可保存状态（Hex 发送 / 结束符 / 待发送内容）与其它设置一样放在数据目录的 settings 下
QString sendPanelFilePath()
{
    return JsonStore::settingsPath(QStringLiteral("send_panel.json"));
}

QString formatHex(const QByteArray &data, int &column)
{
    static const char digits[] = "0123456789ABCDEF";
    QByteArray out;
    out.reserve(data.size() * 3 + 8);
    for (int i = 0; i < data.size(); ++i) {
        const uchar value = uchar(data.at(i));
        out.append(digits[value >> 4]);
        out.append(digits[value & 0x0F]);
        out.append(' ');
        if (++column >= kHexBytesPerLine) {
            out.append('\n');
            column = 0;
        }
    }
    return QString::fromLatin1(out);
}

// 与 serialpanel.ui 里 addEndComboBox 的条目顺序一致
enum class LineEnding { None, CrLf, Cr, Lf };

QByteArray lineEnding(int comboIndex)
{
    switch (static_cast<LineEnding>(comboIndex)) {
    case LineEnding::CrLf:
        return QByteArrayLiteral("\r\n");
    case LineEnding::Cr:
        return QByteArrayLiteral("\r");
    case LineEnding::Lf:
        return QByteArrayLiteral("\n");
    case LineEnding::None:
        break;
    }
    return {};
}

bool isHexDigit(QChar ch)
{
    const ushort value = ch.unicode();
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')
            || (value >= 'A' && value <= 'F');
}

} // namespace

SerialPanel::SerialPanel(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::SerialPanel)
    , m_port(new QSerialPort(this))
    , m_rtt(new RttSession(this))
    , m_device(m_port)
{
    ui->setupUi(this);

    // 标题里带版本号，版本由 CMake 的 project(VERSION ...) 统一提供
    setWindowTitle(tr("rts-tools %1").arg(QCoreApplication::applicationVersion()));

    setupLanguageButton();
    setupAboutButton();
    setupUpdateButton();

    m_logView = new ReceiveLogView(ui->receiveLog, this);

    ui->baudRateComboBox->clear();
    ui->baudRateComboBox->addItems({"1200", "2400", "4800", "9600", "19200", "38400",
                                    "57600", "115200", "230400", "460800", "921600",
                                    "1500000", "2000000"});
    ui->baudRateComboBox->setCurrentText(QStringLiteral("115200"));
    ui->baudRateComboBox->setEditable(true);

    m_flushTimer = new QTimer(this);
    m_flushTimer->setSingleShot(true);
    m_flushTimer->setInterval(kFlushIntervalMs);
    connect(m_flushTimer, &QTimer::timeout, this, [this] {
        flushReceiveQueue();
    });

    m_periodicTimer = new QTimer(this);
    m_periodicTimer->setTimerType(Qt::PreciseTimer);
    connect(m_periodicTimer, &QTimer::timeout, this, &SerialPanel::sendData);

    connect(m_port, &QSerialPort::readyRead, this, &SerialPanel::handleReadyRead);
    connect(m_rtt->socket(), &QTcpSocket::readyRead, this, &SerialPanel::handleReadyRead);
    connect(m_rtt, &RttSession::connected, this, [this] {
        m_rttConnecting = false;
        m_device = m_rtt->socket();
        onPortOpened(QStringLiteral("rtt"));
    });
    connect(m_rtt, &RttSession::failed, this, [this](const QString &message) {
        closePort();
        QMessageBox::warning(this, tr("J-Link RTT"), message);
    });

    // RTT 没有波特率，选中它时波特率框换成“设置”按钮，用来填目标芯片型号
    m_rttSettingsButton = new QPushButton(this);
    m_rttSettingsButton->setMinimumHeight(28);
    m_rttSettingsButton->setMaximumWidth(120); // 放在 QSplitter 里，不限宽的话最大化时会被拉得很长
    ui->connectionBarSplitter->insertWidget(
            ui->connectionBarSplitter->indexOf(ui->baudRateComboBox) + 1, m_rttSettingsButton);
    loadRttDevice();
    connect(m_rttSettingsButton, &QPushButton::clicked, this, &SerialPanel::editRttSettings);
    connect(ui->portComboBox, &QComboBox::currentIndexChanged, this,
            [this] { updateSourceWidgets(); });
    connect(m_port, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError error) {
        if (error != QSerialPort::ResourceError && error != QSerialPort::PermissionError)
            return;
        const QString message = m_port->errorString();
        closePort();
        QMessageBox::warning(this, tr("Serial port"), message);
    });

    connect(ui->openPortButton, &QPushButton::clicked, this, &SerialPanel::togglePort);
    connect(ui->clearLogButton, &QPushButton::clicked, this, [this] {
        m_logView->clear();
        m_atLineStart = true;
        m_hexColumn = 0;
        m_droppedBytes = 0;
        // 计数（R:/T: 与 CSV 的已解析行数）跟着一起清零
        m_rxBytes = 0;
        m_txBytes = 0;
        updateCounters();
        ui->csvPanel->resetCounters();
        // Clear 同时清空自动保存的日志文件
        m_logFile.truncate();
    });
    connect(ui->saveLogButton, &QPushButton::clicked, this, &SerialPanel::saveLog);
    connect(ui->sendButton, &QPushButton::clicked, this, [this] {
        if (!isConnected()) {
            QMessageBox::information(this, tr("Serial port"), tr("Open the port first."));
            return;
        }
        sendData();
    });
    connect(ui->clearSendButton, &QPushButton::clicked, this, [this] {
        ui->sendInput->clear();
    });
    connect(ui->periodicSendCheckBox, &QCheckBox::toggled, this, &SerialPanel::updatePeriodicSend);
    connect(ui->noRefreshCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        if (!checked)
            flushReceiveQueue();
    });
    connect(ui->expandButton, &QPushButton::clicked, this, [this] {
        setTabsFrameVisible(ui->tabsFrame->isHidden());
    });

    ui->portComboBox->installEventFilter(this);
    connect(ui->portComboBox, &QComboBox::currentTextChanged, this, [this] {
        updatePortComboBoxWidth();
    });

    m_logView->setRules(loadHighlightRules(), false);
    connect(ui->highlightButton, &QPushButton::clicked, this, &SerialPanel::editHighlightRules);

    // 发送区状态（Hex 发送 / 结束符 / 待发送内容）随程序保存
    m_sendStateTimer = new QTimer(this);
    m_sendStateTimer->setSingleShot(true);
    m_sendStateTimer->setInterval(kSendStateSaveDelayMs);
    connect(m_sendStateTimer, &QTimer::timeout, this, &SerialPanel::saveSendPanelState);
    connect(ui->hexSendCheckBox, &QCheckBox::toggled, this, [this] { m_sendStateTimer->start(); });
    connect(ui->addEndComboBox, &QComboBox::currentIndexChanged, this,
            [this] { m_sendStateTimer->start(); });
    connect(ui->sendInput, &QPlainTextEdit::textChanged, this, [this] { m_sendStateTimer->start(); });
    loadSendPanelState();

    refreshPorts();
    updateCounters();
    setTabsFrameVisible(false);
}

SerialPanel::~SerialPanel()
{
    if (m_port->isOpen())
        m_port->close();
    m_rtt->stop();
    m_logFile.close();
    saveSendPanelState(); // 关掉前把还没落盘的发送区状态写一次
    delete ui;
}

bool SerialPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->portComboBox) {
        if (event->type() == QEvent::MouseButtonPress && !isConnected() && !m_rttConnecting)
            refreshPorts();
        else if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
            updatePortComboBoxWidth();
    }
    return QWidget::eventFilter(watched, event);
}

void SerialPanel::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
        // LanguageChange 只发给顶层窗口，CSV 面板要自己再刷一遍
        ui->csvPanel->retranslateUi();
    }
    QWidget::changeEvent(event);
}

void SerialPanel::setupLanguageButton()
{
    QPixmap globe(24, 24);
    globe.fill(Qt::transparent);
    QPainter painter(&globe);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(Qt::white, 1.6));
    painter.drawEllipse(QRectF(3, 3, 18, 18));
    painter.drawEllipse(QRectF(8, 3, 8, 18));
    painter.drawLine(QPointF(3.5, 12), QPointF(20.5, 12));

    ui->languageButton->setIcon(QIcon(globe));
    ui->languageButton->setIconSize(QSize(22, 22));

    QMenu *menu = new QMenu(ui->languageButton);
    QActionGroup *languageGroup = new QActionGroup(menu);
    languageGroup->setExclusive(true);
    const QList<QPair<QString, QString>> languages = {
        {QString::fromLatin1(UiLanguage::kSystem), tr("Follow system")},
        {QString::fromLatin1(UiLanguage::kChinese), QStringLiteral("简体中文")},
        {QString::fromLatin1(UiLanguage::kEnglish), QStringLiteral("English")}
    };
    for (const auto &language : languages) {
        QAction *action = menu->addAction(language.second);
        action->setData(language.first);
        action->setCheckable(true);
        languageGroup->addAction(action);
    }

    ui->languageButton->setMenu(menu);
    updateLanguageButton();
    connect(menu, &QMenu::triggered, this, [this](QAction *action) {
        applyUiLanguage(action->data().toString());
    });
}

void SerialPanel::setupAboutButton()
{
    QPixmap icon(24, 24);
    icon.fill(Qt::transparent);
    QPainter painter(&icon);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(Qt::white, 1.6));
    painter.drawEllipse(QRectF(3, 3, 18, 18));
    painter.drawPoint(QPointF(12, 7.5));
    painter.drawLine(QPointF(12, 11), QPointF(12, 17));

    ui->aboutButton->setIcon(QIcon(icon));
    ui->aboutButton->setIconSize(QSize(22, 22));
    connect(ui->aboutButton, &QToolButton::clicked, this, &SerialPanel::showAbout);
}

void SerialPanel::showAbout()
{
    const QString version = QCoreApplication::applicationVersion();
    const QString commit = QStringLiteral(RTS_TOOLS_GIT_SHA);
    const QString text =
            tr("<h3>rts-tools %1</h3>"
               "<p>Serial / J-Link RTT debugging tool with CSV parsing and live plots.</p>"
               "<p>Built with Qt %2.</p>"
               "<p>Released under the MIT License. Qt is used under the LGPLv3; "
               "see THIRD-PARTY-NOTICES.md.</p>"
               "<p><a href=\"https://github.com/rts-component/rts-tools\">"
               "github.com/rts-component/rts-tools</a></p>")
                    .arg(commit.isEmpty() ? version : version + QStringLiteral(" @") + commit,
                         QString::fromLatin1(qVersion()));
    // 不用 QMessageBox：全局样式表给它的正文设了 min-width，富文本又不自动撑宽，
    // 文字会被截掉。自己搭一个对话框，正文标签换行、宽度固定。
    QDialog dialog(this);
    dialog.setWindowTitle(tr("About rts-tools"));
    auto *label = new QLabel(text, &dialog);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setOpenExternalLinks(true);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    label->setMinimumWidth(460);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(label);
    layout->addWidget(buttons);
    dialog.exec();
}

void SerialPanel::setupUpdateButton()
{
    m_updateChecker = new UpdateChecker(this);
    connect(m_updateChecker, &UpdateChecker::finished, this, &SerialPanel::onUpdateChecked);
    connect(ui->updateButton, &QToolButton::clicked, this, [this] {
        if (m_updateAvailable)
            showUpdateDialog();
        else
            checkForUpdates(true);
    });
    updateUpdateButton();
    // 启动时静默检查一次，稍等片刻不和界面初始化抢资源
    QTimer::singleShot(3000, this, [this] { checkForUpdates(false); });
}

void SerialPanel::updateUpdateButton()
{
    QPixmap icon(24, 24);
    icon.fill(Qt::transparent);
    QPainter painter(&icon);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(Qt::white, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawEllipse(QRectF(3, 3, 18, 18));
    painter.drawLine(QPointF(12, 17), QPointF(12, 8));
    painter.drawLine(QPointF(8.5, 11.5), QPointF(12, 8));
    painter.drawLine(QPointF(15.5, 11.5), QPointF(12, 8));
    if (m_updateAvailable) {
        // 有新版本时右上角点一个红点
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#ff4d4f")));
        painter.drawEllipse(QRectF(15, 1, 8, 8));
    }
    painter.end();

    ui->updateButton->setIcon(QIcon(icon));
    ui->updateButton->setIconSize(QSize(22, 22));
    ui->updateButton->setToolTip(m_updateAvailable
                                         ? tr("New version %1 available, click to update").arg(m_latestRelease.version)
                                         : tr("Check for updates"));
}

void SerialPanel::checkForUpdates(bool manual)
{
    if (m_updateChecker->isChecking())
        return;
    m_manualUpdateCheck = manual;
    ui->updateButton->setEnabled(false);
    m_updateChecker->check();
}

void SerialPanel::onUpdateChecked(bool ok, const ReleaseInfo &info, const QString &error)
{
    ui->updateButton->setEnabled(true);
    const bool manual = m_manualUpdateCheck;
    m_manualUpdateCheck = false;

    if (!ok) {
        // 启动时的自动检查失败（离线等）不打扰用户
        if (manual)
            QMessageBox::warning(this, tr("Check for updates"),
                                 tr("Could not check for updates: %1").arg(error));
        return;
    }
    m_updateAvailable = UpdateChecker::isNewer(info.version, QCoreApplication::applicationVersion());
    if (m_updateAvailable)
        m_latestRelease = info;
    updateUpdateButton();

    if (m_updateAvailable)
        showUpdateDialog();
    else if (manual)
        QMessageBox::information(this, tr("Check for updates"),
                                 tr("You are using the latest version (%1).")
                                         .arg(QCoreApplication::applicationVersion()));
}

void SerialPanel::showUpdateDialog()
{
    UpdateDialog dialog(m_latestRelease, this);
    dialog.exec();
}

void SerialPanel::applyUiLanguage(const QString &language)
{
    if (language.isEmpty() || language == UiLanguage::saved())
        return;

    UiLanguage::save(language);
    // 安装/卸载翻译会触发 LanguageChange，界面在 changeEvent 里按新语言重译
    UiLanguage::apply(QStringLiteral("rts-tools"));
}

void SerialPanel::updateLanguageButton()
{
    const QString language = UiLanguage::saved();
    for (QAction *action : ui->languageButton->menu()->actions()) {
        const QString code = action->data().toString();
        if (code == QLatin1String(UiLanguage::kSystem))
            action->setText(tr("Follow system"));
        action->setChecked(code == language);
    }
    ui->languageButton->setToolTip(tr("Interface language"));
    ui->aboutButton->setToolTip(tr("About"));
    updateUpdateButton();
}

void SerialPanel::retranslateUi()
{
    ui->retranslateUi(this);
    setWindowTitle(tr("rts-tools %1").arg(QCoreApplication::applicationVersion()));
    ui->openPortButton->setText(isConnected() || m_rttConnecting ? tr("Close") : tr("Open"));
    // 展开/收起按钮的文字取决于当前状态，.ui 里只有默认的“展开”
    setTabsFrameVisible(!ui->tabsFrame->isHidden());
    updateLanguageButton();
    updateSourceWidgets();
    if (!isConnected() && !m_rttConnecting)
        refreshPorts(); // “没有可用串口”和 RTT 条目的文字要跟着换语言
}

void SerialPanel::setTabsFrameVisible(bool visible)
{
    ui->tabsFrame->setVisible(visible);
    ui->expandButton->setText(visible ? tr("Collapse") : tr("Expand"));
}

void SerialPanel::refreshPorts()
{
    const QString previous = ui->portComboBox->currentData().toString();
    ui->portComboBox->clear();

    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : ports) {
        const QString label = info.description().isEmpty()
                ? info.portName()
                : QStringLiteral("%1 (%2)").arg(info.portName(), info.description());
        ui->portComboBox->addItem(label, info.portName());
    }
    if (ports.isEmpty())
        ui->portComboBox->addItem(tr("No serial port"), QString());
    ui->portComboBox->addItem(tr("RTT (J-Link)"), QString::fromLatin1(kRttSource));

    const int index = ui->portComboBox->findData(previous);
    if (index >= 0)
        ui->portComboBox->setCurrentIndex(index);

    updatePortComboBoxWidth();
    updateSourceWidgets();
}

// QComboBox 默认按最长条目确定宽度，当前文本较短时右侧会留出大片空白，
// 而 QSplitter 又会把剩余空间继续分给它。这里按当前文本算出所需宽度并固定下来，
// 使下拉框随文本长短变化。
void SerialPanel::updatePortComboBoxWidth()
{
    QStyleOptionComboBox option;
    option.initFrom(ui->portComboBox);
    option.editable = true;
    option.frame = true;
    option.currentText = ui->portComboBox->currentText();

    const QFontMetrics metrics(ui->portComboBox->font());
    const QSize contentSize(metrics.horizontalAdvance(option.currentText), metrics.height());
    const int width = ui->portComboBox->style()
                              ->sizeFromContents(QStyle::CT_ComboBox, &option, contentSize,
                                                 ui->portComboBox)
                              .width();

    ui->portComboBox->setFixedWidth(qBound(kMinPortComboWidth, width, kMaxPortComboWidth));
}

bool SerialPanel::isConnected() const
{
    return m_device->isOpen();
}

void SerialPanel::loadRttDevice()
{
    const QJsonObject object = JsonStore::read(rttFilePath()).object();
    m_rttSettings.device = object.value(QLatin1String("device")).toString();
    const int search = object.value(QLatin1String("search")).toInt();
    if (search >= 0 && search <= static_cast<int>(RttSettings::Search::Range))
        m_rttSettings.search = static_cast<RttSettings::Search>(search);
    m_rttSettings.address = object.value(QLatin1String("address"))
                                    .toString()
                                    .toUInt(nullptr, 16);
    m_rttSettings.size = object.value(QLatin1String("size")).toString().toUInt(nullptr, 16);
    if (m_rttSettings.size == 0)
        m_rttSettings.size = RttSettings().size;
}

void SerialPanel::saveRttDevice() const
{
    QJsonObject object;
    object.insert(QLatin1String("device"), m_rttSettings.device);
    object.insert(QLatin1String("search"), static_cast<int>(m_rttSettings.search));
    object.insert(QLatin1String("address"), QString::number(m_rttSettings.address, 16));
    object.insert(QLatin1String("size"), QString::number(m_rttSettings.size, 16));
    JsonStore::write(rttFilePath(), QJsonDocument(object), "RTT setting");
}

// 返回 false 表示用户取消
bool SerialPanel::editRttSettings()
{
    RttSettingsDialog dialog(m_rttSettings, this);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    m_rttSettings = dialog.settings();
    saveRttDevice();
    return true;
}

// 按当前选的数据源显示波特率框或 RTT 的芯片型号框
void SerialPanel::updateSourceWidgets()
{
    const bool rtt = ui->portComboBox->currentData().toString() == QLatin1String(kRttSource);
    ui->baudRateComboBox->setVisible(!rtt);
    m_rttSettingsButton->setVisible(rtt);
    m_rttSettingsButton->setText(tr("Settings"));
}

void SerialPanel::togglePort()
{
    if (isConnected() || m_rttConnecting) {
        closePort();
        return;
    }

    const QString portName = ui->portComboBox->currentData().toString();
    if (portName == QLatin1String(kRttSource)) {
        openRtt();
        return;
    }
    bool baudOk = false;
    const qint32 baudRate = ui->baudRateComboBox->currentText().trimmed().toInt(&baudOk);
    if (portName.isEmpty() || !baudOk || baudRate <= 0) {
        QMessageBox::warning(this, tr("Serial port"),
                             tr("Select a valid port and baud rate first."));
        return;
    }

    m_port->setPortName(portName);
    m_port->setBaudRate(baudRate);
    m_port->setDataBits(QSerialPort::Data8);
    m_port->setParity(QSerialPort::NoParity);
    m_port->setStopBits(QSerialPort::OneStop);
    m_port->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_port->open(QIODevice::ReadWrite)) {
        QMessageBox::warning(this, tr("Serial port"),
                             tr("Cannot open %1: %2").arg(portName, m_port->errorString()));
        return;
    }

    m_device = m_port;
    onPortOpened(portName);
}

void SerialPanel::openRtt()
{
    if (m_rttSettings.device.isEmpty()) {
        QMessageBox::warning(this, tr("J-Link RTT"),
                             tr("Enter the target device name first, e.g. STM32F407VG."));
        editRttSettings();
        return;
    }

    // 连接要等 JLinkExe 连上目标，期间按钮显示“关闭”，点一下就取消
    m_rttConnecting = true;
    ui->openPortButton->setText(tr("Close"));
    ui->portComboBox->setEnabled(false);
    m_rttSettingsButton->setEnabled(false);
    m_rtt->start(m_rttSettings);
}

// 数据源（串口或 RTT）已连上：清计数、锁住连接参数、开日志
void SerialPanel::onPortOpened(const QString &sourceName)
{
    m_pending.clear();
    m_rxBytes = 0;
    m_txBytes = 0;
    m_droppedBytes = 0;
    m_hexColumn = 0;
    updateCounters();

    ui->openPortButton->setText(tr("Close"));
    ui->portComboBox->setEnabled(false);
    ui->baudRateComboBox->setEnabled(false);
    m_rttSettingsButton->setEnabled(false);

    // 打开端口即开始把收到的数据写入日志文件（界面暂停刷新也照写）
    startLogFile(sourceName);
}

void SerialPanel::closePort()
{
    m_periodicTimer->stop();
    ui->periodicSendCheckBox->setChecked(false);
    ui->periodicIntervalLineEdit->setEnabled(true);

    if (m_port->isOpen())
        m_port->close();
    m_rtt->stop();
    m_rttConnecting = false;
    m_device = m_port;

    // 关闭端口时把缓冲中剩余的数据显示出来
    flushReceiveQueue();
    m_logFile.close();

    ui->openPortButton->setText(tr("Open"));
    ui->portComboBox->setEnabled(true);
    ui->baudRateComboBox->setEnabled(true);
    m_rttSettingsButton->setEnabled(true);
}

void SerialPanel::handleReadyRead()
{
    const QByteArray bytes = m_device->readAll();
    if (bytes.isEmpty())
        return;

    m_rxBytes += quint64(bytes.size());
    m_pending.append(bytes);

    // CSV 解析独立于显示模式，直接吃原始字节（内部按换行拼行）
    ui->csvPanel->feed(bytes);

    if (m_pending.size() > kMaxPendingBytes) {
        const int excess = m_pending.size() - kMaxPendingBytes;
        m_pending.remove(0, excess);
        m_droppedBytes += excess;
    }

    if (!m_flushTimer->isActive())
        m_flushTimer->start();
}

void SerialPanel::flushReceiveQueue()
{
    if (m_pending.isEmpty()) {
        updateCounters();
        return;
    }

    // 即使勾选“No Refresh”，也继续取走数据写文件，避免缓冲溢出丢数据
    const int flushable = qMin(m_pending.size(), kMaxBytesPerFlush);
    const QByteArray chunk = m_pending.left(flushable);
    m_pending.remove(0, flushable);

    const bool wasAtLineStart = m_atLineStart;
    const QString text = buildLogText(chunk);
    m_logFile.write(text);
    if (!ui->noRefreshCheckBox->isChecked())
        m_logView->append(text, !wasAtLineStart);

    updateCounters();
    if (!m_pending.isEmpty())
        m_flushTimer->start();
}

QString SerialPanel::buildLogText(const QByteArray &chunk)
{
    const bool hexMode = ui->hexDisplayCheckBox->isChecked();
    QString stamp;
    if (ui->showTimeCheckBox->isChecked()) {
        stamp = QLatin1Char('[') + QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz"))
                + QLatin1String("] ");
    }
    // 带时间戳时每批从新行开始，十六进制列计数同步归零
    if (hexMode && !stamp.isEmpty())
        m_hexColumn = 0;

    QString body;
    if (hexMode) {
        body = formatHex(chunk, m_hexColumn);
    } else {
        // 串口常见 CRLF，统一成 LF 显示，避免出现多余空行
        body = QString::fromLocal8Bit(chunk);
        body.replace(QLatin1String("\r\n"), QLatin1String("\n"));
        body.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    }
    m_atLineStart = body.endsWith(QLatin1Char('\n'));

    QString text;
    if (m_droppedBytes > 0) {
        text += tr("[%1 bytes dropped]").arg(m_droppedBytes) + QLatin1Char('\n');
        m_droppedBytes = 0;
    }

    if (stamp.isEmpty()) {
        text += body;
        return text;
    }

    // 时间戳必须落在行首；若上一批停在行中，先断行
    if (!m_atLineStart && text.isEmpty() && !body.isEmpty())
        text += QLatin1Char('\n');

    if (hexMode) {
        // 十六进制模式每批只加一个时间戳，保持 16 字节行对齐
        text += stamp;
        text += body;
        return text;
    }

    int start = 0;
    while (start < body.size()) {
        const int newline = body.indexOf(QLatin1Char('\n'), start);
        const bool hasNewline = newline >= 0;
        const int lineEnd = hasNewline ? newline : body.size();
        text += stamp;
        text += body.mid(start, lineEnd - start);
        if (hasNewline)
            text += QLatin1Char('\n');
        start = lineEnd + 1;
    }
    return text;
}

void SerialPanel::editHighlightRules()
{
    HighlightDialog dialog(m_logView->rules(), this);
    connect(&dialog, &HighlightDialog::rulesChanged, this, &SerialPanel::setHighlightRules);
    dialog.exec();
}

void SerialPanel::setHighlightRules(const QVector<HighlightRule> &rules)
{
    saveHighlightRules(rules);
    m_logView->setRules(rules);
}

void SerialPanel::startLogFile(const QString &portName)
{
    QString error;
    if (!m_logFile.open(portName, &error))
        QMessageBox::warning(this, tr("Log file"), error);
}

void SerialPanel::updateCounters()
{
    ui->rxCountLabel->setText(QString::number(m_rxBytes));
    ui->txCountLabel->setText(QString::number(m_txBytes));
}

void SerialPanel::sendData()
{
    if (!isConnected()) {
        if (m_periodicTimer->isActive())
            m_periodicTimer->stop();
        ui->periodicSendCheckBox->setChecked(false);
        return;
    }

    QByteArray payload;
    QString error;
    if (!buildPayload(payload, &error)) {
        m_periodicTimer->stop();
        ui->periodicSendCheckBox->setChecked(false);
        ui->sendInput->setFocus();
        QMessageBox::warning(this, tr("Send"), error);
        return;
    }

    const qint64 written = m_device->write(payload);
    if (written > 0) {
        m_txBytes += quint64(written);
        updateCounters();
    }
}

bool SerialPanel::buildPayload(QByteArray &payload, QString *error) const
{
    const QString text = ui->sendInput->toPlainText();

    if (!ui->hexSendCheckBox->isChecked()) {
        payload = text.toLocal8Bit();
        if (payload.isEmpty()) {
            if (error)
                *error = tr("Enter the data to send first.");
            return false;
        }
        payload.append(lineEnding(ui->addEndComboBox->currentIndex()));
        return true;
    }

    QString digits;
    digits.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch.isSpace())
            continue;
        if (!isHexDigit(ch)) {
            if (error) {
                *error = tr("'%1' at position %2 is not a hex digit.\n"
                            "Hex mode accepts 0-9 and A-F only, e.g. 01 0A FF.")
                                 .arg(ch)
                                 .arg(i + 1);
            }
            return false;
        }
        digits.append(ch);
    }

    if (digits.isEmpty()) {
        if (error)
            *error = tr("Enter the hex bytes to send, e.g. 01 0A FF.");
        return false;
    }
    if (digits.size() % 2 != 0) {
        if (error)
            *error = tr("Hex input needs an even number of digits, got %1.").arg(digits.size());
        return false;
    }

    payload = QByteArray::fromHex(digits.toLatin1());
    payload.append(lineEnding(ui->addEndComboBox->currentIndex()));
    return true;
}

void SerialPanel::updatePeriodicSend(bool enabled)
{
    ui->periodicIntervalLineEdit->setEnabled(!enabled);

    if (!enabled) {
        m_periodicTimer->stop();
        return;
    }
    if (!isConnected()) {
        ui->periodicSendCheckBox->setChecked(false);
        QMessageBox::information(this, tr("Periodic send"), tr("Open the port first."));
        return;
    }

    // 定时发送前先校验一次，避免带着非法内容启动
    QByteArray probe;
    QString error;
    if (!buildPayload(probe, &error)) {
        ui->periodicSendCheckBox->setChecked(false);
        ui->sendInput->setFocus();
        QMessageBox::warning(this, tr("Periodic send"), error);
        return;
    }

    applyPeriodicInterval();
}

void SerialPanel::applyPeriodicInterval()
{
    bool ok = false;
    int interval = ui->periodicIntervalLineEdit->text().trimmed().toInt(&ok);
    if (!ok || interval < kMinPeriodicIntervalMs)
        interval = kDefaultPeriodicIntervalMs;

    const QString normalized = QString::number(interval);
    if (ui->periodicIntervalLineEdit->text() != normalized)
        ui->periodicIntervalLineEdit->setText(normalized);

    m_periodicTimer->start(interval);
    sendData();
}

void SerialPanel::saveLog()
{
    const QString logPath = m_logFile.path();
    const QString startDir = logPath.isEmpty() ? QDir::homePath()
                                               : QFileInfo(logPath).absolutePath();
    const QString path = QFileDialog::getSaveFileName(
            this, tr("Save log"), startDir + QStringLiteral("/serial-log.txt"),
            tr("Text files (*.txt);;All files (*)"));
    if (path.isEmpty())
        return;

    // 优先另存自动保存的日志（包含暂停刷新期间收到的数据）
    if (!logPath.isEmpty() && QFile::exists(logPath)) {
        QString error;
        if (!m_logFile.copyTo(path, &error))
            QMessageBox::warning(this, tr("Save log"), error);
        return;
    }

    // 没有自动日志文件时，退回导出当前界面内容
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Save log"),
                             tr("Cannot write %1: %2").arg(path, file.errorString()));
        return;
    }

    QTextStream stream(&file);
    stream << ui->receiveLog->toPlainText();
}
void SerialPanel::loadSendPanelState()
{
    const QJsonObject object = JsonStore::read(sendPanelFilePath()).object();

    {
        const QSignalBlocker blocker(ui->hexSendCheckBox);
        ui->hexSendCheckBox->setChecked(object.value(QLatin1String("hexSend")).toBool(false));
    }
    const int addEnd = object.value(QLatin1String("addEnd")).toInt(0);
    if (addEnd > 0 && addEnd < ui->addEndComboBox->count()) {
        const QSignalBlocker blocker(ui->addEndComboBox);
        ui->addEndComboBox->setCurrentIndex(addEnd);
    }
    ui->sendInput->setPlainText(object.value(QLatin1String("text")).toString());
    m_sendStateTimer->stop(); // 载入过程不算改动
}

void SerialPanel::saveSendPanelState()
{
    m_sendStateTimer->stop();

    QJsonObject object;
    object.insert(QLatin1String("hexSend"), ui->hexSendCheckBox->isChecked());
    object.insert(QLatin1String("addEnd"), ui->addEndComboBox->currentIndex());
    object.insert(QLatin1String("text"), ui->sendInput->toPlainText());

    JsonStore::write(sendPanelFilePath(), QJsonDocument(object), "send panel settings");
}
