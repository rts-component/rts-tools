#ifndef FLASHWORKER_H
#define FLASHWORKER_H

#include <QObject>
#include <QString>

#include <atomic>

// 一次烧录/擦除/校验任务的参数
struct FlashRequest
{
    enum class Operation { Program, Erase, Verify };

    Operation operation = Operation::Program;
    QString firmwarePath;
    QString flmPath;
    enum class ProbeType { JLink, DapLink };

    ProbeType probe = ProbeType::JLink;
    QString device;                   // J-Link 需要的芯片名（DAPLink 不用）
    quint32 binAddress = 0x08000000;  // .bin 固件的写入地址（.hex/.elf 自带地址）
    quint32 ramAddress = 0x20000000;  // 下载算法用的 RAM 起点
    quint32 ramSize = 0;              // 0 = 按算法需要的大小，不检查总容量
    int speedKHz = 4000;
    bool runAfter = true;             // 烧录/校验完成后复位让程序跑起来
};

// 在工作线程里完整执行一次任务：启动 GDB Server → 连接 → 下载算法 → 擦除/编程/校验 → 收尾。
// 用法：moveToThread 后用排队连接调用 run()，cancel() 可以在任意线程调用。
class FlashWorker : public QObject
{
    Q_OBJECT

public:
    explicit FlashWorker(QObject *parent = nullptr);

    // 取消请求。要在发起 run() 之前先 resetCancel()，这样 run() 排队期间点的取消不会丢
    void cancel() { m_cancel = true; }
    void resetCancel() { m_cancel = false; }

public slots:
    void run(const FlashRequest &request);

signals:
    void log(const QString &text);
    void progress(int percent);
    void finished(bool success, const QString &message);

private:
    std::atomic_bool m_cancel{false};
};

Q_DECLARE_METATYPE(FlashRequest)

#endif // FLASHWORKER_H
