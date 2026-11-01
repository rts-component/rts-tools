#ifndef FLASHALGO_H
#define FLASHALGO_H

#include "flash/firmwareimage.h"
#include "flash/flmfile.h"
#include "flash/probe.h"

#include <QString>
#include <QVector>

#include <functional>

// 烧录过程中向界面汇报的回调；任何一个都可以不设
struct FlashCallbacks
{
    std::function<void(const QString &)> log;
    std::function<void(int)> progress;   // 0–100
    std::function<bool()> cancelled;     // 返回 true 表示用户取消
};

// 在目标上执行 FLM 里的 Flash 算法：把算法下载到 RAM，设置寄存器后让内核跑函数，
// 函数返回到 RAM 开头的 BKPT 时内核停下，再从 R0 取返回值。
// 约定见 ARM 的 Flash 编程算法规范（CMSIS-Pack Flash Algorithm）。
class FlashAlgo
{
public:
    FlashAlgo(Probe &probe, const FlmFile &flm, quint32 ramAddress, quint32 ramSize,
              const FlashCallbacks &callbacks);

    // 检查 RAM 是否放得下算法/栈/页缓冲，并把算法写进 RAM
    bool load();
    // 擦除固件覆盖的扇区后逐页编程（跳过全空页），整个过程不校验
    bool program(const QVector<FirmwareSegment> &segments);
    // 整片擦除；算法没有 EraseChip 时逐扇区擦
    bool eraseChip();

    QString error() const { return m_error; }

    // 把目标内存和固件逐字节比对，不需要算法；失败时 error 里带第一个不同的地址
    static bool verify(Probe &probe, const QVector<FirmwareSegment> &segments,
                       const FlashCallbacks &callbacks, QString *error);

private:
    enum Function { Erase = 1, Program = 2, Verify = 3 };

    // args 依次放进 R0–R3，只写给出的那几个
    bool call(int entryOffset, const QVector<quint32> &args, int timeoutMs, quint32 *result);
    bool initAlgo(Function function);
    bool uninitAlgo(Function function);
    bool eraseSectors(const QVector<quint32> &sectors, int *done, int total);
    bool fail(const QString &message);
    void log(const QString &text) const;
    void reportProgress(int done, int total) const;
    bool isCancelled() const;

    Probe &m_probe;
    const FlmFile &m_flm;
    quint32 m_ramAddress;
    quint32 m_ramSize;
    FlashCallbacks m_callbacks;

    quint32 m_codeAddress = 0;   // 算法镜像在 RAM 里的起始地址
    quint32 m_staticBase = 0;    // 传给 R9
    quint32 m_stackTop = 0;
    quint32 m_bufferAddress = 0; // 页缓冲区
    QString m_error;
};

#endif // FLASHALGO_H
