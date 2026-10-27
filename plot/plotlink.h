#ifndef PLOTLINK_H
#define PLOTLINK_H

#include "plotprotocol.h"

#include <QColor>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QLocalServer;
class QLocalSocket;
class QTimer;

// 主程序侧的曲线数据出口：CSV 面板的一“行”（一组数据）对应一条本地管道，
// 该行里勾选曲线的数字列就是这条管道里的多个系列 —— 因此它们画在同一个图表里。
// 曲线进程崩溃/关闭只会让对应的图表变成“离线”，不影响串口接收、日志与 CSV 解析。
class PlotLink : public QObject
{
    Q_OBJECT

public:
    struct SeriesDefinition
    {
        QString key; // 系列稳定标识（分组 id + 字段序号）
        QString title;
        QString unit;
        QColor color;
    };

    struct GroupDefinition
    {
        QString key;   // 分组 id
        QString title; // 行名
        QVector<SeriesDefinition> series;
    };

    explicit PlotLink(QObject *parent = nullptr);
    ~PlotLink() override;

    void setOutputDir(const QString &dir);
    QString outputDir() const { return m_outputDir; }

    // 重建分组集合：保留同 key 的分组（及其序号/热数据），其余新建/移除
    void syncSeries(const QVector<GroupDefinition> &groups);

    // index 与时间戳在这里分配；未配置曲线的系列直接忽略
    void append(const QString &seriesKey, double value, qint64 timestampMs);

    bool isAttached(const QString &groupKey) const;
    QString pipeName(const QString &groupKey) const;
    QString groupTitleOf(const QString &groupKey) const;
    QStringList groupKeys() const;
    QVector<GroupDefinition> groupDefinitions() const;

    QString sessionId() const { return m_sessionId; }
    QString endpointsFilePath() const;

    // 立即把端点清单写盘（平时由内部定时器在状态变化后自动写）
    void writeEndpointsFile();

signals:
    void attachmentChanged(const QString &groupKey, bool attached);
    // 用户在曲线窗口里改了某条曲线的颜色（key 是系列稳定标识）
    void seriesColorChanged(const QString &seriesKey, const QColor &color);

private:
    struct SeriesState
    {
        SeriesDefinition definition;
        quint32 id = 0; // 连接内系列号（1 起，尽量稳定）
        quint32 nextIndex = 1;
    };

    struct Link
    {
        QString key;
        QString title;
        QString pipe;
        QLocalServer *server = nullptr;
        QLocalSocket *socket = nullptr;
        QVector<SeriesState> series;
        QHash<QString, int> seriesIndexByKey;
        quint32 nextSeriesId = 1;
        QVector<PlotProtocol::Sample> pending;
        QVector<PlotProtocol::Sample> hot; // 最近 60 s，重连补发用
        QByteArray incoming;               // 拆帧残留
        qint64 lastReceivedMs = 0;
    };

    Link *findLink(const QString &groupKey) const;
    Link *findLinkBySeries(const QString &seriesKey, int *seriesIndex) const;
    void createLink(const GroupDefinition &group);
    void destroyLink(Link *link);
    void applySeries(Link *link, const QVector<SeriesDefinition> &series);
    void attachSocket(Link *link, QLocalSocket *socket);
    void handleSocketData(Link *link);
    void handleClientFrame(Link *link, PlotProtocol::MessageType type, const QByteArray &payload);
    void detachSocket(Link *link);
    void sendHandshake(Link *link);
    void sendSeriesInfo(Link *link, const SeriesState &series);
    void flushLink(Link *link);
    void trimHot(Link *link);

    QString suggestPosition(int order) const;

    QVector<Link *> m_links;
    QHash<QString, Link *> m_byGroup;
    QHash<QString, Link *> m_bySeries;
    QTimer *m_flushTimer = nullptr;
    QTimer *m_heartbeatTimer = nullptr;
    QTimer *m_endpointsTimer = nullptr;
    QString m_sessionId;
    qint64 m_sessionStartMs = 0;
    QString m_outputDir;
    bool m_endpointsDirty = true;
};

#endif // PLOTLINK_H
