#ifndef PLOTENDPOINTS_H
#define PLOTENDPOINTS_H

#include <QColor>
#include <QPoint>
#include <QString>
#include <QVector>

// rts-tools 写出的端点清单（run/plot-endpoints.json），rts-plot 读取后连接。
// 读写两端都在 plotendpoints.cpp，字段名只在那里定义一份。
// 一个 group（CSV 面板的一行）对应一条管道和一个图表窗口，组内可以有多个系列。
struct PlotSeries
{
    quint32 id = 0;
    QString key;
    QString title;
    QString unit;
    QColor color;
};

struct PlotGroup
{
    QString key;
    QString title;
    QString pipe;
    QString outDir;
    QPoint position;
    bool hasPosition = false;
    bool attached = false;
    QVector<PlotSeries> series;
};

struct PlotEndpointFile
{
    bool valid = false;
    QString sessionId;
    qint64 sessionStartMs = 0;
    qint64 pid = 0;
    QVector<PlotGroup> groups;
    QString error;
};

// 与生产者约定的默认路径：<数据目录>/run/plot-endpoints.json
QString defaultEndpointsFilePath();
PlotEndpointFile readPlotEndpoints(const QString &path);
// 生产者（rts-tools）侧：把清单落盘；失败时 qWarning 并返回 false
bool writePlotEndpoints(const QString &path, const PlotEndpointFile &file);

#endif // PLOTENDPOINTS_H
