#ifndef PLOTPROTOCOL_H
#define PLOTPROTOCOL_H

#include <QByteArray>
#include <QColor>
#include <QMetaType>
#include <QString>
#include <QVector>
#include <QtGlobal>

// rts-tools.exe（生产者/服务端）与 rts-plot.exe（消费者/客户端）之间的本地管道协议。
//
// 一条管道对应 CSV 面板里的“一行”（一组数据）：该行勾选曲线的数字列都是这条管道里的一个系列，
// 因此它们会画在同一个图表里。
//
// 帧 = [u32 payloadLen][u8 type][payload]，payload 内部用 QDataStream（大端）序列化。
namespace PlotProtocol {

inline constexpr quint16 kVersion = 2;
inline constexpr quint32 kMaxFrameBytes = 16u * 1024u * 1024u;

enum class MessageType : quint8 {
    Hello = 1,         // client -> server：协议版本
    HelloAck = 2,      // server -> client：版本回执 + 会话起点
    LinkMeta = 3,      // server -> client：组标题 / 落盘目录
    SeriesInfo = 4,    // server -> client：一个系列（名称/颜色/已分配到第几个点）
    SeriesRemoved = 5, // server -> client：该系列不再显示
    Samples = 6,       // server -> client：一批采样点
    Resync = 8,        // client -> server：请求 ts >= fromMs 的补发
    ResyncData = 9,    // server -> client：补发结果
    Heartbeat = 10,    // 双向心跳
    SeriesColor = 11,  // client -> server：用户改了某个系列的颜色
};

// 一个采样点：组内系列号 + 该系列内序号 + 采集时刻（epoch 毫秒）+ 值。
// index 由主程序在入队前分配，每系列从 1 连续自增：固定采样率时可据此推算时间，
// 也是丢点/断线检测的依据。
struct Sample
{
    quint32 series = 0;
    quint32 index = 0;
    qint64 timestampMs = 0;
    float value = 0.0f;
};

struct LinkMeta
{
    QString key;   // 组 key（分组 id）
    QString title; // 组名
    QString outDir;
};

struct SeriesMeta
{
    quint32 series = 0; // 组内系列号（1 起）
    QString key;        // 系列稳定 key（组 id + 字段序号）
    QString title;
    QString unit;
    QColor color;
    quint32 lastIndex = 0; // 服务端该系列已分配到的最大序号
};

QByteArray encodeFrame(MessageType type, const QByteArray &payload);

// 增量拆帧：喂入任意分片，能拆多少拆多少。
class FrameReader
{
public:
    void append(const QByteArray &data);
    bool next(MessageType *type, QByteArray *payload);
    QByteArray takeRemainder(); // 取走未凑成整帧的残留字节
    void clear();
    bool hasError() const { return !m_error.isEmpty(); }
    QString errorString() const { return m_error; }

private:
    QByteArray m_buffer;
    QString m_error;
};

QByteArray encodeHello(quint16 version);
bool decodeHello(const QByteArray &payload, quint16 *version);

QByteArray encodeHelloAck(quint16 version, qint64 sessionStartMs);
bool decodeHelloAck(const QByteArray &payload, quint16 *version, qint64 *sessionStartMs);

QByteArray encodeLinkMeta(const LinkMeta &meta);
bool decodeLinkMeta(const QByteArray &payload, LinkMeta *meta);

QByteArray encodeSeriesInfo(const SeriesMeta &meta);
bool decodeSeriesInfo(const QByteArray &payload, SeriesMeta *meta);

QByteArray encodeSeriesRemoved(quint32 series);
bool decodeSeriesRemoved(const QByteArray &payload, quint32 *series);

QByteArray encodeSamples(const QVector<Sample> &samples);
bool decodeSamples(const QByteArray &payload, QVector<Sample> *samples);


QByteArray encodeResync(qint64 fromMs);
bool decodeResync(const QByteArray &payload, qint64 *fromMs);

QByteArray encodeResyncData(const QVector<Sample> &samples, bool truncated);
bool decodeResyncData(const QByteArray &payload, QVector<Sample> *samples, bool *truncated);

// 用户在曲线窗口里改颜色：回传给主程序保存，保证下次启动还是这个颜色
QByteArray encodeSeriesColor(quint32 series, const QColor &color);
bool decodeSeriesColor(const QByteArray &payload, quint32 *series, QColor *color);

} // namespace PlotProtocol

Q_DECLARE_METATYPE(PlotProtocol::Sample)

#endif // PLOTPROTOCOL_H
