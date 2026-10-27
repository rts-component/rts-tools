#ifndef PLOTCLIENT_H
#define PLOTCLIENT_H

#include "plotendpoints.h"
#include "plotprotocol.h"
#include "sampletable.h"

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

class QLocalSocket;
class QTimer;

// rts-plot.exe 侧的数据接收端：连接一条组管道，拆帧、按系列累积采样点、统计缺口。
// 断线后按固定间隔重试；用稳定 group key 连接时会重新读取端点文件（主程序重启后管道名会变）。
class PlotClient : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Idle,       // 未选组
        Resolving,  // 正在端点文件里找管道
        Connecting, // 正在连接
        Connected,
        Retrying,   // 断开后等待重试
        Failed,     // 放弃重试
    };

    struct SeriesData
    {
        quint32 id = 0;
        QString key;
        QString title;
        QString unit;
        QColor color;
        SampleView samples; // 指向 m_table 里属于这条曲线的一列
        quint32 lastIndex = 0;
        quint64 received = 0;
    };

    explicit PlotClient(QObject *parent = nullptr);
    ~PlotClient() override;

    void setEndpointsFilePath(const QString &path);
    QString endpointsFilePath() const { return m_endpointsPath; }

    // 内部硬上限（行数）：每行 12 + 4N 字节（N = 曲线条数）
    static constexpr int kMaxSamplesPerSeries = 1000000;

    // 保留的采样行数上限（0 = 不限制，用内部硬上限）
    void setSampleLimit(int limit);
    void clearAllSamples();

    void connectToGroup(const QString &groupKey);
    void connectToPipe(const QString &pipe, const QString &groupKey = QString());
    void disconnectFromServer();
    // 改某个系列的颜色：本地立即生效；在线时回传主程序持久化
    void setSeriesColor(quint32 seriesId, const QColor &color);
    void setAutoReconnect(bool on) { m_autoReconnect = on; }
    void setRetryInterval(int ms);

    State state() const { return m_state; }
    QString stateText() const;
    QString placeholder() const { return m_placeholder; }

    QString groupKey() const { return m_groupKey; }
    QString pipe() const { return m_pipe; }
    QString title() const { return m_title; }
    QPoint suggestedPosition() const { return m_suggestedPosition; }
    bool hasSuggestedPosition() const { return m_hasSuggestedPosition; }

    const QVector<SeriesData> &series() const { return m_series; }
    SeriesData *findSeries(quint32 id);
    quint64 totalSamples() const;
    bool isEmpty() const;

signals:
    void stateChanged();
    void metaChanged();
    void samplesAppended();

private:
    void setState(State state, const QString &placeholder = QString());
    void openSocket(const QString &pipe);
    void handleReadyRead();
    void handleDisconnected();
    void handleFrame(PlotProtocol::MessageType type, const QByteArray &payload);
    void appendSamples(const QVector<PlotProtocol::Sample> &samples);
    void applySeriesMeta(const PlotProtocol::SeriesMeta &meta);
    SeriesData &ensureSeries(quint32 id);
    void removeSeries(quint32 series);
    void resetData();
    void scheduleRetry();
    bool resolveEndpoint(const QString &groupKey, PlotGroup *group);
    void writeSeriesColor(quint32 seriesId, const QColor &color);
    int indexOfSeries(quint32 id) const;

    QLocalSocket *m_socket = nullptr;
    PlotProtocol::FrameReader m_reader;
    QTimer *m_retryTimer = nullptr;
    QTimer *m_heartbeatTimer = nullptr;
    QString m_endpointsPath;
    QString m_groupKey;
    QString m_pipe;
    QString m_title;
    QString m_placeholder;
    SampleTable m_table;
    QVector<SeriesData> m_series;
    QHash<QString, QColor> m_colorOverrides;
    State m_state = State::Idle;
    quint16 m_peerVersion = 0;
    QPoint m_suggestedPosition{-1, -1};
    bool m_hasSuggestedPosition = false;
    qint64 m_lastReceivedMs = 0;
    qint64 m_sessionStartMs = 0;
    int m_retryIntervalMs = 2000;
    int m_sampleLimit = 0;
    bool m_autoReconnect = true;
};

#endif // PLOTCLIENT_H
