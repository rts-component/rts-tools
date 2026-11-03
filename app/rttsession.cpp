#include "app/rttsession.h"

#include "common/jlinkpaths.h"
#include "flash/probeserver.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace {

constexpr quint16 kRttPort = 19021;     // JLinkExe 的 RTT telnet 端口
constexpr int kRetryIntervalMs = 250;
constexpr int kMaxAttempts = 60;        // 约 15 秒：JLinkExe 连接目标要几秒
constexpr int kMaxOutputBytes = 4096;
constexpr quint32 kAddressSearchSize = 0x400; // 指定控制块地址时，OpenOCD 只在这一小段里找

} // namespace

RttSession::RttSession(QObject *parent)
    : QObject(parent)
    , m_process(new QProcess(this))
    , m_socket(new QTcpSocket(this))
    , m_retryTimer(new QTimer(this))
{
    m_retryTimer->setInterval(kRetryIntervalMs);
    connect(m_retryTimer, &QTimer::timeout, this, &RttSession::tryConnect);

    m_process->setProcessChannelMode(QProcess::MergedChannels);
    hideConsoleWindow(m_process);
    connect(m_process, &QProcess::readyRead, this, [this] {
        m_processOutput.append(m_process->readAll());
        if (m_processOutput.size() > kMaxOutputBytes)
            m_processOutput.remove(0, m_processOutput.size() - kMaxOutputBytes);
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_active)
            fail(tr("Cannot start the RTT tool: %1").arg(m_process->errorString()));
    });
    connect(m_process, &QProcess::finished, this, [this] {
        if (!m_active)
            return;
        const QString output = QString::fromLocal8Bit(m_processOutput).trimmed();
        fail(tr("The RTT tool exited unexpectedly.\n\n%1").arg(output));
    });

    connect(m_socket, &QTcpSocket::connected, this, [this] {
        m_retryTimer->stop();
        emit connected();
    });
    connect(m_socket, &QTcpSocket::disconnected, this, [this] {
        if (m_active && !m_retryTimer->isActive())
            fail(tr("The RTT connection was closed."));
    });
}

void RttSession::hideConsoleWindow(QProcess *process)
{
    JLinkPaths::hideConsoleWindow(process);
}

RttSession::~RttSession()
{
    stop();
}

QString RttSession::findJLinkExecutable()
{
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("JLink.exe");
#else
    const QString name = QStringLiteral("JLinkExe");
#endif
    return JLinkPaths::findJLinkTool(name);
}

void RttSession::start(const RttSettings &settings)
{
    stop();

    m_active = true;
    m_attempts = 0;
    m_processOutput.clear();

    const bool started = settings.probe == RttSettings::Probe::DapLink ? startOpenOcd(settings)
                                                                       : startJLink(settings);
    if (!started) {
        m_active = false;
        return;
    }
    m_retryTimer->start();
}

bool RttSession::startJLink(const RttSettings &settings)
{
    const QString program = findJLinkExecutable();
    if (program.isEmpty()) {
        emit failed(tr("J-Link Commander was not found. Install the SEGGER J-Link software "
                       "or add it to PATH."));
        return false;
    }
    m_port = kRttPort;

    QStringList arguments = {QStringLiteral("-device"), settings.device, QStringLiteral("-if"),
                             QStringLiteral("SWD"), QStringLiteral("-speed"),
                             QStringLiteral("4000"), QStringLiteral("-NoGui"),
                             QStringLiteral("1")};
    // 自动搜索：-autoconnect 1 启动后立即连接目标，JLinkExe 连上后才会开 RTT 端口。
    // 指定地址/范围：控制块位置要在连接前设好，所以改由 stdin 依次发命令。
    if (settings.search == RttSettings::Search::Auto)
        arguments << QStringLiteral("-autoconnect") << QStringLiteral("1");
    m_process->start(program, arguments);

    if (settings.search == RttSettings::Search::Address) {
        m_process->write(QStringLiteral("exec SetRTTAddr 0x%1\n")
                                 .arg(settings.address, 0, 16)
                                 .toLatin1());
    } else if (settings.search == RttSettings::Search::Range) {
        m_process->write(QStringLiteral("exec SetRTTSearchRanges 0x%1 0x%2\n")
                                 .arg(settings.address, 0, 16)
                                 .arg(settings.size, 0, 16)
                                 .toLatin1());
    }
    if (settings.search != RttSettings::Search::Auto)
        m_process->write("connect\n");
    return true;
}

bool RttSession::startOpenOcd(const RttSettings &settings)
{
    const QString program = ProbeServer::findOpenOcd();
    if (program.isEmpty()) {
        emit failed(tr("OpenOCD not found. Install OpenOCD and make sure it is in PATH."));
        return false;
    }

    // 让系统挑一个空闲端口，避免和别的 RTT 服务冲突
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) {
        emit failed(tr("No free local port for the RTT server"));
        return false;
    }
    m_port = probe.serverPort();
    probe.close();

    // OpenOCD 要有搜索范围：自动搜索时用默认的 RAM 区域，指定地址时只在该地址处找
    quint32 address = settings.address;
    quint32 size = settings.size;
    if (settings.search == RttSettings::Search::Auto) {
        address = RttSettings().address;
        size = RttSettings().size;
    } else if (settings.search == RttSettings::Search::Address) {
        size = kAddressSearchSize;
    }
    const QStringList preInit = {QStringLiteral("gdb_port disabled")};
    const QStringList postInit = {
        QStringLiteral("rtt setup 0x%1 0x%2 \"SEGGER RTT\"").arg(address, 0, 16).arg(size, 0, 16),
        QStringLiteral("rtt start"),
        QStringLiteral("rtt server start %1 0").arg(m_port),
    };
    m_process->start(program, ProbeServer::openOcdArguments(settings.speedKHz, preInit, postInit));
    return true;
}

void RttSession::tryConnect()
{
    if (++m_attempts > kMaxAttempts) {
        const QString output = QString::fromLocal8Bit(m_processOutput).trimmed();
        fail(tr("Timed out waiting for the RTT port.\n\n%1").arg(output));
        return;
    }
    if (m_socket->state() == QAbstractSocket::UnconnectedState)
        m_socket->connectToHost(QStringLiteral("127.0.0.1"), m_port);
}

void RttSession::fail(const QString &message)
{
    stop();
    emit failed(message);
}

void RttSession::stop()
{
    m_active = false; // 先清标志，下面的 finished / disconnected 信号就不会再当成失败
    m_retryTimer->stop();

    m_socket->abort();

    if (m_process->state() != QProcess::NotRunning) {
        m_process->write("exit\n");
        if (!m_process->waitForFinished(500)) {
            m_process->kill();
            m_process->waitForFinished(500);
        }
    }
}
