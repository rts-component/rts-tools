#include "flash/probeserver.h"

#include "common/jlinkpaths.h"

#include <QFile>
#include <QTcpServer>

namespace {

constexpr int kMaxOutputBytes = 8192;

// 让系统挑一个空闲端口，避免和用户已经开着的 GDB Server（默认 2331）冲突
quint16 pickFreePort()
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0))
        return 0;
    return server.serverPort();
}

} // namespace

ProbeServer::~ProbeServer()
{
    stop();
}

bool ProbeServer::start(const Config &config, QString *error)
{
    stop();

#ifdef Q_OS_WIN
    const QString name = QStringLiteral("JLinkGDBServerCL.exe");
#else
    const QString name = QStringLiteral("JLinkGDBServerCLExe");
#endif
    const QString program = JLinkPaths::findJLinkTool(name);
    if (program.isEmpty()) {
        if (error)
            *error = QObject::tr("J-Link software not found (%1). Install the SEGGER J-Link package.").arg(name);
        return false;
    }

    m_port = pickFreePort();
    if (m_port == 0) {
        if (error)
            *error = QObject::tr("No free local port for the GDB server");
        return false;
    }

    // -noir：不要在连接时碰目标 RAM；-singlerun：客户端断开后自己退出；-nogui/-silent：纯命令行
    const QStringList args = {
        QStringLiteral("-device"), config.device,
        QStringLiteral("-if"), QStringLiteral("SWD"),
        QStringLiteral("-speed"), QString::number(config.speedKHz),
        QStringLiteral("-port"), QString::number(m_port),
        QStringLiteral("-localhostonly"), QStringLiteral("1"), // 只接受本机连接，别把内核读写开放给局域网
        QStringLiteral("-noir"),
        QStringLiteral("-singlerun"),
        QStringLiteral("-nogui"),
        QStringLiteral("-silent"),
    };

    m_logFile = std::make_unique<QTemporaryFile>();
    if (!m_logFile->open()) {
        if (error)
            *error = QObject::tr("Cannot create a temporary file: %1").arg(m_logFile->errorString());
        return false;
    }
    m_logFile->close();
    m_process.setProcessChannelMode(QProcess::MergedChannels);
    m_process.setStandardOutputFile(m_logFile->fileName());
    JLinkPaths::hideConsoleWindow(&m_process);
    m_process.start(program, args);
    if (!m_process.waitForStarted(5000)) {
        if (error)
            *error = QObject::tr("Cannot start %1: %2").arg(program, m_process.errorString());
        return false;
    }
    return true;
}

void ProbeServer::stop()
{
    if (m_process.state() == QProcess::NotRunning)
        return;
    // 客户端先 detach，-singlerun 的服务器会自己退出；给它一点时间，不行再杀
    if (!m_process.waitForFinished(1500)) {
        m_process.terminate();
        if (!m_process.waitForFinished(1500)) {
            m_process.kill();
            m_process.waitForFinished(1500);
        }
    }
}

bool ProbeServer::isAlive()
{
    if (m_process.state() == QProcess::NotRunning)
        return false;
    // 没有事件循环时进程状态不会自己更新，零等待查一次
    return !m_process.waitForFinished(0);
}

QString ProbeServer::output()
{
    if (!m_logFile)
        return {};
    QFile log(m_logFile->fileName());
    if (!log.open(QIODevice::ReadOnly))
        return {};
    if (log.size() > kMaxOutputBytes)
        log.seek(log.size() - kMaxOutputBytes);
    return QString::fromLocal8Bit(log.readAll());
}
