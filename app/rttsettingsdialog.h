#ifndef RTTSETTINGSDIALOG_H
#define RTTSETTINGSDIALOG_H

#include "app/rttsession.h"

#include <QDialog>

class QComboBox;
class QLineEdit;
class JLinkDeviceList;
class QPushButton;

// RTT 设置：目标芯片型号，以及 RTT 控制块的查找方式
class RttSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RttSettingsDialog(const RttSettings &settings, QWidget *parent = nullptr);

    RttSettings settings() const;

private:
    void updateEnabled();
    void accept() override;

    QComboBox *m_probeCombo = nullptr;
    QComboBox *m_speedCombo = nullptr;
    QLineEdit *m_deviceEdit = nullptr;
    QComboBox *m_searchCombo = nullptr;
    QLineEdit *m_addressEdit = nullptr;
    QLineEdit *m_sizeEdit = nullptr;
    QPushButton *m_refreshButton = nullptr;
    JLinkDeviceList *m_deviceList = nullptr;
};

#endif // RTTSETTINGSDIALOG_H
