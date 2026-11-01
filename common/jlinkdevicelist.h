#ifndef JLINKDEVICELIST_H
#define JLINKDEVICELIST_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;
class QStringListModel;

// J-Link 支持的芯片型号列表：由 JLinkExe 的 ExpDevList 导出成 CSV 缓存在设置目录，
// RTT 设置和 ARM Flash 页共用同一份缓存
class JLinkDeviceList : public QObject
{
    Q_OBJECT

public:
    // 芯片的主 RAM 区（取列表里的第一个非空 RAM 区域）
    struct RamArea
    {
        quint32 address = 0;
        quint32 size = 0;
        bool isValid() const { return size != 0; }
    };

    explicit JLinkDeviceList(QObject *parent = nullptr);

    // 解析 ExpDevList 的一行，如：
    //   "ST", "STM32F103C8", "Cortex-M3", {0x08000000, 0x00010000}, {0x20000000, 0x00005000}
    // 第一组是 Flash 区、第二组是 RAM 区（都是 {起始, 大小}，可以是 { {..}, {..} } 的多段）
    static bool parseLine(const QString &line, QString *device, RamArea *ram);

    // 列表里该芯片的 RAM 区；不在列表里或列表没给（通用内核名如 Cortex-M0+）时 isValid() 为 false
    RamArea ramArea(const QString &device) const;

    // 给 QCompleter 用的型号列表
    QStringListModel *model() const { return m_model; }
    bool isRefreshing() const { return m_process != nullptr; }

    // 读缓存；缓存为空就向 J-Link 要
    void load();
    // 让 JLinkExe 重新导出；没装 J-Link 时什么也不做（型号仍可手输）
    void refresh();

signals:
    void refreshingChanged(bool refreshing);

private:
    void reload();

    QStringListModel *m_model = nullptr;
    QHash<QString, RamArea> m_ram;
    QProcess *m_process = nullptr;
};

#endif // JLINKDEVICELIST_H
