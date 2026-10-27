#include "plotwindow.h"

#include "endpointdialog.h"
#include "plotclient.h"
#include "ploticons.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QDateTime>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QScrollArea>
#include <QSet>
#include <QStatusBar>
#include <QStringList>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

const QColor kCanvasBackground(0x1c, 0x1c, 0x1c);
const QColor kPanelBackground(0x2b, 0x2b, 0x2b);
const QColor kHeaderBackground(0x24, 0x24, 0x24);
const QColor kGridColor(0x33, 0x33, 0x33);
const QColor kAxisText(0xa8, 0xa8, 0xa8);
const QColor kCursorLine(0xb8, 0xb8, 0xb8);
const QColor kReadoutBackground(0x5c, 0x3f, 0x45);
const QColor kAccent(0xe0, 0x66, 0x2a);

QString formatAxis(double value)
{
    return QString::number(value, 'g', 6);
}

double niceStep(double raw)
{
    if (!(raw > 0))
        return 1.0;
    const double exponent = std::floor(std::log10(raw));
    const double base = std::pow(10.0, exponent);
    const double fraction = raw / base;
    double nice = 10.0;
    if (fraction <= 1.0)
        nice = 1.0;
    else if (fraction <= 2.0)
        nice = 2.0;
    else if (fraction <= 5.0)
        nice = 5.0;
    return nice * base;
}

QColor seriesColor(const PlotClient::SeriesData &series)
{
    return series.color.isValid() ? series.color : QColor(0x4f, 0x9d, 0xf7);
}

// 样本按 index 递增：二分出落在 [xMin, xMax] 内的区间 [begin, end)
void visibleSpan(const SampleView &samples, double xMin, double xMax, int *begin, int *end)
{
    *begin = samples.lowerBound(xMin);
    *end = samples.upperBound(xMax);
}

QString chipStyle(const QColor &color)
{
    return QStringLiteral("QToolButton { background:%1; border:1px solid #9aa2ad;"
                          " border-radius:3px; }"
                          "QToolButton:hover { border:1px solid #4a90d9; }")
            .arg(color.name());
}

} // namespace

// ---------------------------------------------------------------- 绘图区

class PlotCanvas : public QWidget
{
public:
    explicit PlotCanvas(PlotClient *client, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_client(client)
    {
        setMinimumSize(420, 260);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
    }

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
    bool addHLine(const HLine &line)
    {
        if (m_hlines.size() >= kMaxHLines)
            return false;
        m_hlines.append(line);
        update();
        return true;
    }
    void removeHLine(int index)
    {
        if (index < 0 || index >= m_hlines.size())
            return;
        m_hlines.removeAt(index);
        update();
    }
    void setHLine(int index, const HLine &line)
    {
        if (index < 0 || index >= m_hlines.size())
            return;
        m_hlines[index] = line;
        update();
    }
    // 当前视口 Y 范围的中点，新加的水平线放在这里，一定看得见
    double viewCenterY() const
    {
        return m_cacheRange.valid ? (m_cacheRange.yMin + m_cacheRange.yMax) / 2.0 : 0.0;
    }

    // AutoFit 表示“全部数据塞进视口”；follow 表示“右边缘跟随最新数据”
    void setAutoFitX(bool on)
    {
        if (m_autoFit == on)
            return;
        if (on && m_paused) {
            fitFrozen(); // 暂停中：只按现有数据铺满一次，不跟随新数据
            return;
        }
        m_autoFit = on;
        if (on)
            m_follow = true;
        update();
    }
    bool autoFitX() const { return m_autoFit; }
    bool following() const { return m_follow; }

    void setAutoY(bool on)
    {
        if (m_autoY == on)
            return;
        m_autoY = on;
        if (!on) {
    // 视口右侧还有多少个点没显示（缩放到历史区间时会大于 0）
            m_manualYMin = m_lastYMin;
            m_manualYMax = m_lastYMax;
        }
        update();
    }
    bool autoY() const { return m_autoY; }

    void fit()
    {
        if (m_paused) {
            fitFrozen();
            m_autoY = true;
            update();
            return;
        }
        m_autoFit = true;
        m_follow = true;
        m_autoY = true;
        update();
    }

    // 暂停时的“铺满”：把当前全部数据放进视口后冻结，之后到达的新数据不再改变画面
    void fitFrozen()
    {
        double low = 0;
        double high = 0;
        if (dataIndexRange(&low, &high)) {
            m_left = low;
            m_span = qMax(1.0, high - low);
        }
        m_autoFit = false;
        m_follow = false;
        update();
    }

    void setSeriesHidden(quint32 id, bool hidden)
    {
        if (hidden)
            m_hidden.insert(id);
        else
            m_hidden.remove(id);
        update();
    }

    void setPaused(bool paused)
    {
        if (m_paused == paused)
            return;
        m_paused = paused;
        if (paused) {
            m_savedAutoX = m_autoFit;
            const Range range = computeRange();
            m_autoFit = false;
            m_follow = false; // 冻结当前视口
            if (range.valid) {
                m_span = range.xMax - range.xMin;
                m_left = range.xMin;
            }
        } else {
            m_autoFit = m_savedAutoX;
            m_follow = true;
            setMeasure(false);
        }
        update();
    }

    // 测量竖线：只在暂停时可用，两根线可以用鼠标拖动，显示两线之间的 index 差和时间差
    void setMeasure(bool on)
    {
        if (on && !m_paused)
            return;
        if (m_measure == on)
            return;
        m_measure = on;
        m_measureDrag = -1;
        if (on && m_cacheRange.valid) {
            const double span = m_cacheRange.xMax - m_cacheRange.xMin;
            m_measureAt[0] = std::round(m_cacheRange.xMin + span / 3.0);
            m_measureAt[1] = std::round(m_cacheRange.xMin + span * 2.0 / 3.0);
        }
        setCursor(Qt::ArrowCursor);
        update();
    }

    double pixelSpacing() const { return m_pixelSpacing; }
    int stride() const { return m_stride; }
    double viewMin() const { return m_viewMin; }
    double viewMax() const { return m_viewMax; }
    // 视口右边缘之外还有多少个新点
    qint64 hiddenRight() const { return m_hiddenRight; }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event)
        QPainter painter(this);

        const QRectF area = plotArea();

        // 悬停只移动游标：数据和视口没变时复用上次画好的曲线，避免每次鼠标移动都重绘所有点
        const quint64 key = cacheKey();
        const bool reuse = !m_cache.isNull() && key == m_cacheKey
                && m_cache.size() == size() * devicePixelRatioF();
        Range range = reuse ? m_cacheRange : computeRange();
        if (reuse) {
            // 画面复用时，状态栏用到的“视口右侧新点数”仍要跟着数据更新
            double xMax = m_dataMax;
            for (const PlotClient::SeriesData &series : m_client->series()) {
                if (seriesVisible(series))
                    xMax = qMax(xMax, double(series.samples.lastIndex()));
            }
            m_dataMax = xMax;
            m_hiddenRight = qMax<qint64>(0, qint64(std::ceil(xMax - range.xMax)));
        }

        if (!range.valid) {
            painter.fillRect(rect(), kCanvasBackground);
            // 没有数据时只显示连接状态，细节留在标题的 tooltip 里
            QString text;
            switch (m_client->state()) {
            case PlotClient::State::Connected:
                text = tr("Connected");
                break;
            case PlotClient::State::Idle:
                text = tr("No group selected");
                break;
            default:
                text = tr("Disconnected");
                break;
            }
            QFont hintFont = painter.font();
            if (hintFont.pointSizeF() > 0.0)
                hintFont.setPointSizeF(hintFont.pointSizeF() * 1.8);
            else
                hintFont.setPixelSize(qRound(hintFont.pixelSize() * 1.8));
            hintFont.setBold(true);
            painter.setFont(hintFont);
            painter.setPen(kAxisText);
            painter.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, text);
            return;
        }

        if (!reuse) {
            if (m_cache.isNull() || m_cache.size() != size() * devicePixelRatioF()) {
                m_cache = QPixmap(size() * devicePixelRatioF());
                m_cache.setDevicePixelRatio(devicePixelRatioF());
            }
            m_cache.fill(kCanvasBackground);
            QPainter cachePainter(&m_cache);
            drawGrid(cachePainter, area, range);
            drawSeries(cachePainter, area, range);
            cachePainter.end();
            m_cacheRange = range;
            m_cacheKey = cacheKey(); // computeRange 可能改了 m_left/m_span
        }
        painter.drawPixmap(0, 0, m_cache);
        drawHLines(painter, area, range); // 不进缓存：改名字/数值时不必重画曲线
        if (m_measure)
            drawMeasure(painter, area, range);
        drawCursor(painter, area, range);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_measureDrag >= 0 && (event->buttons() & Qt::LeftButton)) {
            if (m_cacheRange.valid) {
                const QRectF area = plotArea();
                const double ratio = (event->position().x() - area.left())
                        / qMax(1.0, area.width());
                double index = std::round(m_cacheRange.xMin
                        + ratio * (m_cacheRange.xMax - m_cacheRange.xMin));
                double low = 0;
                double high = 0;
                if (dataIndexRange(&low, &high))
                    index = qBound(low, index, high);
                m_measureAt[m_measureDrag] = index;
            }
            update();
            return;
        }
        if (m_measure && !(event->buttons() & Qt::LeftButton))
            setCursor(measureLineAt(event->position().x()) >= 0 ? Qt::SizeHorCursor
                                                                : Qt::ArrowCursor);
        if (m_dragging && (event->buttons() & Qt::LeftButton)) {
            const Range range = computeRange();
            if (range.valid) {
                const QRectF area = plotArea();
                const double perPixel = (range.xMax - range.xMin) / qMax(1.0, area.width());
                const double deltaIndex = (event->position().x() - m_lastDrag.x()) * perPixel;
                m_autoFit = false;
                m_follow = false; // 缩放后查看历史，不再被新数据推着走
                m_span = range.xMax - range.xMin;
                m_left = range.xMin - deltaIndex;
                if (!m_autoY) {
                    const double perPixelY = (range.yMax - range.yMin) / qMax(1.0, area.height());
                    const double deltaValue = (event->position().y() - m_lastDrag.y()) * perPixelY;
                    m_manualYMin = range.yMin + deltaValue;
                    m_manualYMax = range.yMax + deltaValue;
                }
            }
            m_lastDrag = event->position().toPoint();
            update();
            return;
        }
        m_cursorValid = true;
        m_cursorPos = event->position().toPoint();
        update();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_measure) {
            const int line = measureLineAt(event->position().x());
            if (line >= 0) {
                m_measureDrag = line;
                return;
            }
        }
        if (event->button() == Qt::LeftButton) {
            m_dragging = true;
            m_lastDrag = event->position().toPoint();
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_dragging = false;
            m_measureDrag = -1;
        }
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        Q_UNUSED(event)
        fit();
    }

    void wheelEvent(QWheelEvent *event) override
    {
        const Range range = computeRange();
        if (!range.valid) {
            QWidget::wheelEvent(event);
            return;
        }

        const QRectF area = plotArea();
        const double factor = event->angleDelta().y() > 0 ? 1.0 / 1.25 : 1.25;

        if (event->modifiers() & Qt::ControlModifier) {
            const double anchor = range.yMax - (event->position().y() - area.top())
                    / qMax(1.0, area.height()) * (range.yMax - range.yMin);
            m_autoY = false;
            m_manualYMin = anchor + (range.yMin - anchor) * factor;
            m_manualYMax = anchor + (range.yMax - anchor) * factor;
        } else {
            // 以鼠标所在位置为锚点缩放：鼠标下的数据点保持不动
            const double oldSpan = range.xMax - range.xMin;
            const double span = qBound(1.0, oldSpan * factor, 1.0e9);
            const double ratio = qBound(0.0, (event->position().x() - area.left())
                                        / qMax(1.0, area.width()), 1.0);
            const double anchor = range.xMin + ratio * oldSpan;
            m_autoFit = false;
            m_left = anchor - ratio * span;
            m_span = span;
            // 只有当窗口重新覆盖到最新数据时才恢复“整体滚动”，否则停在历史区间不动
            m_follow = (m_left + span >= m_dataMax);
        }
        update();
    }

    void leaveEvent(QEvent *event) override
    {
        Q_UNUSED(event)
        m_cursorValid = false;
        update();
    }

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
    quint64 cacheKey() const
    {
        quint64 hash = 1469598103934665603ULL;
        auto mix = [&hash](quint64 value) { hash = (hash ^ value) * 1099511628211ULL; };
        auto mixDouble = [&mix](double value) {
            quint64 bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            mix(bits);
        };
        mix(quint64(width()));
        mix(quint64(height()));
        mix(m_autoFit);
        mix(m_follow);
        mix(m_autoY);
        mixDouble(m_left);
        mixDouble(m_span);
        mixDouble(m_manualYMin);
        mixDouble(m_manualYMax);
        for (const PlotClient::SeriesData &series : m_client->series()) {
            mix(series.id);
            mix(m_hidden.contains(series.id));
            if (!m_autoFit && !m_follow && m_span > 0 && !series.samples.isEmpty()) {
                // 停在历史区间时，视口外的新数据不影响画面，不必重绘
                int begin = 0;
                int end = 0;
                visibleSpan(series.samples, m_left, m_left + m_span, &begin, &end);
                mix(quint64(end - begin));
                if (end > begin) {
                    mix(series.samples.indexAt(begin));
                    mix(series.samples.indexAt(end - 1));
                    mixDouble(series.samples.valueAt(end - 1));
                }
                mix(series.samples.lastIndex() > m_left + m_span);
            } else {
                mix(quint64(series.samples.size()));
                if (!series.samples.isEmpty()) {
                    mix(series.samples.firstIndex());
                    mix(series.samples.lastIndex());
                    mixDouble(series.samples.lastValue());
                }
            }
            mix(seriesColor(series).rgba());
        }
        return hash;
    }

    QRectF plotArea() const
    {
        return QRectF(rect()).adjusted(56, 12, -14, -34);
    }

    bool seriesVisible(const PlotClient::SeriesData &series) const
    {
        return !m_hidden.contains(series.id) && !series.samples.isEmpty();
    }

    Range computeRange()
    {
        Range range;

        bool hasX = false;
        double xMin = 0;
        double xMax = 0;
        for (const PlotClient::SeriesData &series : m_client->series()) {
            if (!seriesVisible(series))
                continue;
            const double first = series.samples.firstIndex();
            const double last = series.samples.lastIndex();
            if (!hasX) {
                xMin = first;
                xMax = last;
                hasX = true;
            } else {
                xMin = qMin(xMin, first);
                xMax = qMax(xMax, last);
            }
        }
        if (!hasX)
            return range;

        range.valid = true;
        if (m_autoFit) {
            range.xMin = xMin;
            range.xMax = xMax;
            if (range.xMax <= range.xMin)
                range.xMax = range.xMin + 1.0;
            m_span = range.xMax - range.xMin;
            m_left = range.xMin;
        } else {
            const double span = m_span > 0 ? m_span : qMax(1.0, xMax - xMin);
            // 左边缘固定；只有“数据越过右边缘”且仍在跟随时才整体右移
            if (m_follow && xMax > m_left + span)
                m_left = xMax - span;
            range.xMin = m_left;
            range.xMax = m_left + span;
        }

        // Y 范围：默认按视口内的数据自适应
        double yMin = 0;
        double yMax = 0;
        bool hasY = false;
        int maxVisible = 0;
        for (const PlotClient::SeriesData &series : m_client->series()) {
            if (!seriesVisible(series))
                continue;
            int begin = 0;
            int end = 0;
            visibleSpan(series.samples, range.xMin, range.xMax, &begin, &end);
            maxVisible = qMax(maxVisible, end - begin);
            for (int i = begin; i < end; ++i) {
                const double value = series.samples.valueAt(i);
                if (!hasY) {
                    yMin = value;
                    yMax = value;
                    hasY = true;
                } else {
                    yMin = qMin(yMin, value);
                    yMax = qMax(yMax, value);
                }
            }
        }
        if (!hasY) {
            yMin = -1;
            yMax = 1;
        }
        const double padding = (yMax - yMin) * 0.08;
        m_lastYMin = yMin - (padding > 0 ? padding : 0.5);
        m_lastYMax = yMax + (padding > 0 ? padding : 0.5);
        if (m_autoY) {
            range.yMin = m_lastYMin;
            range.yMax = m_lastYMax;
        } else {
            range.yMin = m_manualYMin;
            range.yMax = m_manualYMax;
        }
        if (range.yMax <= range.yMin)
            range.yMax = range.yMin + 1.0;

        const int width = qMax(1, int(plotArea().width()));
        m_pixelSpacing = double(width) / qMax(1.0, range.xMax - range.xMin);
        // 仅用于状态栏显示：视口内每个像素列对应多少个点
        m_stride = qMax(1, maxVisible / width);
        m_viewMin = range.xMin;
        m_viewMax = range.xMax;
        m_dataMax = xMax;
        m_hiddenRight = qMax<qint64>(0, qint64(std::ceil(xMax - range.xMax)));
        return range;
    }

    double mapX(double index, const QRectF &area, const Range &range) const
    {
        return area.left() + (index - range.xMin) / (range.xMax - range.xMin) * area.width();
    }

    double mapY(double value, const QRectF &area, const Range &range) const
    {
        return area.bottom() - (value - range.yMin) / (range.yMax - range.yMin) * area.height();
    }

    void drawGrid(QPainter &painter, const QRectF &area, const Range &range) const
    {
        painter.setRenderHint(QPainter::Antialiasing, false);
        QFont small = painter.font();
        small.setPointSizeF(qMax(7.5, small.pointSizeF() - 1.0));
        painter.setFont(small);

        // 横向：数值刻度
        const double yStep = niceStep((range.yMax - range.yMin) / 6.0);
        const double yStart = std::ceil(range.yMin / yStep) * yStep;
        for (double value = yStart; value <= range.yMax + yStep * 0.001; value += yStep) {
            const double y = mapY(value, area, range);
            painter.setPen(QPen(kGridColor, 1));
            painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
            painter.setPen(kAxisText);
            painter.drawText(QRectF(0, y - 8, area.left() - 6, 16),
                             Qt::AlignRight | Qt::AlignVCenter, formatAxis(value));
        }

        // 纵向：序号刻度
        const double xSpan = range.xMax - range.xMin;
        const double xStep = niceStep(xSpan / 8.0);
        const double xStart = std::ceil(range.xMin / xStep) * xStep;
        for (double value = xStart; value <= range.xMax + xStep * 0.001; value += xStep) {
            const double x = mapX(value, area, range);
            painter.setPen(QPen(kGridColor, 1));
            painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
            painter.setPen(kAxisText);
            painter.drawText(QRectF(x - 32, area.bottom() + 4, 64, 16),
                             Qt::AlignHCenter | Qt::AlignTop,
                             QString::number(qRound64(value)));
        }

        painter.setPen(kAxisText);
        painter.drawText(QRectF(area.left(), area.bottom() + 18, 120, 16), Qt::AlignLeft,
                         tr("Index"));
    }

    void drawSeries(QPainter &painter, const QRectF &area, const Range &range) const
    {
        painter.save();
        painter.setClipRect(area);

        const int columns = qMax(1, int(area.width()));
        QPolygonF polyline;

        for (const PlotClient::SeriesData &series : m_client->series()) {
            if (!seriesVisible(series))
                continue;

            // 只处理视口内的点，两侧各多取一个，让线段能连到视口外
            int begin = 0;
            int end = 0;
            visibleSpan(series.samples, range.xMin, range.xMax, &begin, &end);
            begin = qMax(0, begin - 1);
            end = qMin(series.samples.size(), end + 1);
            const int count = end - begin;
            if (count <= 0)
                continue;

            polyline.clear();
            const bool decimate = count > columns * 2;
            if (!decimate) {
                polyline.reserve(count);
                for (int i = begin; i < end; ++i) {
                    polyline.append(QPointF(mapX(series.samples.indexAt(i), area, range),
                                            mapY(series.samples.valueAt(i), area, range)));
                }
            } else {
                // 每个像素列只保留 首/最小/最大/末 四个点（按 index 排序），尖峰不会丢
                polyline.reserve(columns * 4 + 8);
                int i = begin;
                while (i < end) {
                    const int column = int(std::floor(mapX(series.samples.indexAt(i), area, range)));
                    int first = i;
                    int minAt = i;
                    int maxAt = i;
                    int last = i;
                    for (++i; i < end; ++i) {
                        if (int(std::floor(mapX(series.samples.indexAt(i), area, range))) != column)
                            break;
                        const float value = series.samples.valueAt(i);
                        if (value < series.samples.valueAt(minAt))
                            minAt = i;
                        if (value > series.samples.valueAt(maxAt))
                            maxAt = i;
                        last = i;
                    }
                    int picks[4] = { first, minAt, maxAt, last };
                    std::sort(picks, picks + 4);
                    int previous = -1;
                    for (int pick : picks) {
                        if (pick == previous)
                            continue;
                        previous = pick;
                        polyline.append(QPointF(mapX(series.samples.indexAt(pick), area, range),
                                                mapY(series.samples.valueAt(pick), area, range)));
                    }
                }
            }

            // 点很多时抗锯齿代价高且看不出差别，只在稀疏时开启
            painter.setRenderHint(QPainter::Antialiasing, !decimate);
            // 线宽必须保持 1.0：大于 1 会走慢得多的描边路径（实测相差上百倍）
            painter.setPen(QPen(seriesColor(series), 1.0));
            painter.drawPolyline(polyline);
        }

        painter.restore();
    }

    // 可见曲线里最小/最大的 index（测量线只能拖在有数据的范围内）
    bool dataIndexRange(double *low, double *high) const
    {
        bool has = false;
        for (const PlotClient::SeriesData &series : m_client->series()) {
            if (!seriesVisible(series))
                continue;
            const double first = series.samples.firstIndex();
            const double last = series.samples.lastIndex();
            *low = has ? qMin(*low, first) : first;
            *high = has ? qMax(*high, last) : last;
            has = true;
        }
        return has;
    }

    // 鼠标 x 附近（6 像素内）的测量线，没有返回 -1；两根重合时优先后一根，方便拖开
    int measureLineAt(double x) const
    {
        if (!m_cacheRange.valid)
            return -1;
        const QRectF area = plotArea();
        int best = -1;
        double bestDistance = 6.0;
        for (int k = 1; k >= 0; --k) {
            const double distance = qAbs(mapX(m_measureAt[k], area, m_cacheRange) - x);
            if (distance <= bestDistance) {
                best = k;
                bestDistance = distance;
            }
        }
        return best;
    }

    // 某个 index 对应的采集时刻（所有曲线共用时间轴，取任意一条可见曲线即可）
    bool timestampOf(double index, qint64 *timestampMs) const
    {
        for (const PlotClient::SeriesData &series : m_client->series()) {
            if (!seriesVisible(series))
                continue;
            const int at = series.samples.nearest(quint32(qMax(0.0, index)));
            if (at < 0)
                continue;
            *timestampMs = series.samples.timestampAt(at);
            return true;
        }
        return false;
    }

    void drawMeasure(QPainter &painter, const QRectF &area, const Range &range) const
    {
        static const QColor kColors[2] = { QColor(0xff, 0xb7, 0x4d), QColor(0x4d, 0xd0, 0xe1) };
        static const char *const kNames[2] = { "A", "B" };

        painter.save();
        painter.setClipRect(area.adjusted(-1, -1, 1, 1));
        QFont font = painter.font();
        font.setPointSizeF(qMax(7.5, font.pointSizeF() - 1.0));
        painter.setFont(font);
        const int lineHeight = painter.fontMetrics().height() + 2;

        for (int k = 0; k < 2; ++k) {
            const double x = mapX(m_measureAt[k], area, range);
            if (x < area.left() || x > area.right())
                continue;
            painter.setPen(QPen(kColors[k], 1.5));
            painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));

            // 顶部小标签：名字 + index，贴边时翻到线的另一侧
            const QString label = QStringLiteral("%1 %2").arg(QLatin1String(kNames[k]))
                                          .arg(qRound64(m_measureAt[k]));
            const int labelWidth = painter.fontMetrics().horizontalAdvance(label) + 10;
            QRectF tag(x + 2, area.top() + 2 + k * (lineHeight + 2), labelWidth, lineHeight);
            if (tag.right() > area.right())
                tag.moveRight(x - 2);
            painter.setPen(Qt::NoPen);
            painter.setBrush(kColors[k]);
            painter.drawRect(tag);
            painter.setPen(QColor(0x20, 0x20, 0x20));
            painter.drawText(tag, Qt::AlignCenter, label);
        }

        // 差值读数框：放在绘图区底部中间
        const qint64 deltaIndex = qAbs(qRound64(m_measureAt[1]) - qRound64(m_measureAt[0]));
        QString timeText = QStringLiteral("--");
        qint64 timeA = 0;
        qint64 timeB = 0;
        if (timestampOf(m_measureAt[0], &timeA) && timestampOf(m_measureAt[1], &timeB)) {
            const qint64 deltaMs = qAbs(timeB - timeA);
            timeText = deltaMs >= 1000 ? QStringLiteral("%1 s").arg(deltaMs / 1000.0, 0, 'f', 3)
                                       : QStringLiteral("%1 ms").arg(deltaMs);
        }
        const QString text = tr("ΔIndex: %1    ΔTime: %2").arg(deltaIndex).arg(timeText);
        const int boxWidth = painter.fontMetrics().horizontalAdvance(text) + 24;
        const QRectF box(area.center().x() - boxWidth / 2.0, area.bottom() - lineHeight - 14,
                         boxWidth, lineHeight + 8);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(kReadoutBackground.red(), kReadoutBackground.green(),
                                kReadoutBackground.blue(), 235));
        painter.drawRect(box);
        painter.setPen(QColor(0xf0, 0xf0, 0xf0));
        painter.drawText(box, Qt::AlignCenter, text);
        painter.restore();
    }

    void drawHLines(QPainter &painter, const QRectF &area, const Range &range) const
    {
        painter.save();
        painter.setClipRect(area);
        QFont font = painter.font();
        font.setPointSizeF(qMax(7.5, font.pointSizeF() - 1.0));
        painter.setFont(font);
        const int lineHeight = painter.fontMetrics().height();

        for (const HLine &line : m_hlines) {
            if (!line.visible)
                continue;
            const double y = mapY(line.value, area, range);
            if (y < area.top() || y > area.bottom())
                continue;
            const QColor color = line.color.isValid() ? line.color : QColor(0xff, 0xd5, 0x4f);
            painter.setPen(QPen(color, 1.0, Qt::DashLine));
            painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));

            const QString value = formatAxis(line.value);
            const QString label = line.name.isEmpty() ? value
                                                      : QStringLiteral("%1  %2").arg(line.name, value);
            painter.setPen(color);
            // 标签放在线上方、靠右；贴近上边缘时放到线下方
            const double top = y - lineHeight - 1 >= area.top() ? y - lineHeight - 1 : y + 2;
            painter.drawText(QRectF(area.left(), top, area.width() - 6, lineHeight),
                             Qt::AlignRight | Qt::AlignVCenter, label);
        }
        painter.restore();
    }

    // 游标处的采样点：按序号取最近的样本
    bool sampleAt(quint32 index, SampleView::Point *point) const
    {
        const PlotClient::SeriesData *best = nullptr;
        qint64 bestDistance = 0;
        int bestAt = -1;
        for (const PlotClient::SeriesData &series : m_client->series()) {
            if (!seriesVisible(series))
                continue;
            const int at = series.samples.nearest(index);
            if (at < 0)
                continue;
            const qint64 distance = qAbs(qint64(series.samples.indexAt(at)) - qint64(index));
            if (!best || distance < bestDistance) {
                best = &series;
                bestDistance = distance;
                bestAt = at;
            }
        }
        if (!best)
            return false;
        *point = best->samples.at(bestAt);
        return true;
    }

    void drawCursor(QPainter &painter, const QRectF &area, const Range &range) const
    {
        if (!m_cursorValid)
            return;
        const double px = qBound(area.left(), qreal(m_cursorPos.x()), area.right());
        const double index = range.xMin
                + (px - area.left()) / qMax(1.0, area.width()) * (range.xMax - range.xMin);
        const quint32 rounded = quint32(qMax(0.0, std::round(index)));

        SampleView::Point anchor;
        if (!sampleAt(rounded, &anchor))
            return;

        painter.setPen(QPen(kCursorLine, 1, Qt::DashLine));
        painter.drawLine(QPointF(px, area.top()), QPointF(px, area.bottom()));

        // 读数框
        int lineCount = 1; // "Value:"
        QVector<QPair<QColor, QPair<QString, QString>>> lines;
        for (const PlotClient::SeriesData &series : m_client->series()) {
            if (!seriesVisible(series))
                continue;
            const int at = series.samples.nearest(rounded);
            lines.append(qMakePair(seriesColor(series),
                                   qMakePair(series.title.isEmpty()
                                                     ? tr("Series %1").arg(series.id)
                                                     : series.title,
                                             formatAxis(series.samples.valueAt(at)))));
        }
        lineCount += lines.size();
        lineCount += 2; // 空行 + "Index:"

        QFont font = painter.font();
        font.setPointSizeF(qMax(7.5, font.pointSizeF() - 1.0));
        painter.setFont(font);
        const int lineHeight = fontMetrics().height() + 2;
        const int boxWidth = 208;
        const int boxHeight = lineCount * lineHeight + 10;
        QRectF box(px + 12, area.top() + area.height() * 0.42, boxWidth, boxHeight);
        if (box.right() > area.right())
            box.moveLeft(px - 12 - boxWidth);
        if (box.bottom() > area.bottom())
            box.moveTop(area.bottom() - boxHeight);

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(kReadoutBackground.red(), kReadoutBackground.green(),
                                kReadoutBackground.blue(), 235));
        painter.drawRect(box);
        painter.setBrush(Qt::NoBrush);

        painter.setPen(QColor(0xc8, 0xc8, 0xc8));
        qreal y = box.top() + 6;
        painter.drawText(QRectF(box.left() + 8, y, boxWidth - 16, lineHeight), Qt::AlignLeft,
                         tr("Value:"));
        y += lineHeight;
        for (const auto &line : std::as_const(lines)) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(line.first);
            painter.drawRect(QRectF(box.left() + 14, y + 4, 12, 6));
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QColor(0xd8, 0xd8, 0xd8));
            painter.drawText(QRectF(box.left() + 32, y, 100, lineHeight), Qt::AlignLeft,
                             line.second.first);
            painter.drawText(QRectF(box.left() + boxWidth - 84, y, 76, lineHeight),
                             Qt::AlignRight, line.second.second);
            y += lineHeight;
        }
        y += lineHeight / 2;
        painter.setPen(QColor(0xc8, 0xc8, 0xc8));
        painter.drawText(QRectF(box.left() + 8, y, boxWidth - 16, lineHeight), Qt::AlignLeft,
                         tr("Index:"));
        y += lineHeight;
        painter.setPen(QColor(0xd8, 0xd8, 0xd8));
        const QString stamp = QDateTime::fromMSecsSinceEpoch(anchor.timestampMs)
                                      .toString(QStringLiteral("yy-MM-dd HH:mm:ss.zzz"));
        painter.drawText(QRectF(box.left() + 8, y, boxWidth - 16, lineHeight), Qt::AlignLeft,
                         QStringLiteral("%1(%2)").arg(stamp).arg(rounded));
    }

    PlotClient *m_client = nullptr;
    QSet<quint32> m_hidden;
    QVector<HLine> m_hlines;
    bool m_measure = false;
    double m_measureAt[2] = { 0, 0 }; // 两根测量线的 index
    int m_measureDrag = -1;           // 正在拖的那根（-1 = 没有）
    QPixmap m_cache;
    Range m_cacheRange;
    quint64 m_cacheKey = 0;
    bool m_autoFit = true;   // 全部数据放进视口
    bool m_follow = true;    // 数据越过右边缘时整体滚动（实时跟随）
    bool m_autoY = true;
    bool m_paused = false;
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

// ---------------------------------------------------------------- 窗口

PlotWindow::PlotWindow(PlotClient *client, QWidget *parent)
    : QMainWindow(parent)
    , m_client(client)
{
    setWindowTitle(tr("rts-plot"));
    resize(1180, 640);

    setStyleSheet(QStringLiteral(
            "QMainWindow, QWidget#plotRoot { background: #f0f0f0; }"
            "QWidget#seriesPanel { background: #e4e4e4; }"
            "QWidget#plotHeader { background: #f7f7f7; border-bottom: 1px solid #d5d5d5; }"
            "QLabel { color: #202020; }"
            "QCheckBox { color: #202020; spacing: 5px; }"
            "QComboBox { background: #ffffff; color: #202020; border: 1px solid #b8b8b8;"
            "  padding: 1px 6px; }"
            "QComboBox QAbstractItemView { background: #ffffff; color: #202020; }"
            "QToolButton { background: transparent; border: none; color: #202020; padding: 2px; }"
            "QToolButton:hover { background: #dfe4ea; border-radius: 4px; }"
            "QToolButton#plotAction { background: #ffffff; border: 1px solid #ccd2da;"
            "  border-radius: 4px; padding: 2px 8px; color: #2b3038; }"
            "QToolButton#plotAction:hover { background: #f1f5fa; border-color: #a9b2be; }"
            "QToolButton#plotAction:pressed { background: #e2e8f0; border-color: #97a1ad; }"
            "QToolButton#plotPause { background: #e0662a; border: 1px solid #c8551e;"
            "  border-radius: 4px; padding: 2px 10px; color: #ffffff; font-weight: 600; }"
            "QToolButton#plotPause:hover { background: #ea7639; border-color: #d0601f; }"
            "QToolButton#plotPause:pressed { background: #cb5a22; }"
            "QToolButton#plotPause:checked { background: #ffffff; color: #c05420;"
            "  border: 1px solid #e0662a; }"
            "QStatusBar { background: #f7f7f7; color: #4a4a4a; }"
            "QStatusBar QLabel { color: #4a4a4a; }"
            "QScrollArea { background: transparent; border: none; }"
            "QScrollBar:vertical { background: #e4e4e4; width: 10px; }"
            "QScrollBar::handle:vertical { background: #b8b8b8; border-radius: 5px; }"));

    auto *root = new QWidget(this);
    root->setObjectName(QStringLiteral("plotRoot"));
    auto *rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // 顶部标题行
    auto *header = new QWidget(root);
    header->setObjectName(QStringLiteral("plotHeader"));
    header->setFixedHeight(34);
    m_titleLabel = new QLabel(header);
    QFont titleFont = m_titleLabel->font();
    titleFont.setBold(true);
    m_titleLabel->setFont(titleFont);
    m_titleLabel->setTextFormat(Qt::PlainText);
    m_connectionLabel = new QLabel(header);
    m_connectionLabel->setFont(titleFont);

    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(10, 0, 8, 0);
    headerLayout->setSpacing(8);
    headerLayout->addWidget(m_titleLabel, 1);
    headerLayout->addWidget(m_connectionLabel); // 连接状态靠最右

    // 左侧面板
    auto *panel = new QWidget(root);
    panel->setObjectName(QStringLiteral("seriesPanel"));
    panel->setFixedWidth(232);

    m_pauseButton = new QToolButton(panel);
    m_pauseButton->setObjectName(QStringLiteral("plotPause"));
    m_pauseButton->setCheckable(true);
    m_pauseButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_pauseButton->setIconSize(QSize(14, 14));
    m_pauseButton->setIcon(PlotIcons::pause(Qt::white));
    m_pauseButton->setText(tr("Pause"));
    m_pauseButton->setToolTip(tr("Pause / resume the view"));
    m_pauseButton->setCursor(Qt::PointingHandCursor);
    m_pauseButton->setFixedHeight(32);
    m_pauseButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto *fitButton = new QToolButton(panel);
    fitButton->setObjectName(QStringLiteral("plotAction"));
    fitButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    fitButton->setIconSize(QSize(14, 14));
    fitButton->setIcon(PlotIcons::fit(QColor(0x3a, 0x40, 0x48)));
    fitButton->setText(tr("Fit"));
    fitButton->setToolTip(tr("Fit all data to the view and follow new samples"));
    fitButton->setCursor(Qt::PointingHandCursor);
    fitButton->setFixedHeight(26);
    fitButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto *clearButton = new QToolButton(panel);
    clearButton->setObjectName(QStringLiteral("plotAction"));
    clearButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    clearButton->setIconSize(QSize(14, 14));
    clearButton->setIcon(PlotIcons::trash(QColor(0x3a, 0x40, 0x48)));
    clearButton->setText(tr("Clear"));
    clearButton->setToolTip(tr("Clear buffered samples of every series"));
    clearButton->setCursor(Qt::PointingHandCursor);
    clearButton->setFixedHeight(26);
    clearButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto *toolsRow = new QVBoxLayout;
    toolsRow->setContentsMargins(0, 0, 0, 2);
    toolsRow->setSpacing(5);
    toolsRow->addWidget(m_pauseButton);

    auto *toolsSubRow = new QHBoxLayout;
    toolsSubRow->setContentsMargins(0, 0, 0, 0);
    toolsSubRow->setSpacing(5);
    toolsSubRow->addWidget(fitButton);
    toolsSubRow->addWidget(clearButton);
    toolsRow->addLayout(toolsSubRow);

    m_autoXCheck = new QCheckBox(tr("X"), panel);
    m_autoXCheck->setChecked(true);
    m_autoYCheck = new QCheckBox(tr("Y"), panel);
    m_autoYCheck->setChecked(true);
    auto *autoRow = new QHBoxLayout;
    autoRow->setContentsMargins(0, 0, 0, 0);
    autoRow->setSpacing(6);
    autoRow->addWidget(new QLabel(tr("Auto:"), panel));
    autoRow->addWidget(m_autoXCheck);
    autoRow->addWidget(m_autoYCheck);
    autoRow->addStretch();

    m_cacheCombo = new QComboBox(panel);
    // 0 = 不设软上限，实际受 PlotClient 的硬上限约束，标签如实写出
    m_cacheCombo->addItem(tr("Max (%1)").arg(QLocale().toString(PlotClient::kMaxSamplesPerSeries)),
                          0);
    m_cacheCombo->addItem(QStringLiteral("1 000"), 1000);
    m_cacheCombo->addItem(QStringLiteral("10 000"), 10000);
    m_cacheCombo->addItem(QStringLiteral("100 000"), 100000);
    m_measureCheck = new QCheckBox(tr("Measure"), panel);
    m_measureCheck->setEnabled(false);
    m_measureCheck->setToolTip(tr("Pause first, then drag the two vertical lines to measure "
                                  "the index / time difference"));
    auto *cacheRow = new QHBoxLayout;
    cacheRow->setContentsMargins(0, 0, 0, 0);
    cacheRow->setSpacing(6);
    cacheRow->addWidget(new QLabel(tr("Cache:"), panel));
    cacheRow->addWidget(m_cacheCombo, 1);

    m_seriesHost = new QWidget;
    m_seriesLayout = new QVBoxLayout(m_seriesHost);
    m_seriesLayout->setContentsMargins(0, 0, 0, 0);
    m_seriesLayout->setSpacing(1);
    m_seriesLayout->setAlignment(Qt::AlignTop);

    auto *scroll = new QScrollArea(panel);
    scroll->setWidget(m_seriesHost);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(8, 8, 8, 6);
    panelLayout->setSpacing(8);
    panelLayout->addLayout(toolsRow);
    panelLayout->addLayout(autoRow);
    panelLayout->addLayout(cacheRow);
    panelLayout->addWidget(m_measureCheck);
    scroll->setMinimumHeight(80);
    panelLayout->addWidget(scroll, 1);

    // 水平参考线（最多 5 条）：左下角
    auto *hlineTitle = new QLabel(tr("Horizontal lines"), panel);
    m_hlineAddButton = new QToolButton(panel);
    m_hlineAddButton->setObjectName(QStringLiteral("plotAction"));
    m_hlineAddButton->setText(QStringLiteral("+"));
    m_hlineAddButton->setFixedSize(22, 22);
    m_hlineAddButton->setCursor(Qt::PointingHandCursor);
    auto *hlineHeader = new QHBoxLayout;
    hlineHeader->setContentsMargins(0, 0, 0, 0);
    hlineHeader->addWidget(hlineTitle, 1);
    hlineHeader->addWidget(m_hlineAddButton);
    m_hlineLayout = new QVBoxLayout;
    m_hlineLayout->setContentsMargins(0, 0, 0, 0);
    m_hlineLayout->setSpacing(2);
    panelLayout->addLayout(hlineHeader);
    panelLayout->addLayout(m_hlineLayout);
    connect(m_hlineAddButton, &QToolButton::clicked, this, &PlotWindow::addHLine);

    // 绘图区
    m_canvas = new PlotCanvas(m_client, root);
    rebuildHLineRows(); // 需要画布已创建

    auto *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(panel);
    body->addWidget(m_canvas, 1);

    rootLayout->addWidget(header);
    rootLayout->addLayout(body, 1);
    setCentralWidget(root);

    m_statusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusLabel);

    connect(m_autoXCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_canvas->setAutoFitX(on);
    });
    connect(m_autoYCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_canvas->setAutoY(on);
    });
    connect(m_cacheCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        m_client->setSampleLimit(m_cacheCombo->currentData().toInt());
    });
    connect(m_pauseButton, &QToolButton::toggled, this, &PlotWindow::setPaused);
    connect(m_measureCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_canvas->setMeasure(on);
    });
    connect(fitButton, &QToolButton::clicked, this, &PlotWindow::fitView);
    connect(clearButton, &QToolButton::clicked, this, [this] {
        m_client->clearAllSamples();
    });

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(50);
    connect(m_refreshTimer, &QTimer::timeout, this, [this] {
        refreshValues();
        // 缩放/平移会改掉视口模式，把复选框同步回来
        if (m_canvasDirty) {
            m_canvasDirty = false;
            m_canvas->update();
        }
        // 缩放/拖动后也要跟着新数据刷新
        {
            const QSignalBlocker blockerX(m_autoXCheck);
            m_autoXCheck->setChecked(m_canvas->autoFitX());
            const QSignalBlocker blockerY(m_autoYCheck);
            m_autoYCheck->setChecked(m_canvas->autoY());
        }
    });
    m_refreshTimer->start();

    connect(m_client, &PlotClient::stateChanged, this, [this] {
        refreshStatus();
        applySuggestedPosition();
    });
    connect(m_client, &PlotClient::metaChanged, this, &PlotWindow::rebuildSeriesRows);
    connect(m_client, &PlotClient::samplesAppended, this, [this] { m_canvasDirty = true; });

    refreshTitle();
    refreshStatus();
    rebuildSeriesRows();
}

void PlotWindow::chooseGroup()
{
    EndpointDialog dialog(m_client->endpointsFilePath(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    m_positionApplied = false;
    m_client->setAutoReconnect(true);
    m_client->connectToGroup(dialog.selectedGroupKey());
}

void PlotWindow::refreshTitle()
{
    const QString title = m_client->title().isEmpty() ? m_client->groupKey() : m_client->title();
    const QString name = title.isEmpty() ? tr("(no group)") : title;
    const bool connected = m_client->state() == PlotClient::State::Connected;

    // 名字在左；连接状态在最右，用颜色区分
    m_titleLabel->setText(name);
    m_connectionLabel->setText(connected ? tr("Connected") : tr("Disconnected"));
    m_connectionLabel->setStyleSheet(QStringLiteral("color: %1;")
                                             .arg(connected ? QStringLiteral("#1e7b34")
                                                            : QStringLiteral("#b3261e")));
    m_connectionLabel->setToolTip(statusDetail());
    m_titleLabel->setToolTip(statusDetail());
}

QString PlotWindow::statusDetail() const
{
    QStringList parts;
    const QString state = m_client->placeholder().isEmpty() ? m_client->stateText()
                                                            : m_client->placeholder();
    if (!state.isEmpty())
        parts.append(state);
    if (!m_client->pipe().isEmpty())
        parts.append(m_client->pipe());
    return parts.join(QLatin1Char('\n'));
}

void PlotWindow::addHLine()
{
    static const QColor kColors[PlotCanvas::kMaxHLines] = {
        QColor(0xff, 0xd5, 0x4f), QColor(0x4d, 0xd0, 0xe1), QColor(0xf0, 0x62, 0x92),
        QColor(0xae, 0xd5, 0x81), QColor(0xff, 0xff, 0xff),
    };
    PlotCanvas::HLine line;
    // 取一个没被占用的序号和颜色
    for (int n = 1; n <= PlotCanvas::kMaxHLines; ++n) {
        const QString name = tr("Line %1").arg(n);
        const bool used = std::any_of(m_canvas->hlines().cbegin(), m_canvas->hlines().cend(),
                                      [&name](const PlotCanvas::HLine &other) {
                                          return other.name == name;
                                      });
        if (!used) {
            line.name = name;
            line.color = kColors[n - 1];
            break;
        }
    }
    line.value = m_canvas->viewCenterY();
    if (m_canvas->addHLine(line))
        rebuildHLineRows();
}

void PlotWindow::rebuildHLineRows()
{
    while (QLayoutItem *item = m_hlineLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }

    const QVector<PlotCanvas::HLine> lines = m_canvas->hlines();
    for (int i = 0; i < lines.size(); ++i) {
        const PlotCanvas::HLine &line = lines.at(i);
        auto *row = new QWidget;

        auto *visible = new QCheckBox(row);
        visible->setChecked(line.visible);
        visible->setToolTip(tr("Show / hide this line"));

        auto *name = new QLineEdit(line.name, row);
        name->setMinimumWidth(44);
        name->setPlaceholderText(tr("Name"));
        name->setToolTip(tr("Line name"));

        auto *value = new QDoubleSpinBox(row);
        value->setRange(-1.0e15, 1.0e15);
        value->setDecimals(4);
        value->setButtonSymbols(QAbstractSpinBox::NoButtons);
        value->setValue(line.value);
        value->setFixedWidth(84);
        value->setToolTip(tr("Line value (Y)"));

        auto *chip = new QToolButton(row);
        chip->setFixedSize(16, 16);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setToolTip(tr("Click to change this line's colour"));
        chip->setStyleSheet(chipStyle(line.color));

        auto *remove = new QToolButton(row);
        remove->setText(QStringLiteral("\u00d7"));
        remove->setToolTip(tr("Delete this line"));
        remove->setAutoRaise(true);
        remove->setFixedSize(18, 18);
        remove->setCursor(Qt::PointingHandCursor);

        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(2, 1, 2, 1);
        layout->setSpacing(4);
        layout->addWidget(visible);
        layout->addWidget(name, 1);
        layout->addWidget(value);
        layout->addWidget(chip);
        layout->addWidget(remove);
        m_hlineLayout->addWidget(row);

        auto edit = [this, i](auto &&change) {
            if (i >= m_canvas->hlines().size())
                return;
            PlotCanvas::HLine updated = m_canvas->hlines().at(i);
            change(updated);
            m_canvas->setHLine(i, updated);
        };
        connect(visible, &QCheckBox::toggled, this, [edit](bool on) {
            edit([on](PlotCanvas::HLine &l) { l.visible = on; });
        });
        connect(name, &QLineEdit::textChanged, this, [edit](const QString &text) {
            edit([&text](PlotCanvas::HLine &l) { l.name = text; });
        });
        connect(value, &QDoubleSpinBox::valueChanged, this, [edit](double v) {
            edit([v](PlotCanvas::HLine &l) { l.value = v; });
        });
        connect(chip, &QToolButton::clicked, this, [this, i, chip] {
            if (i >= m_canvas->hlines().size())
                return;
            PlotCanvas::HLine updated = m_canvas->hlines().at(i);
            const QColor picked = QColorDialog::getColor(updated.color, this, tr("Line colour"));
            if (!picked.isValid())
                return;
            updated.color = picked;
            m_canvas->setHLine(i, updated);
            chip->setStyleSheet(chipStyle(picked));
        });
        connect(remove, &QToolButton::clicked, this, [this, i] {
            m_canvas->removeHLine(i);
            // 在按钮自己的槽函数里不能立刻销毁它，等事件循环回来再重建
            QTimer::singleShot(0, this, &PlotWindow::rebuildHLineRows);
        });
    }
    m_hlineAddButton->setEnabled(lines.size() < PlotCanvas::kMaxHLines);
    m_hlineAddButton->setToolTip(lines.size() < PlotCanvas::kMaxHLines
                                         ? tr("Add a horizontal line")
                                         : tr("At most %1 lines").arg(PlotCanvas::kMaxHLines));
}

void PlotWindow::rebuildSeriesRows()
{
    // 清空整个列表（包括之前插入的占位提示）
    while (QLayoutItem *item = m_seriesLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }
    m_rows.clear();

    const QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    for (const PlotClient::SeriesData &data : m_client->series()) {
        SeriesRow row;
        row.widget = new QWidget(m_seriesHost);

        row.visible = new QCheckBox(row.widget);
        row.visible->setChecked(true);
        row.visible->setToolTip(tr("Show / hide this curve"));

        row.name = new QLabel(data.title.isEmpty() ? tr("Series %1").arg(data.id) : data.title,
                              row.widget);
        row.name->setMinimumWidth(56);
        row.name->setStyleSheet(QStringLiteral("color: #202020; background: transparent;"));

        row.value = new QLabel(QStringLiteral("--"), row.widget);
        row.value->setFont(fixedFont);
        row.value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row.value->setMinimumWidth(58);
        row.value->setStyleSheet(QStringLiteral("color: #0b6b3a; background: transparent;"));

        row.chip = new QToolButton(row.widget);
        row.chip->setFixedSize(16, 16);
        row.chip->setCursor(Qt::PointingHandCursor);
        row.chip->setToolTip(tr("Click to change this curve's colour"));
        row.chip->setStyleSheet(chipStyle(seriesColor(data)));

        auto *layout = new QHBoxLayout(row.widget);
        layout->setContentsMargins(2, 3, 2, 3);
        layout->setSpacing(6);
        layout->addWidget(row.visible);
        layout->addWidget(row.name, 1);
        layout->addWidget(row.value);
        layout->addWidget(row.chip);

        m_seriesLayout->addWidget(row.widget);
        m_rows.insert(data.id, row);

        const quint32 id = data.id;
        connect(row.visible, &QCheckBox::toggled, this, [this, id](bool on) {
            m_canvas->setSeriesHidden(id, !on);
        });
        connect(row.chip, &QToolButton::clicked, this, [this, id] {
            changeSeriesColor(id);
        });
    }

    if (m_rows.isEmpty()) {
        auto *placeholder = new QLabel(tr("No series in this group"), m_seriesHost);
        placeholder->setObjectName(QStringLiteral("plotHint"));
        placeholder->setAlignment(Qt::AlignCenter);
        m_seriesLayout->addWidget(placeholder);
    }

    refreshTitle();
    refreshValues();
}

void PlotWindow::refreshValues()
{
    for (const PlotClient::SeriesData &data : m_client->series()) {
        const auto it = m_rows.constFind(data.id);
        if (it == m_rows.constEnd() || !it->value)
            continue;
        it->value->setText(data.samples.isEmpty() ? QStringLiteral("--")
                                                  : QString::number(double(data.samples.lastValue()),
                                                                    'g', 6));
    }

    const QString status =
            tr("Index: %1 - %2    Spacing: %3 px    Sampling: 1/%4    Samples: %5")
                    .arg(qRound64(m_canvas->viewMin()))
                    .arg(qRound64(m_canvas->viewMax()))
                    .arg(m_canvas->pixelSpacing(), 0, 'f', 2)
                    .arg(m_canvas->stride())
                    .arg(m_client->totalSamples());
    const qint64 hidden = m_canvas->hiddenRight();
    m_statusLabel->setText(hidden > 0 ? status + tr("    (+%1 new)").arg(hidden) : status);
}

void PlotWindow::refreshStatus()
{
    refreshTitle();
    // 没有数据时画布中央显示连接状态文字，状态一变就要重绘
    m_canvasDirty = true;
}

void PlotWindow::changeSeriesColor(quint32 seriesId)
{
    PlotClient::SeriesData *data = m_client->findSeries(seriesId);
    if (!data)
        return;
    const QColor picked = QColorDialog::getColor(seriesColor(*data), this, tr("Curve colour"));
    if (!picked.isValid())
        return;
    m_client->setSeriesColor(seriesId, picked);
    const auto row = m_rows.constFind(seriesId);
    if (row != m_rows.constEnd() && row->chip)
        row->chip->setStyleSheet(chipStyle(picked));
    m_canvas->update();
}

void PlotWindow::fitView()
{
    m_canvas->fit();
    // 暂停时铺满后画面是冻结的（Auto X 不会勾上），复选框以画布的实际状态为准
    const QSignalBlocker blockerX(m_autoXCheck);
    m_autoXCheck->setChecked(m_canvas->autoFitX());
    const QSignalBlocker blockerY(m_autoYCheck);
    m_autoYCheck->setChecked(m_canvas->autoY());
    m_canvasDirty = true;
}

void PlotWindow::setPaused(bool paused)
{
    m_pauseButton->setText(paused ? tr("Resume") : tr("Pause"));
    m_pauseButton->setIcon(paused ? PlotIcons::play(QColor(0xc0, 0x54, 0x20))
                                  : PlotIcons::pause(Qt::white));
    m_canvas->setPaused(paused);
    // 测量只在暂停时可用；恢复时画布会自动退出测量，复选框跟着取消
    m_measureCheck->setEnabled(paused);
    if (!paused)
        m_measureCheck->setChecked(false);
    m_canvasDirty = true;
}

void PlotWindow::applySuggestedPosition()
{
    if (m_positionApplied || !m_client->hasSuggestedPosition())
        return;
    if (m_client->state() != PlotClient::State::Connected)
        return;
    m_positionApplied = true;
    move(m_client->suggestedPosition());
}
