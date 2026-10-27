#ifndef PLOTICONS_H
#define PLOTICONS_H

#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRectF>

// 曲线窗口用的小图标：全部用 QPainter 画，避免不同系统下 Unicode 字符被渲染成彩色方块。
namespace PlotIcons {

inline QPixmap canvas(int size)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    return pixmap;
}

inline QIcon pause(const QColor &color, int size = 16)
{
    QPixmap pixmap = canvas(size);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    const qreal barWidth = size * 0.20;
    const qreal top = size * 0.18;
    const qreal height = size * 0.64;
    painter.drawRoundedRect(QRectF(size * 0.26, top, barWidth, height), 1.0, 1.0);
    painter.drawRoundedRect(QRectF(size * 0.54, top, barWidth, height), 1.0, 1.0);
    return QIcon(pixmap);
}

inline QIcon play(const QColor &color, int size = 16)
{
    QPixmap pixmap = canvas(size);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    QPainterPath triangle;
    triangle.moveTo(size * 0.30, size * 0.18);
    triangle.lineTo(size * 0.78, size * 0.50);
    triangle.lineTo(size * 0.30, size * 0.82);
    triangle.closeSubpath();
    painter.drawPath(triangle);
    return QIcon(pixmap);
}

inline QIcon fit(const QColor &color, int size = 16)
{
    QPixmap pixmap = canvas(size);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, qMax(1.2, size * 0.09));
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const qreal margin = size * 0.16;
    const qreal arm = size * 0.26;
    const qreal left = margin;
    const qreal right = size - margin;
    const qreal top = margin;
    const qreal bottom = size - margin;

    // 左上
    painter.drawLine(QPointF(left, top + arm), QPointF(left, top));
    painter.drawLine(QPointF(left, top), QPointF(left + arm, top));
    // 右上
    painter.drawLine(QPointF(right - arm, top), QPointF(right, top));
    painter.drawLine(QPointF(right, top), QPointF(right, top + arm));
    // 左下
    painter.drawLine(QPointF(left, bottom - arm), QPointF(left, bottom));
    painter.drawLine(QPointF(left, bottom), QPointF(left + arm, bottom));
    // 右下
    painter.drawLine(QPointF(right - arm, bottom), QPointF(right, bottom));
    painter.drawLine(QPointF(right, bottom), QPointF(right, bottom - arm));
    return QIcon(pixmap);
}

inline QIcon trash(const QColor &color, int size = 16)
{
    QPixmap pixmap = canvas(size);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, qMax(1.2, size * 0.09));
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const qreal left = size * 0.26;
    const qreal right = size * 0.74;
    const qreal top = size * 0.28;
    const qreal bottom = size * 0.82;
    // 盖子
    painter.drawLine(QPointF(size * 0.18, top), QPointF(size * 0.82, top));
    // 提手
    painter.drawLine(QPointF(size * 0.40, top), QPointF(size * 0.40, size * 0.18));
    painter.drawLine(QPointF(size * 0.40, size * 0.18), QPointF(size * 0.60, size * 0.18));
    painter.drawLine(QPointF(size * 0.60, size * 0.18), QPointF(size * 0.60, top));
    // 桶身
    painter.drawLine(QPointF(left, top), QPointF(left + size * 0.04, bottom));
    painter.drawLine(QPointF(right, top), QPointF(right - size * 0.04, bottom));
    painter.drawLine(QPointF(left + size * 0.04, bottom), QPointF(right - size * 0.04, bottom));
    // 内部两条竖线
    painter.drawLine(QPointF(size * 0.42, size * 0.42), QPointF(size * 0.43, size * 0.70));
    painter.drawLine(QPointF(size * 0.58, size * 0.42), QPointF(size * 0.57, size * 0.70));
    return QIcon(pixmap);
}

} // namespace PlotIcons

#endif // PLOTICONS_H
