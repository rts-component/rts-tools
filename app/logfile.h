#ifndef LOGFILE_H
#define LOGFILE_H

#include <QCoreApplication>
#include <QFile>
#include <QString>

// 串口接收内容的自动落盘文件：<数据目录>/logs/<端口>_<时间>.log。
// 打开端口即创建，界面暂停刷新期间收到的数据也照写。
class LogFile
{
    Q_DECLARE_TR_FUNCTIONS(LogFile)

public:
    ~LogFile();

    // 失败时返回 false 并在 error 里给出原因
    bool open(const QString &portName, QString *error);
    void close();
    bool isOpen() const { return m_file.isOpen(); }
    QString path() const { return m_path; }

    void write(const QString &text);
    void truncate(); // “清空日志”时一并清空文件
    void flush();

    // 把整个日志文件复制到 target；失败时返回 false 并在 error 里给出原因
    bool copyTo(const QString &target, QString *error);

private:
    QFile m_file;
    QString m_path;
};

#endif // LOGFILE_H
