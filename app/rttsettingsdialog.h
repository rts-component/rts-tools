#ifndef RTTSETTINGSDIALOG_H
#define RTTSETTINGSDIALOG_H

#include "app/rttsession.h"

#include <QDialog>

class QComboBox;
class QLineEdit;
class QProcess;
class QPushButton;
class QStringListModel;

// J-Link RTT 设置：目标芯片型号，以及 RTT 控制块的查找方式
class RttSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RttSettingsDialog(const RttSettings &settings, QWidget *parent = nullptr);

    RttSettings settings() const;

private:
    void updateEnabled();
    void loadDeviceList();    // 读缓存；没有缓存就向 J-Link 要
    void refreshDeviceList(); // 让 JLinkExe 导出支持的芯片列表
    void accept() override;

    QLineEdit *m_deviceEdit = nullptr;
    QComboBox *m_searchCombo = nullptr;
    QLineEdit *m_addressEdit = nullptr;
    QLineEdit *m_sizeEdit = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QStringListModel *m_deviceModel = nullptr;
    QProcess *m_listProcess = nullptr;
};

#endif // RTTSETTINGSDIALOG_H
