#ifndef CSVICONS_H
#define CSVICONS_H

#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QPolygon>

// 用绘制出来的小图标代替字体符号，避免不同机器上缺字形显示成空框
namespace CsvIcons {

inline QIcon chart(bool on)
{
    QPixmap pixmap(14, 14);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor color = on ? QColor(0x0b, 0x63, 0xa8) : QColor(0xd6, 0xe4, 0xf2);
    painter.setPen(QPen(color, 1.6));
    painter.drawPolyline(QPolygon() << QPoint(2, 9) << QPoint(5, 5) << QPoint(8, 7)
                                    << QPoint(12, 2));
    painter.setPen(QPen(color, 1.2));
    painter.drawLine(2, 12, 12, 12);
    return QIcon(pixmap);
}

inline QIcon pencil()
{
    QPixmap pixmap(14, 14);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(0x4a, 0x55, 0x63), 1.4));
    painter.drawLine(3, 11, 10, 4);
    painter.drawLine(4, 12, 11, 5);
    painter.drawLine(10, 4, 11, 5);
    painter.drawLine(3, 11, 4, 12);
    painter.setPen(QPen(QColor(0x4a, 0x55, 0x63), 1.0));
    painter.drawLine(2, 12, 4, 12);
    return QIcon(pixmap);
}

inline QIcon menu()
{
    QPixmap pixmap(14, 14);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setPen(QPen(QColor(0x5b, 0x64, 0x70), 1.6));
    for (int y : {4, 7, 10})
        painter.drawLine(3, y, 11, y);
    return QIcon(pixmap);
}

inline QIcon copy()
{
    QPixmap pixmap(14, 14);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setPen(QPen(QColor(0x4a, 0x55, 0x63), 1.2));
    painter.drawRect(2, 4, 8, 8);
    painter.drawRect(5, 2, 8, 8);
    return QIcon(pixmap);
}

} // namespace CsvIcons

#endif // CSVICONS_H
