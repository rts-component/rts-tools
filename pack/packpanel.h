#ifndef PACKPANEL_H
#define PACKPANEL_H

#include "pack/packconfig.h"

#include <QHash>
#include <QWidget>

#include <functional>

class QLabel;
class QPushButton;
class QTableWidget;
class QTimer;

// 发送面板：一张可保存的"发送列表"。每行可以发文本 / Hex 字节，或选一个组包格式，
// 把数值打包成协议帧再发；可逐行发送，也可按勾选顺序连续发送（行间延时）。
class PackPanel : public QWidget
{
    Q_OBJECT

public:
    // 把字节交给串口；返回 false 表示没发出去（例如端口未打开）
    using Sender = std::function<bool(const QByteArray &)>;

    explicit PackPanel(QWidget *parent = nullptr);
    ~PackPanel() override;

    void setSender(Sender sender) { m_sender = std::move(sender); }
    void retranslateUi();

private:
    enum Column {
        IndexColumn,   // 勾选 + 序号
        NameColumn,
        DataColumn,
        HexColumn,
        DelayColumn,
        FormatColumn,
        EndingColumn,
        SendColumn,
        ColumnCount,
    };

    void setRows(const QVector<PackRow> &rows, int select = -1);
    void fillRow(int row, const PackRow &data);
    PackRow rowAt(int row) const;
    QVector<PackRow> allRows() const;
    void refreshFormatCombos();
    void updateDataHint(int row);

    void addRow();
    void copyRow();
    void removeRow();
    void moveRow(int offset);
    void editFormats();

    // 按一行的设置生成要发送的字节；用到了组包格式时 formatIdUsed 返回它的 id（发送成功后推进序号）
    bool buildPayload(const PackRow &row, QByteArray *payload, QString *error,
                      QString *formatIdUsed) const;
    bool sendRow(int row, bool showErrors);
    void toggleSequence();
    void sendNextInSequence();
    void stopSequence(const QString &message = QString());

    void scheduleSave();
    void save();
    void showStatus(const QString &text);

    PackConfig m_config;            // 只有 formats 是权威数据；行以表格为准
    QHash<QString, quint8> m_seq;   // 每个格式的帧序号计数
    Sender m_sender;
    bool m_loading = false;

    QTableWidget *m_table = nullptr;
    QPushButton *m_sendCheckedButton = nullptr;
    QPushButton *m_formatsButton = nullptr;
    QPushButton *m_copyButton = nullptr;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_removeButton = nullptr;
    QPushButton *m_upButton = nullptr;
    QPushButton *m_downButton = nullptr;
    QLabel *m_status = nullptr;
    QTimer *m_saveTimer = nullptr;
    QTimer *m_sequenceTimer = nullptr;
    QVector<int> m_queue;
    int m_queuePos = 0;
};

#endif // PACKPANEL_H
