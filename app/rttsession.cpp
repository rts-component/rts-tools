#include "app/rttsession.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTimer>

namespace {

constexpr quint16 kRttPort = 19021;     // JLinkExe 的 RTT telnet 端口
constexpr int kRetryIntervalMs = 250;
constexpr int kMaxAttempts = 60;        // 约 15 秒：JLinkExe 连接目标要几秒
constexpr int kMaxOutputBytes = 4096;

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
#ifdef Q_OS_WIN
    // JLink.exe 是控制台程序；不加这个标志，从 GUI 程序启动时会弹出一个黑窗口
    m_process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= 0x08000000; // CREATE_NO_WINDOW
    });
#endif
    connect(m_process, &QProcess::readyRead, this, [this] {
        m_processOutput.append(m_process->readAll());
        if (m_processOutput.size() > kMaxOutputBytes)
            m_processOutput.remove(0, m_processOutput.size() - kMaxOutputBytes);
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_active)
            fail(tr("Cannot start J-Link: %1").arg(m_process->errorString()));
    });
    connect(m_process, &QProcess::finished, this, [this] {
        if (!m_active)
            return;
        const QString output = QString::fromLocal8Bit(m_processOutput).trimmed();
        fail(tr("J-Link exited unexpectedly.\n\n%1").arg(output));
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
    QString path = QStandardPaths::findExecutable(name);
    if (!path.isEmpty())
        return path;

#ifdef Q_OS_WIN
    const QSettings::Format registryFormats[] = {
        QSettings::Registry64Format,
        QSettings::Registry32Format,
    };
    const QSettings::Scope registryScopes[] = {
        QSettings::UserScope,
        QSettings::SystemScope,
    };
    for (QSettings::Format format : registryFormats) {
        for (QSettings::Scope scope : registryScopes) {
            const QSettings settings(format, scope, QStringLiteral("SEGGER"),
                                     QStringLiteral("J-Link"));
            const QString installPath = settings.value(QStringLiteral("InstallPath")).toString();
            if (installPath.isEmpty())
                continue;

            const QString candidate = QDir(installPath).filePath(name);
            if (QFileInfo(candidate).isFile())
                return candidate;
        }
    }
#endif

    // 再检查 SEGGER 的默认安装位置，兼容未写入注册表的安装。
    QStringList roots;
#ifdef Q_OS_WIN
    roots << QStringLiteral("C:/Program Files/SEGGER") << QStringLiteral("C:/Program Files (x86)/SEGGER");
#elif defined(Q_OS_MACOS)
    roots << QStringLiteral("/Applications/SEGGER");
#else
    roots << QStringLiteral("/opt/SEGGER");
#endif
    for (const QString &root : roots) {
        const QDir rootDir(root);
        const QStringList dirs =
                rootDir.entryList({QStringLiteral("JLink*")}, QDir::Dirs, QDir::Name | QDir::Reversed);
        for (const QString &dir : dirs) {
            const QString candidate = rootDir.filePath(dir + QLatin1Char('/') + name);
            if (QFileInfo::exists(candidate))
                return candidate;
        }
    }
    return {};
}

void RttSession::start(const QString &device)
{
    stop();

    const QString program = findJLinkExecutable();
    if (program.isEmpty()) {
        emit failed(tr("J-Link Commander was not found. Install the SEGGER J-Link software "
                       "or add it to PATH."));
        return;
    }

    m_active = true;
    m_attempts = 0;
    m_processOutput.clear();
    // -autoconnect 1：启动后立即连接目标，JLinkExe 连上后才会开 RTT 端口
    m_process->start(program, {QStringLiteral("-device"), device, QStringLiteral("-if"),
                               QStringLiteral("SWD"), QStringLiteral("-speed"),
                               QStringLiteral("4000"), QStringLiteral("-autoconnect"),
                               QStringLiteral("1"), QStringLiteral("-NoGui"), QStringLiteral("1")});
    m_retryTimer->start();
}

void RttSession::tryConnect()
{
    if (++m_attempts > kMaxAttempts) {
        const QString output = QString::fromLocal8Bit(m_processOutput).trimmed();
        fail(tr("Timed out waiting for the J-Link RTT port.\n\n%1").arg(output));
        return;
    }
    if (m_socket->state() == QAbstractSocket::UnconnectedState)
        m_socket->connectToHost(QStringLiteral("127.0.0.1"), kRttPort);
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
