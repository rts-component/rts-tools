#ifndef GDBREMOTEPROBE_H
#define GDBREMOTEPROBE_H

#include "flash/probe.h"

#include <QByteArray>
#include <QHash>
#include <QTcpSocket>

#include <functional>

// 通过 GDB 远程串行协议（RSP）访问目标：J-Link 的 GDB Server、openocd 等都说这个协议。
// 只实现烧录需要的那一小部分（m/M/p/P/c/D、qSupported、qRcmd、Ctrl-C）。
// 同步使用：对象和调用都应在同一个线程里。
class GdbRemoteProbe : public Probe
{
public:
    GdbRemoteProbe() = default;
    ~GdbRemoteProbe() override;

    // 连接本机 GDB Server 的端口。服务器刚启动时端口可能还没开，会在 timeoutMs 内重试；
    // alive 返回 false（服务器进程已退出）时立刻放弃
    bool open(quint16 port, int timeoutMs, const std::function<bool()> &alive = {});
    // 先 detach 再断开；可重复调用
    void close();
    bool isOpen() const { return m_socket.state() == QAbstractSocket::ConnectedState; }

    // 长时间等待（连接、等内核停下）期间会轮询它，返回 true 就放弃并报 Cancelled
    void setCancelCheck(const std::function<bool()> &check) { m_cancelCheck = check; }

    // 复位用的 monitor 命令；J-Link 是 "reset"，openocd 是 "reset halt"
    void setResetCommand(const QString &command) { m_resetCommand = command; }

    bool reset() override;
    bool halt() override;
    bool resume() override;
    bool waitHalted(int timeoutMs) override;
    bool readMemory(quint32 address, int length, QByteArray *out) override;
    bool writeMemory(quint32 address, const QByteArray &data) override;
    bool readRegister(int index, quint32 *value) override;
    bool writeRegister(int index, quint32 value) override;

    // 从 target.xml 里解析出 Probe::Register 对应的 GDB 寄存器编号（单独暴露给单元测试）；
    // 解析不出来的寄存器不在结果里，调用方沿用默认编号
    static QHash<int, int> parseRegisterMap(const QString &targetXml,
                                            const std::function<QString(const QString &)> &include = {});

    // 协议编解码（单独暴露给单元测试）
    static QByteArray encodePacket(const QByteArray &payload);
    static QByteArray escapePayload(const QByteArray &payload);
    static QByteArray decodePayload(const QByteArray &raw);

private:
    bool sendPacket(const QByteArray &payload);
    bool receivePacket(QByteArray *payload, int timeoutMs);
    bool transact(const QByteArray &payload, QByteArray *reply, int timeoutMs = 5000);
    bool monitor(const QString &command);
    bool expectOk(const QByteArray &payload);
    bool fillBuffer(int timeoutMs);
    bool interrupt();
    bool cancelled() const { return m_cancelCheck && m_cancelCheck(); }
    bool readXml(const QString &annex, QString *xml);
    void resolveRegisters();
    int registerNumber(int index) const { return m_registerMap.value(index, index); }

    QTcpSocket m_socket;
    QByteArray m_buffer;
    QString m_resetCommand = QStringLiteral("reset");
    std::function<bool()> m_cancelCheck;
    QHash<int, int> m_registerMap; // Probe::Register → GDB 寄存器编号；没有的就是同一个数
    int m_packetSize = 0x400;
    bool m_running = false;
};

#endif // GDBREMOTEPROBE_H
