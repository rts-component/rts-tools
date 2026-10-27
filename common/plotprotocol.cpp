#include "common/plotprotocol.h"

#include <QDataStream>
#include <QIODevice>

namespace PlotProtocol {

namespace {

constexpr int kHeaderBytes = 5; // u32 payloadLen + u8 type
constexpr int kSampleBytes = 20; // 4+4+8+4

void configureStream(QDataStream &stream)
{
    stream.setByteOrder(QDataStream::BigEndian);
    stream.setVersion(QDataStream::Qt_6_2);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision); // float 固定 4 字节
}

} // namespace

QByteArray encodeFrame(MessageType type, const QByteArray &payload)
{
    QByteArray frame;
    frame.reserve(kHeaderBytes + payload.size());

    QDataStream stream(&frame, QIODevice::WriteOnly);
    configureStream(stream);
    stream << static_cast<quint32>(payload.size());
    stream << static_cast<quint8>(type);
    frame.append(payload);
    return frame;
}

void FrameReader::append(const QByteArray &data)
{
    m_buffer.append(data);
}

bool FrameReader::next(MessageType *type, QByteArray *payload)
{
    if (m_buffer.size() < kHeaderBytes)
        return false;

    QDataStream stream(m_buffer);
    configureStream(stream);
    quint32 length = 0;
    quint8 typeValue = 0;
    stream >> length >> typeValue;

    if (length > kMaxFrameBytes) {
        m_error = QStringLiteral("frame too large: %1 bytes").arg(length);
        m_buffer.clear();
        return false;
    }
    if (quint32(m_buffer.size()) < kHeaderBytes + length)
        return false;

    *payload = m_buffer.mid(kHeaderBytes, int(length));
    *type = static_cast<MessageType>(typeValue);
    m_buffer.remove(0, kHeaderBytes + int(length));
    return true;
}

void FrameReader::clear()
{
    m_buffer.clear();
    m_error.clear();
}

QByteArray encodeHello(quint16 version)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    configureStream(stream);
    stream << version;
    return payload;
}

bool decodeHello(const QByteArray &payload, quint16 *version)
{
    QDataStream stream(payload);
    configureStream(stream);
    stream >> *version;
    return stream.status() == QDataStream::Ok;
}

QByteArray encodeHelloAck(quint16 version, qint64 sessionStartMs)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    configureStream(stream);
    stream << version << sessionStartMs;
    return payload;
}

bool decodeHelloAck(const QByteArray &payload, quint16 *version, qint64 *sessionStartMs)
{
    QDataStream stream(payload);
    configureStream(stream);
    stream >> *version >> *sessionStartMs;
    return stream.status() == QDataStream::Ok;
}

QByteArray encodeLinkMeta(const LinkMeta &meta)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    configureStream(stream);
    stream << meta.key << meta.title << meta.outDir;
    return payload;
}

bool decodeLinkMeta(const QByteArray &payload, LinkMeta *meta)
{
    QDataStream stream(payload);
    configureStream(stream);
    stream >> meta->key >> meta->title >> meta->outDir;
    return stream.status() == QDataStream::Ok;
}

QByteArray encodeSeriesInfo(const SeriesMeta &meta)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    configureStream(stream);
    stream << meta.series << meta.key << meta.title << meta.unit << meta.color << meta.lastIndex;
    return payload;
}

bool decodeSeriesInfo(const QByteArray &payload, SeriesMeta *meta)
{
    QDataStream stream(payload);
    configureStream(stream);
    stream >> meta->series >> meta->key >> meta->title >> meta->unit >> meta->color
            >> meta->lastIndex;
    return stream.status() == QDataStream::Ok;
}

QByteArray encodeSeriesRemoved(quint32 series)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    configureStream(stream);
    stream << series;
    return payload;
}

bool decodeSeriesRemoved(const QByteArray &payload, quint32 *series)
{
    QDataStream stream(payload);
    configureStream(stream);
    stream >> *series;
    return stream.status() == QDataStream::Ok;
}

QByteArray encodeSamples(const QVector<Sample> &samples)
{
    QByteArray payload;
    payload.reserve(4 + samples.size() * kSampleBytes);
    QDataStream stream(&payload, QIODevice::WriteOnly);
    configureStream(stream);
    stream << static_cast<quint32>(samples.size());
    for (const Sample &sample : samples)
        stream << sample.series << sample.index << sample.timestampMs << sample.value;
    return payload;
}

bool decodeSamples(const QByteArray &payload, QVector<Sample> *samples)
{
    QDataStream stream(payload);
    configureStream(stream);
    quint32 count = 0;
    stream >> count;
    if (stream.status() != QDataStream::Ok)
        return false;
    // 每个点 20 字节，先按剩余长度挡掉明显损坏的帧
    if (qint64(count) * kSampleBytes > payload.size())
        return false;

    samples->resize(int(count));
    for (quint32 i = 0; i < count; ++i) {
        Sample &sample = (*samples)[int(i)];
        stream >> sample.series >> sample.index >> sample.timestampMs >> sample.value;
    }
    return stream.status() == QDataStream::Ok;
}

QByteArray encodeResync(qint64 fromMs)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    configureStream(stream);
    stream << fromMs;
    return payload;
}

bool decodeResync(const QByteArray &payload, qint64 *fromMs)
{
    QDataStream stream(payload);
    configureStream(stream);
    stream >> *fromMs;
    return stream.status() == QDataStream::Ok;
}

QByteArray encodeResyncData(const QVector<Sample> &samples, bool truncated)
{
    QByteArray payload;
    payload.reserve(5 + samples.size() * kSampleBytes);
    QDataStream stream(&payload, QIODevice::WriteOnly);
    configureStream(stream);
    stream << static_cast<quint32>(samples.size());
    stream << static_cast<quint8>(truncated ? 1 : 0);
    for (const Sample &sample : samples)
        stream << sample.series << sample.index << sample.timestampMs << sample.value;
    return payload;
}

bool decodeResyncData(const QByteArray &payload, QVector<Sample> *samples, bool *truncated)
{
    QDataStream stream(payload);
    configureStream(stream);
    quint32 count = 0;
    quint8 truncatedValue = 0;
    stream >> count >> truncatedValue;
    if (stream.status() != QDataStream::Ok)
        return false;
    if (qint64(count) * kSampleBytes + 5 > payload.size())
        return false;

    samples->resize(int(count));
    for (quint32 i = 0; i < count; ++i) {
        Sample &sample = (*samples)[int(i)];
        stream >> sample.series >> sample.index >> sample.timestampMs >> sample.value;
    }
    if (truncated)
        *truncated = truncatedValue != 0;
    return stream.status() == QDataStream::Ok;
}

QByteArray encodeSeriesColor(quint32 series, const QColor &color)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    configureStream(stream);
    stream << series << color;
    return payload;
}

bool decodeSeriesColor(const QByteArray &payload, quint32 *series, QColor *color)
{
    QDataStream stream(payload);
    configureStream(stream);
    stream >> *series >> *color;
    return stream.status() == QDataStream::Ok;
}

} // namespace PlotProtocol
