#ifndef PROBE_H
#define PROBE_H

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QVector>

// 调试探针对烧录逻辑暴露的能力：读写目标内存、读写内核寄存器、停止/运行。
// 烧录算法只通过这个接口和目标通信，所以不关心底下是 J-Link 还是别的探针。
// 所有调用都是同步的，失败时返回 false 并可从 lastError() 取原因。
class Probe
{
public:
    // 寄存器编号：R0–R15（13=SP，14=LR，15=PC）和 xPSR
    enum Register {
        R0 = 0,
        R1 = 1,
        R2 = 2,
        R3 = 3,
        R9 = 9,
        SP = 13,
        LR = 14,
        PC = 15,
        XPSR = 25,
    };

    virtual ~Probe() = default;

    // 复位并停住内核
    virtual bool reset() = 0;
    virtual bool halt() = 0;
    virtual bool resume() = 0;
    // 等待内核停下（比如执行到 BKPT）；超时返回 false，并把内核强制停住
    virtual bool waitHalted(int timeoutMs) = 0;

    virtual bool readMemory(quint32 address, int length, QByteArray *out) = 0;
    virtual bool writeMemory(quint32 address, const QByteArray &data) = 0;
    virtual bool readRegister(int index, quint32 *value) = 0;
    virtual bool writeRegister(int index, quint32 value) = 0;

    // 批量读写寄存器（逐个调用）。J-Link 的 GDB Server 不接受把多个包一起发出去的流水线方式，
    // 一次只能一问一答，所以这里不做流水线
    virtual bool writeRegisters(const QVector<QPair<int, quint32>> &values)
    {
        for (const auto &value : values) {
            if (!writeRegister(value.first, value.second))
                return false;
        }
        return true;
    }
    virtual bool readRegisters(const QVector<int> &indexes, QVector<quint32> *values)
    {
        values->clear();
        for (int index : indexes) {
            quint32 value = 0;
            if (!readRegister(index, &value))
                return false;
            values->append(value);
        }
        return true;
    }

    QString lastError() const { return m_error; }

protected:
    bool setError(const QString &message)
    {
        m_error = message;
        return false;
    }

private:
    QString m_error;
};

#endif // PROBE_H
