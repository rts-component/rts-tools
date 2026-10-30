#ifndef RTTSESSION_H
#define RTTSESSION_H

#include <QObject>
#include <QString>

class QProcess;
class QTcpSocket;
class QTimer;

// J-Link RTT 数据源：启动 JLinkExe 连上目标，再连它在本机开的 RTT 端口（默认 19021）。
// 连上后 socket() 就是一个普通的 QIODevice，收发方式和串口一致。
class RttSession : public QObject
{
    Q_OBJECT

public:
    explicit RttSession(QObject *parent = nullptr);
    ~RttSession() override;

    // 启动 JLinkExe 并开始连接；结果通过 connected() / failed() 通知
    void start(const QString &device);
    // 断开并结束 JLinkExe；可重复调用
    void stop();

    bool isActive() const { return m_active; }
    QTcpSocket *socket() const { return m_socket; }

signals:
    void connected();
    void failed(const QString &message);

private:
    void tryConnect();
    void fail(const QString &message);
    static QString findJLinkExecutable();

    QProcess *m_process = nullptr;
    QTcpSocket *m_socket = nullptr;
    QTimer *m_retryTimer = nullptr;
    QByteArray m_processOutput; // JLinkExe 的输出，失败时用来给出原因
    int m_attempts = 0;
    bool m_active = false;
};

#endif // RTTSESSION_H
