#include "plot/viewer/plotcanvas.h"

#include <QDateTime>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

const QColor kCanvasBackground(0x1c, 0x1c, 0x1c);
const QColor kGridColor(0x33, 0x33, 0x33);
const QColor kAxisText(0xa8, 0xa8, 0xa8);
const QColor kCursorLine(0xb8, 0xb8, 0xb8);
const QColor kReadoutBackground(0x5c, 0x3f, 0x45);

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

// 样本按 index 递增：二分出落在 [xMin, xMax] 内的区间 [begin, end)
void visibleSpan(const SampleView &samples, double xMin, double xMax, int *begin, int *end)
{
    *begin = samples.lowerBound(xMin);
    *end = samples.upperBound(xMax);
}

} // namespace

PlotCanvas::PlotCanvas(PlotClient *client, QWidget *parent)
    : QWidget(parent)
    , m_client(client)
{
    setMinimumSize(420, 260);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

bool PlotCanvas::addHLine(const HLine &line)
{
    if (m_hlines.size() >= kMaxHLines)
        return false;
    m_hlines.append(line);
    update();
    return true;
}

void PlotCanvas::removeHLine(int index)
{
    if (index < 0 || index >= m_hlines.size())
        return;
    m_hlines.removeAt(index);
    update();
}

void PlotCanvas::setHLine(int index, const HLine &line)
{
    if (index < 0 || index >= m_hlines.size())
        return;
    m_hlines[index] = line;
    update();
}

// 当前视口 Y 范围的中点，新加的水平线放在这里，一定看得见
double PlotCanvas::viewCenterY() const
{
    return m_cacheRange.valid ? (m_cacheRange.yMin + m_cacheRange.yMax) / 2.0 : 0.0;
}

// AutoFit 表示“全部数据塞进视口”；follow 表示“右边缘跟随最新数据”
void PlotCanvas::setAutoFitX(bool on)
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

void PlotCanvas::setAutoY(bool on)
{
    if (m_autoY == on)
        return;
    m_autoY = on;
    if (!on) {
        m_manualYMin = m_lastYMin;
        m_manualYMax = m_lastYMax;
    }
    update();
}

void PlotCanvas::fit()
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
void PlotCanvas::fitFrozen()
{
    double low = 0;
    double high = 0;
    if (dataIndexRange(&low, &high)) {
        m_pauseLimit = high; // “铺满一次”会把此刻的全部数据放进视口，之后到达的才算新数据
        m_left = low;
        m_span = qMax(1.0, high - low);
    }
    m_autoFit = false;
    m_follow = false;
    update();
}

void PlotCanvas::setSeriesHidden(quint32 id, bool hidden)
{
    if (hidden)
        m_hidden.insert(id);
    else
        m_hidden.remove(id);
    update();
}

void PlotCanvas::setPaused(bool paused)
{
    if (m_paused == paused)
        return;
    m_paused = paused;
    if (paused) {
        double low = 0;
        double high = 0;
        m_pauseLimit = dataIndexRange(&low, &high) ? high : 0;
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
        setMeasureY(false);
    }
    update();
}

// 测量竖线：只在暂停时可用，两根线可以用鼠标拖动，显示两线之间的 index 差和时间差
void PlotCanvas::setMeasure(bool on)
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

// Y 方向测量：两根水平线（按数值定位），显示两线之间的数值差
void PlotCanvas::setMeasureY(bool on)
{
    if (on && !m_paused)
        return;
    if (m_measureY == on)
        return;
    m_measureY = on;
    m_measureDragY = -1;
    if (on && m_cacheRange.valid) {
        const double span = m_cacheRange.yMax - m_cacheRange.yMin;
        m_measureYAt[0] = m_cacheRange.yMin + span / 3.0;
        m_measureYAt[1] = m_cacheRange.yMin + span * 2.0 / 3.0;
    }
    setCursor(Qt::ArrowCursor);
    update();
}

void PlotCanvas::paintEvent(QPaintEvent *event)
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
    if (m_measureY)
        drawMeasureY(painter, area, range);
    drawCursor(painter, area, range);
}

void PlotCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (m_measureDragY >= 0 && (event->buttons() & Qt::LeftButton)) {
        if (m_cacheRange.valid) {
            const QRectF area = plotArea();
            const double ratio = (area.bottom() - event->position().y())
                    / qMax(1.0, area.height());
            m_measureYAt[m_measureDragY] = m_cacheRange.yMin
                    + ratio * (m_cacheRange.yMax - m_cacheRange.yMin);
        }
        update();
        return;
    }
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
                index = qBound(low, index, visibleLimit(high)); // 暂停后到达的点不可测
            m_measureAt[m_measureDrag] = index;
        }
        update();
        return;
    }
    if ((m_measure || m_measureY) && !(event->buttons() & Qt::LeftButton)) {
        if (m_measure && measureLineAt(event->position().x()) >= 0)
            setCursor(Qt::SizeHorCursor);
        else if (m_measureY && measureHLineAt(event->position().y()) >= 0)
            setCursor(Qt::SizeVerCursor);
        else
            setCursor(Qt::ArrowCursor);
    }
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

void PlotCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_measure) {
        const int line = measureLineAt(event->position().x());
        if (line >= 0) {
            m_measureDrag = line;
            return;
        }
    }
    if (event->button() == Qt::LeftButton && m_measureY) {
        const int line = measureHLineAt(event->position().y());
        if (line >= 0) {
            m_measureDragY = line;
            return;
        }
    }
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_lastDrag = event->position().toPoint();
    }
}

void PlotCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
        m_measureDrag = -1;
        m_measureDragY = -1;
    }
}

void PlotCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    Q_UNUSED(event)
    fit();
}

void PlotCanvas::wheelEvent(QWheelEvent *event)
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
        // 只有当窗口重新覆盖到最新数据时才恢复“整体滚动”，否则停在历史区间不动；
        // 暂停中无论如何都不跟随，否则画面会随新数据继续滚动
        m_follow = !m_paused && (m_left + span >= m_dataMax);
    }
    update();
}

void PlotCanvas::leaveEvent(QEvent *event)
{
    Q_UNUSED(event)
    m_cursorValid = false;
    update();
}

// 暂停后画面冻结：暂停之后才到达的点即使落进视口（缩小/平移到数据右侧）也不画
double PlotCanvas::visibleLimit(double xMax) const
{
    return m_paused ? qMin(xMax, m_pauseLimit) : xMax;
}

// 影响曲线画面的全部输入（数据、视口、显示开关、尺寸）的摘要
quint64 PlotCanvas::cacheKey() const
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
            visibleSpan(series.samples, m_left, visibleLimit(m_left + m_span), &begin, &end);
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
        mix(seriesDisplayColor(series).rgba());
    }
    return hash;
}

QRectF PlotCanvas::plotArea() const
{
    return QRectF(rect()).adjusted(56, 12, -14, -34);
}

bool PlotCanvas::seriesVisible(const PlotClient::SeriesData &series) const
{
    return !m_hidden.contains(series.id) && !series.samples.isEmpty();
}

PlotCanvas::Range PlotCanvas::computeRange()
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
        visibleSpan(series.samples, range.xMin, visibleLimit(range.xMax), &begin, &end);
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

double PlotCanvas::mapX(double index, const QRectF &area, const Range &range) const
{
    return area.left() + (index - range.xMin) / (range.xMax - range.xMin) * area.width();
}

double PlotCanvas::mapY(double value, const QRectF &area, const Range &range) const
{
    return area.bottom() - (value - range.yMin) / (range.yMax - range.yMin) * area.height();
}

void PlotCanvas::drawGrid(QPainter &painter, const QRectF &area, const Range &range) const
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

void PlotCanvas::drawSeries(QPainter &painter, const QRectF &area, const Range &range) const
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
        visibleSpan(series.samples, range.xMin, visibleLimit(range.xMax), &begin, &end);
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
        painter.setPen(QPen(seriesDisplayColor(series), 1.0));
        painter.drawPolyline(polyline);
    }

    painter.restore();
}

// 可见曲线里最小/最大的 index（测量线只能拖在有数据的范围内）
bool PlotCanvas::dataIndexRange(double *low, double *high) const
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
int PlotCanvas::measureLineAt(double x) const
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

// 鼠标 y 附近（6 像素内）的 Y 测量线，没有返回 -1；两根重合时优先后一根
int PlotCanvas::measureHLineAt(double y) const
{
    if (!m_cacheRange.valid)
        return -1;
    const QRectF area = plotArea();
    int best = -1;
    double bestDistance = 6.0;
    for (int k = 1; k >= 0; --k) {
        const double distance = qAbs(mapY(m_measureYAt[k], area, m_cacheRange) - y);
        if (distance <= bestDistance) {
            best = k;
            bestDistance = distance;
        }
    }
    return best;
}

// 某个 index 对应的采集时刻（所有曲线共用时间轴，取任意一条可见曲线即可）
bool PlotCanvas::timestampOf(double index, qint64 *timestampMs) const
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

void PlotCanvas::drawMeasure(QPainter &painter, const QRectF &area, const Range &range) const
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

void PlotCanvas::drawMeasureY(QPainter &painter, const QRectF &area, const Range &range) const
{
    static const QColor kColors[2] = { QColor(0xff, 0xb7, 0x4d), QColor(0x4d, 0xd0, 0xe1) };
    static const char *const kNames[2] = { "C", "D" };

    painter.save();
    painter.setClipRect(area.adjusted(-1, -1, 1, 1));
    QFont font = painter.font();
    font.setPointSizeF(qMax(7.5, font.pointSizeF() - 1.0));
    painter.setFont(font);
    const int lineHeight = painter.fontMetrics().height() + 2;

    for (int k = 0; k < 2; ++k) {
        const double y = mapY(m_measureYAt[k], area, range);
        if (y < area.top() || y > area.bottom())
            continue;
        painter.setPen(QPen(kColors[k], 1.5));
        painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));

        // 右侧小标签：名字 + 数值，贴边时翻到线的另一侧
        const QString label = QStringLiteral("%1 %2").arg(QLatin1String(kNames[k]),
                                                          formatAxis(m_measureYAt[k]));
        const int labelWidth = painter.fontMetrics().horizontalAdvance(label) + 10;
        QRectF tag(area.right() - labelWidth - 2 - k * (labelWidth + 2), y - lineHeight - 2,
                   labelWidth, lineHeight);
        if (tag.top() < area.top())
            tag.moveTop(y + 2);
        painter.setPen(Qt::NoPen);
        painter.setBrush(kColors[k]);
        painter.drawRect(tag);
        painter.setPen(QColor(0x20, 0x20, 0x20));
        painter.drawText(tag, Qt::AlignCenter, label);
    }

    // 差值读数框：绘图区底部中间；X 测量同时开启时叠在它上方
    const QString text = tr("ΔY: %1").arg(formatAxis(qAbs(m_measureYAt[1] - m_measureYAt[0])));
    const int boxWidth = painter.fontMetrics().horizontalAdvance(text) + 24;
    const double bottom = area.bottom() - 14 - (m_measure ? lineHeight + 12 : 0);
    const QRectF box(area.center().x() - boxWidth / 2.0, bottom - lineHeight - 8, boxWidth,
                     lineHeight + 8);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(kReadoutBackground.red(), kReadoutBackground.green(),
                            kReadoutBackground.blue(), 235));
    painter.drawRect(box);
    painter.setPen(QColor(0xf0, 0xf0, 0xf0));
    painter.drawText(box, Qt::AlignCenter, text);
    painter.restore();
}

void PlotCanvas::drawHLines(QPainter &painter, const QRectF &area, const Range &range) const
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
bool PlotCanvas::sampleAt(quint32 index, SampleView::Point *point) const
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

void PlotCanvas::drawCursor(QPainter &painter, const QRectF &area, const Range &range) const
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
        lines.append(qMakePair(seriesDisplayColor(series),
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
