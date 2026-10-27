#ifndef PLOTCANVAS_H
#define PLOTCANVAS_H

#include "plot/viewer/plotclient.h"

#include <QColor>
#include <QPixmap>
#include <QPoint>
#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

// 曲线绘图区：横轴是样本 index，纵轴是数值；负责缩放/平移/测量线/水平参考线与光标读数。
class PlotCanvas : public QWidget
{
    Q_OBJECT

public:
    explicit PlotCanvas(PlotClient *client, QWidget *parent = nullptr);

    // 水平参考线：用于对照分析曲线，叠画在曲线上方
    struct HLine
    {
        QString name;
        double value = 0.0;
        QColor color;
        bool visible = true;
    };
    static constexpr int kMaxHLines = 5;
    const QVector<HLine> &hlines() const { return m_hlines; }
    bool addHLine(const HLine &line);
    void removeHLine(int index);
    void setHLine(int index, const HLine &line);

    // 当前视口 Y 范围的中点，新加的水平线放在这里，一定看得见
    double viewCenterY() const;

    // AutoFit 表示“全部数据塞进视口”；follow 表示“右边缘跟随最新数据”
    void setAutoFitX(bool on);
    bool autoFitX() const { return m_autoFit; }
    bool following() const { return m_follow; }
    void setAutoY(bool on);
    bool autoY() const { return m_autoY; }
    void fit();

    // 暂停时的“铺满”：把当前全部数据放进视口后冻结，之后到达的新数据不再改变画面
    void fitFrozen();
    void setSeriesHidden(quint32 id, bool hidden);
    void setPaused(bool paused);

    // 测量竖线：只在暂停时可用，两根线可以用鼠标拖动，显示两线之间的 index 差和时间差
    void setMeasure(bool on);

    // Y 方向测量：两根水平线（按数值定位），显示两线之间的数值差
    void setMeasureY(bool on);
    double pixelSpacing() const { return m_pixelSpacing; }
    int stride() const { return m_stride; }
    double viewMin() const { return m_viewMin; }
    double viewMax() const { return m_viewMax; }

    // 视口右边缘之外还有多少个新点
    qint64 hiddenRight() const { return m_hiddenRight; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct Range
    {
        double xMin = 0;
        double xMax = 0;
        double yMin = 0;
        double yMax = 0;
        bool valid = false;
    };

    // 影响曲线画面的全部输入（数据、视口、显示开关、尺寸）的摘要
    quint64 cacheKey() const;
    double visibleLimit(double xMax) const;
    QRectF plotArea() const;
    bool seriesVisible(const PlotClient::SeriesData &series) const;
    Range computeRange();
    double mapX(double index, const QRectF &area, const Range &range) const;
    double mapY(double value, const QRectF &area, const Range &range) const;
    void drawGrid(QPainter &painter, const QRectF &area, const Range &range) const;
    void drawSeries(QPainter &painter, const QRectF &area, const Range &range) const;

    // 可见曲线里最小/最大的 index（测量线只能拖在有数据的范围内）
    bool dataIndexRange(double *low, double *high) const;

    // 鼠标 x 附近（6 像素内）的测量线，没有返回 -1；两根重合时优先后一根，方便拖开
    int measureLineAt(double x) const;

    // 鼠标 y 附近（6 像素内）的 Y 测量线，没有返回 -1；两根重合时优先后一根
    int measureHLineAt(double y) const;

    // 某个 index 对应的采集时刻（所有曲线共用时间轴，取任意一条可见曲线即可）
    bool timestampOf(double index, qint64 *timestampMs) const;
    void drawMeasure(QPainter &painter, const QRectF &area, const Range &range) const;
    void drawMeasureY(QPainter &painter, const QRectF &area, const Range &range) const;
    void drawHLines(QPainter &painter, const QRectF &area, const Range &range) const;

    // 游标处的采样点：按序号取最近的样本
    bool sampleAt(quint32 index, SampleView::Point *point) const;
    void drawCursor(QPainter &painter, const QRectF &area, const Range &range) const;

    PlotClient *m_client = nullptr;
    QSet<quint32> m_hidden;
    QVector<HLine> m_hlines;
    bool m_measure = false;
    double m_measureAt[2] = { 0, 0 }; // 两根测量线的 index
    int m_measureDrag = -1;           // 正在拖的那根（-1 = 没有）
    bool m_measureY = false;
    double m_measureYAt[2] = { 0, 0 }; // 两根 Y 测量线的数值
    int m_measureDragY = -1;
    QPixmap m_cache;
    Range m_cacheRange;
    quint64 m_cacheKey = 0;
    bool m_autoFit = true;   // 全部数据放进视口
    bool m_follow = true;    // 数据越过右边缘时整体滚动（实时跟随）
    bool m_autoY = true;
    bool m_paused = false;
    double m_pauseLimit = 0; // 暂停瞬间已有的最大 index，只画不超过它的点
    bool m_savedAutoX = true;
    double m_span = 0;       // 视口宽度（index 单位）
    double m_left = 0;       // 视口左边缘（缩放锚点）
    double m_manualYMin = 0;
    double m_manualYMax = 0;
    double m_lastYMin = 0;
    double m_lastYMax = 0;
    mutable double m_pixelSpacing = 1;
    mutable int m_stride = 1;
    mutable double m_viewMin = 0;
    mutable double m_viewMax = 0;
    mutable double m_dataMax = 0;
    mutable qint64 m_hiddenRight = 0;
    QPoint m_cursorPos;
    bool m_cursorValid = false;
    bool m_dragging = false;
    QPoint m_lastDrag;
};

// 系列没有指定颜色时使用的默认色
inline QColor seriesDisplayColor(const PlotClient::SeriesData &series)
{
    return series.color.isValid() ? series.color : QColor(0x4f, 0x9d, 0xf7);
}

#endif // PLOTCANVAS_H
