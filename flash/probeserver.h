#ifndef PROBESERVER_H
#define PROBESERVER_H

#include <QProcess>
#include <QString>
#include <QTemporaryFile>

#include <memory>

// 启动并管理 GDB Server 工具（SEGGER 的 JLinkGDBServerCLExe，或者 DAPLink 用的 OpenOCD），
// 我们只通过它开的本机 TCP 端口说 GDB 协议，不链接任何厂商库。
// 同步使用，和 GdbRemoteProbe 在同一个线程里。
class ProbeServer
{
public:
    enum class Kind { JLink, OpenOcdCmsisDap };

    struct Config
    {
        Kind kind = Kind::JLink;
        QString device;     // 芯片名，J-Link 必须知道（如 STM32F103C8，不确定时可用 Cortex-M3）；OpenOCD 不用
        int speedKHz = 4000;
    };

    ProbeServer() = default;
    ~ProbeServer();

    // 启动进程；找不到工具或进程起不来时返回 false 并写出原因
    bool start(const Config &config, QString *error);
    void stop();

    quint16 port() const { return m_port; }
    bool isAlive();
    // 服务器目前输出的内容（用于连接失败时说明原因）
    QString output();

private:
    QProcess m_process;
    // 服务器的输出直接写到临时文件：工作线程没有事件循环，用管道的话没人读，输出一多就会把服务器写阻塞
    std::unique_ptr<QTemporaryFile> m_logFile;
    quint16 m_port = 0;
};

#endif // PROBESERVER_H
