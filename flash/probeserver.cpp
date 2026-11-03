#include "flash/probeserver.h"

#include "common/jlinkpaths.h"

#include <QCoreApplication>
#include <QFile>
#include <QStandardPaths>
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

// -noir：不要在连接时碰目标 RAM；-singlerun：客户端断开后自己退出；-nogui/-silent：纯命令行
QStringList jlinkArgs(const ProbeServer::Config &config, quint16 port)
{
    return {
        QStringLiteral("-device"), config.device,
        QStringLiteral("-if"), QStringLiteral("SWD"),
        QStringLiteral("-speed"), QString::number(config.speedKHz),
        QStringLiteral("-port"), QString::number(port),
        QStringLiteral("-localhostonly"), QStringLiteral("1"), // 只接受本机连接，别把内核读写开放给局域网
        QStringLiteral("-noir"),
        QStringLiteral("-singlerun"),
        QStringLiteral("-nogui"),
        QStringLiteral("-silent"),
    };
}

// DAPLink（CMSIS-DAP）：不带芯片专用脚本，直接建一个通用的 Cortex-M 目标，
// 擦写交给我们自己下载的 FLM 算法，所以关掉 OpenOCD 自己的 flash 处理；detach 后退出（相当于 -singlerun）
QStringList openOcdArgs(const ProbeServer::Config &config, quint16 port)
{
    const QStringList commands = {
        QStringLiteral("gdb_port %1").arg(port),
        QStringLiteral("bindto 127.0.0.1"), // 只接受本机连接
        QStringLiteral("telnet_port disabled"),
        QStringLiteral("tcl_port disabled"),
        QStringLiteral("transport select swd"),
        QStringLiteral("adapter speed %1").arg(config.speedKHz),
        // 没有 srst 线，靠 SYSRESETREQ 做真正的系统复位；少了这两行，OpenOCD 只复位内核，
        // 外设（时钟树、Flash 控制器）还留着应用程序配好的状态，算法写 Flash 会悄悄失败
        QStringLiteral("reset_config srst_nogate"),
        QStringLiteral("swd newdap chip cpu -enable"),
        QStringLiteral("dap create chip.dap -chain-position chip.cpu"),
        QStringLiteral("target create chip.cpu cortex_m -dap chip.dap"),
        QStringLiteral("chip.cpu cortex_m reset_config sysresetreq"),
        QStringLiteral("gdb_memory_map disable"),
        QStringLiteral("gdb_flash_program disable"),
        QStringLiteral("chip.cpu configure -event gdb-detach { shutdown }"),
        QStringLiteral("chip.cpu configure -event examine-end { catch {mmw 0xE0042004 0x00000307 0} }"),
        QStringLiteral("init"),
    };
    QStringList args = {QStringLiteral("-f"), QStringLiteral("interface/cmsis-dap.cfg")};
    for (const QString &command : commands)
        args << QStringLiteral("-c") << command;
    return args;
}

QString findOpenOcd()
{
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("openocd.exe");
#else
    const QString name = QStringLiteral("openocd");
#endif
    // 先找随程序带的，再查 PATH
    const QString bundled = QCoreApplication::applicationDirPath() + QStringLiteral("/openocd/bin");
    const QString path = QStandardPaths::findExecutable(name, {bundled});
    return path.isEmpty() ? QStandardPaths::findExecutable(name) : path;
}

} // namespace

ProbeServer::~ProbeServer()
{
    stop();
}

bool ProbeServer::start(const Config &config, QString *error)
{
    stop();

    const bool openOcd = config.kind == Kind::OpenOcdCmsisDap;
    QString program;
    if (openOcd) {
        program = findOpenOcd();
        if (program.isEmpty()) {
            if (error)
                *error = QObject::tr("OpenOCD not found. Install OpenOCD and make sure it is in PATH.");
            return false;
        }
    } else {
#ifdef Q_OS_WIN
        const QString name = QStringLiteral("JLinkGDBServerCL.exe");
#else
        const QString name = QStringLiteral("JLinkGDBServerCLExe");
#endif
        program = JLinkPaths::findJLinkTool(name);
        if (program.isEmpty()) {
            if (error)
                *error = QObject::tr("J-Link software not found (%1). Install the SEGGER J-Link package.").arg(name);
            return false;
        }
    }

    m_port = pickFreePort();
    if (m_port == 0) {
        if (error)
            *error = QObject::tr("No free local port for the GDB server");
        return false;
    }

    const QStringList args = openOcd ? openOcdArgs(config, m_port) : jlinkArgs(config, m_port);

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
