#ifndef SERIALPANEL_H
#define SERIALPANEL_H

#include "app/logfile.h"
#include "highlight/highlightrule.h"

#include <QByteArray>
#include <QString>
#include "app/rttsession.h"

#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui {
class SerialPanel;
}
QT_END_NAMESPACE

class QIODevice;
class QPushButton;
class QLineEdit;
class QSerialPort;
class RttSession;
class QTimer;
class ReceiveLogView;

class SerialPanel : public QWidget
{
    Q_OBJECT

public:
    explicit SerialPanel(QWidget *parent = nullptr);
    ~SerialPanel() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void setTabsFrameVisible(bool visible);

    void setupLanguageButton();
    void setupAboutButton();
    void showAbout();
    void applyUiLanguage(const QString &language);
    void updateLanguageButton();
    void retranslateUi();

    void refreshPorts();
    void updatePortComboBoxWidth();
    void togglePort();
    void closePort();
    void openRtt();
    void onPortOpened(const QString &sourceName);
    void updateSourceWidgets();
    bool isConnected() const;
    void loadRttDevice();
    void saveRttDevice() const;
    bool editRttSettings();

    void handleReadyRead();
    void flushReceiveQueue();
    QString buildLogText(const QByteArray &chunk);

    void editHighlightRules();
    void setHighlightRules(const QVector<HighlightRule> &rules);
    void updateCounters();

    void startLogFile(const QString &portName);

    void sendData();
    bool buildPayload(QByteArray &payload, QString *error) const;
    void updatePeriodicSend(bool enabled);
    void applyPeriodicInterval();
    void saveLog();
    void loadSendPanelState();
    void saveSendPanelState();

    Ui::SerialPanel *ui;
    QSerialPort *m_port = nullptr;
    RttSession *m_rtt = nullptr;
    QIODevice *m_device = nullptr; // 当前数据源：串口或 RTT 的 socket
    QPushButton *m_rttSettingsButton = nullptr;
    RttSettings m_rttSettings;
    bool m_rttConnecting = false;
    QTimer *m_flushTimer = nullptr;
    QTimer *m_periodicTimer = nullptr;
    QTimer *m_sendStateTimer = nullptr; // 发送区状态延迟落盘
    ReceiveLogView *m_logView = nullptr;
    QByteArray m_pending;      // 已接收、尚未渲染的数据
    LogFile m_logFile;
    quint64 m_rxBytes = 0;
    quint64 m_txBytes = 0;
    qint64 m_droppedBytes = 0;
    bool m_atLineStart = true;
    int m_hexColumn = 0;
};

#endif // SERIALPANEL_H
