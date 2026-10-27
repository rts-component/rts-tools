#ifndef PLOTWINDOW_H
#define PLOTWINDOW_H

#include <QHash>
#include <QMainWindow>

class PlotCanvas;
class PlotClient;
class QCheckBox;
class QComboBox;
class QLabel;
class QTimer;
class QToolButton;
class QVBoxLayout;

// 一个组的实时曲线窗口（该 CSV 行的所有曲线画在一起）。
// 布局参照常用实时曲线工具：左侧系列列表 + 深色绘图区 + 游标读数。
class PlotWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit PlotWindow(PlotClient *client, QWidget *parent = nullptr);

    void chooseGroup();

private:
    struct SeriesRow
    {
        QWidget *widget = nullptr;
        QCheckBox *visible = nullptr;
        QLabel *name = nullptr;
        QLabel *value = nullptr;
        QToolButton *chip = nullptr;
    };

    QWidget *buildHeader(QWidget *root);
    QWidget *buildSidePanel(QWidget *root);
    void connectSignals();
    void rebuildSeriesRows();
    void addHLine();
    void rebuildHLineRows();
    void changeSeriesColor(quint32 seriesId);
    void refreshValues();
    void refreshStatus();
    void refreshTitle();
    QString statusDetail() const;
    void applySuggestedPosition();
    void fitView();
    void setPaused(bool paused);

    PlotClient *m_client = nullptr;
    PlotCanvas *m_canvas = nullptr;
    QWidget *m_seriesHost = nullptr;
    QVBoxLayout *m_seriesLayout = nullptr;
    QVBoxLayout *m_hlineLayout = nullptr;
    QToolButton *m_hlineAddButton = nullptr;
    QHash<quint32, SeriesRow> m_rows;
    QCheckBox *m_autoXCheck = nullptr;
    QCheckBox *m_autoYCheck = nullptr;
    QCheckBox *m_measureCheck = nullptr;
    QCheckBox *m_measureYCheck = nullptr;
    QComboBox *m_cacheCombo = nullptr;
    QToolButton *m_pauseButton = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_connectionLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTimer *m_refreshTimer = nullptr;
    bool m_positionApplied = false;
    bool m_canvasDirty = false;
};

#endif // PLOTWINDOW_H
