#ifndef RTTSESSION_H
#define RTTSESSION_H

#include <QObject>
#include <QString>
#include <QtGlobal>

class QProcess;
class QTcpSocket;
class QTimer;

// 目标芯片，以及 RTT 控制块（_SEGGER_RTT）的查找方式
struct RttSettings
{
    enum class Search {
        Auto,    // 让 J-Link 自己搜索
        Address, // 控制块在指定地址
        Range,   // 在指定地址开始的一段范围内搜索
    };

    QString device;
    Search search = Search::Auto;
    quint32 address = 0x20000000;
    quint32 size = 0x10000;
};

// J-Link RTT 数据源：启动 JLinkExe 连上目标，再连它在本机开的 RTT 端口（默认 19021）。
// 连上后 socket() 就是一个普通的 QIODevice，收发方式和串口一致。
class RttSession : public QObject
{
    Q_OBJECT

public:
    explicit RttSession(QObject *parent = nullptr);
    ~RttSession() override;

    // 启动 JLinkExe 并开始连接；结果通过 connected() / failed() 通知
    void start(const RttSettings &settings);
    // 断开并结束 JLinkExe；可重复调用
    void stop();

    // JLinkExe 的路径，找不到返回空
    static QString findJLinkExecutable();
    // Windows 下不让控制台程序弹黑窗口，其他平台什么也不做
    static void hideConsoleWindow(QProcess *process);

    bool isActive() const { return m_active; }
    QTcpSocket *socket() const { return m_socket; }

signals:
    void connected();
    void failed(const QString &message);

private:
    void tryConnect();
    void fail(const QString &message);

    QProcess *m_process = nullptr;
    QTcpSocket *m_socket = nullptr;
    QTimer *m_retryTimer = nullptr;
    QByteArray m_processOutput; // JLinkExe 的输出，失败时用来给出原因
    int m_attempts = 0;
    bool m_active = false;
};

#endif // RTTSESSION_H
