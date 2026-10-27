#ifndef CSVPANEL_H
#define CSVPANEL_H

#include "csv/csvparser.h"

#include <QByteArray>
#include <QVector>
#include <QWidget>

class CsvRowList;
class PlotLauncher;
class PlotServer;
class QCheckBox;
class QLabel;
class QPushButton;
class QTimer;

// 按参考界面组织的 CSV 解析面板：
// 上方是若干“行”（每行一套分隔符/列名，列数匹配才更新），
// 勾选曲线的数字列会把采样点推给 PlotServer（每个系列一个 rts-plot）。
class CsvPanel : public QWidget
{
    Q_OBJECT

public:
    explicit CsvPanel(QWidget *parent = nullptr);

    // 串口原始数据；内部按换行拼行
    void feed(const QByteArray &bytes);
    void clearData();
    // 只把"已解析/已跳过"的计数归零，保留当前显示的值
    void resetCounters();
    // 切换界面语言后重新设置所有界面文字
    void retranslateUi();

    // 曲线：当前生效的系列（行名/列名 + 颜色），供其它界面展示
    int curveSeriesCount() const;

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void addGroup();
    void copySelectedGroup();
    void removeSelectedGroup();
    void moveSelectedGroup(int offset);
    void editGroup(int index);
    void selectGroup(int index);

    void rebuildGroups();
    void refreshGroup(int index);
    void applyPendingValues();
    void updateStatus();
    void processLine(const QString &line);
    void saveConfig();
    void showGroupMenu(int index, const QPoint &globalPos);

    void rebuildCurveKeys();
    void syncCurveSeries();
    void showHint(const QString &text);
    void placeHint();

    // 行名自动生成：Unnamed1、Unnamed2 …… 保证每行都有唯一名字
    bool isGroupNameUsed(const QString &name) const;
    QString nextAutoGroupName() const;
    QString uniqueGroupName(const QString &base) const;

    CsvParseConfig m_config;
    QStringList m_rowIds;                 // 与 m_values 等逐行对应的行 id，重建时据此保留各行的值
    QVector<QStringList> m_values;
    QVector<QStringList> m_pendingValues; // 每行各自的最新值
    QVector<bool> m_pendingDirty;
    QVector<QStringList> m_curveKeys;     // 每行每个字段对应的曲线 key，空串表示不画
    PlotServer *m_plotServer = nullptr;
    PlotLauncher *m_plotLauncher = nullptr;

    QByteArray m_buffer;
    int m_selectedIndex = -1;
    quint64 m_parsedLines = 0;
    quint64 m_skippedLines = 0;

    CsvRowList *m_list = nullptr;
    QLabel *m_headerIndex = nullptr;
    QLabel *m_headerPanel = nullptr;
    QLabel *m_headerCurve = nullptr;
    QLabel *m_placeholder = nullptr;
    QCheckBox *m_parseCheckBox = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_copyButton = nullptr;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_removeButton = nullptr;
    QPushButton *m_upButton = nullptr;
    QPushButton *m_downButton = nullptr;
    QTimer *m_updateTimer = nullptr;
    QLabel *m_hint = nullptr;
    QTimer *m_hintTimer = nullptr;
};

#endif // CSVPANEL_H
