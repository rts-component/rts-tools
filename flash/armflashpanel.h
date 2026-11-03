#ifndef ARMFLASHPANEL_H
#define ARMFLASHPANEL_H

#include "common/jlinkdevicelist.h"
#include "flash/flashworker.h"

#include <QElapsedTimer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QThread;

// ARM Flash 烧录页：选固件和 FLM 算法，经 J-Link 把算法下载到目标 RAM 里执行完成烧录/擦除/校验。
// 实际工作在 FlashWorker（独立线程）里，这里只负责输入检查、状态和日志显示。
class ArmFlashPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ArmFlashPanel(QWidget *parent = nullptr);
    ~ArmFlashPanel() override;

    // 切换界面语言后重新设置所有界面文字
    void retranslateUi();

signals:
    void startRequested(const FlashRequest &request);

private:
    void browseFirmware();
    void browseFlm();
    void updateAddressVisible();
    bool isBinFirmware() const;
    bool isDapLink() const;
    JLinkDeviceList::RamArea ramAreaFromList(const QString &device) const;
    void updateProbeFields();
    void updateRamFields();
    void startOperation(FlashRequest::Operation operation);
    bool buildRequest(FlashRequest::Operation operation, FlashRequest *request, QString *error) const;
    void onFinished(bool success, const QString &message);
    void setBusy(bool busy);
    void appendLog(const QString &text);
    void loadConfig();
    void saveConfig() const;

    QLabel *m_firmwareLabel = nullptr;
    QLineEdit *m_firmwareEdit = nullptr;
    QPushButton *m_browseButton = nullptr;
    QLabel *m_flmLabel = nullptr;
    QLineEdit *m_flmEdit = nullptr;
    QPushButton *m_flmBrowseButton = nullptr;
    QLabel *m_probeLabel = nullptr;
    QComboBox *m_probeCombo = nullptr;
    QComboBox *m_speedCombo = nullptr;
    QLabel *m_chipLabel = nullptr;
    QLineEdit *m_chipEdit = nullptr;
    QPushButton *m_chipRefreshButton = nullptr;
    JLinkDeviceList *m_deviceList = nullptr;
    QLabel *m_addressLabel = nullptr;
    QLineEdit *m_addressEdit = nullptr;
    QLabel *m_ramAddressLabel = nullptr;
    QLabel *m_ramSizeLabel = nullptr;
    QLineEdit *m_ramAddressEdit = nullptr;
    QLineEdit *m_ramSizeEdit = nullptr;
    QCheckBox *m_runAfterCheck = nullptr;
    QPushButton *m_programButton = nullptr;
    QPushButton *m_eraseButton = nullptr;
    QPushButton *m_verifyButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QProgressBar *m_progress = nullptr;
    QPlainTextEdit *m_log = nullptr;

    QThread *m_thread = nullptr;
    FlashWorker *m_worker = nullptr;
    // 手填的 RAM（芯片在 J-Link 列表里时输入框显示列表值且不可改，切走时靠这两个还原）
    QString m_manualRamAddress;
    QString m_manualRamSize;
    QElapsedTimer m_elapsed; // 一次任务从点击到结束的耗时
    bool m_busy = false;
};

#endif // ARMFLASHPANEL_H
