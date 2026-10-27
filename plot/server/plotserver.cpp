#include "plot/server/plotserver.h"

#include "common/plotendpoints.h"
#include "common/plotpaths.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSaveFile>
#include <QSet>
#include <QTimer>

namespace {

constexpr qint64 kHotWindowMs = 60 * 1000;  // 重连补发的历史跨度
constexpr int kHotMaxSamples = 400000;      // 补发缓冲上限（防止极高速率撑爆内存）
constexpr int kMaxPendingSamples = 1000000; // 待发缓冲上限
constexpr int kMaxBatchSamples = 4096;      // 单帧最多点数
constexpr qint64 kMaxBacklogBytes = 32 * 1024 * 1024;
constexpr int kFlushIntervalMs = 16;
constexpr int kEndpointsWriteIntervalMs = 2000; // 端点清单有改动时的落盘周期

} // namespace

PlotServer::PlotServer(QObject *parent)
    : QObject(parent)
    , m_sessionId(QStringLiteral("%1-%2")
                          .arg(QDateTime::currentMSecsSinceEpoch())
                          .arg(QCoreApplication::applicationPid()))
    , m_sessionStartMs(QDateTime::currentMSecsSinceEpoch())
    , m_outputDir(appDataDir() + QStringLiteral("/logs/curves"))
{
    m_flushTimer = new QTimer(this);
    m_flushTimer->setInterval(kFlushIntervalMs);
    connect(m_flushTimer, &QTimer::timeout, this, [this] {
        for (Link *link : std::as_const(m_links))
            flushLink(link);
    });
    m_flushTimer->start();

    m_heartbeatTimer = new QTimer(this);
    m_heartbeatTimer->setInterval(PlotProtocol::kHeartbeatIntervalMs);
    connect(m_heartbeatTimer, &QTimer::timeout, this, [this] {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (Link *link : std::as_const(m_links)) {
            if (!isSocketConnected(link))
                continue;
            if (link->lastReceivedMs > 0 && now - link->lastReceivedMs > PlotProtocol::kStaleTimeoutMs) {
                qWarning("Plot link %s stopped responding, dropping it",
                         qUtf8Printable(link->key));
                detachSocket(link);
                continue;
            }
            send(link, PlotProtocol::MessageType::Heartbeat);
        }
    });
    m_heartbeatTimer->start();

    m_endpointsTimer = new QTimer(this);
    m_endpointsTimer->setInterval(kEndpointsWriteIntervalMs);
    connect(m_endpointsTimer, &QTimer::timeout, this, [this] {
        if (m_endpointsDirty)
            writeEndpointsFile();
    });
    m_endpointsTimer->start();
}

PlotServer::~PlotServer()
{
    while (!m_links.isEmpty())
        removeLink(m_links.last());
}

void PlotServer::setOutputDir(const QString &dir)
{
    if (dir.isEmpty() || dir == m_outputDir)
        return;
    m_outputDir = dir;
    m_endpointsDirty = true;
}

PlotServer::Link *PlotServer::findLink(const QString &groupKey) const
{
    return m_byGroup.value(groupKey, nullptr);
}

PlotServer::Link *PlotServer::findLinkBySeries(const QString &seriesKey, int *seriesIndex) const
{
    Link *link = m_bySeries.value(seriesKey, nullptr);
    if (!link)
        return nullptr;
    if (seriesIndex)
        *seriesIndex = link->seriesIndexByKey.value(seriesKey, -1);
    return link;
}

void PlotServer::syncSeries(const QVector<GroupDefinition> &groups)
{
    QSet<QString> wanted;
    for (const GroupDefinition &group : groups)
        wanted.insert(group.key);

    for (int i = m_links.size() - 1; i >= 0; --i) {
        Link *link = m_links.at(i);
        if (!wanted.contains(link->key))
            removeLink(link);
    }

    for (int order = 0; order < groups.size(); ++order) {
        const GroupDefinition &group = groups.at(order);
        Link *link = findLink(group.key);
        if (!link) {
            createLink(group);
            continue;
        }
        if (link->title != group.title) {
            link->title = group.title;
            sendLinkMeta(link);
            m_endpointsDirty = true;
        }
        applySeries(link, group.series);
    }

    m_endpointsDirty = true;
}

void PlotServer::createLink(const GroupDefinition &group)
{
    auto *link = new Link;
    link->key = group.key;
    link->title = group.title;
    link->pipe = QStringLiteral("rts-plot-%1-%2").arg(m_sessionId, group.key);

    link->server = new QLocalServer(this);
    link->server->setSocketOptions(QLocalServer::UserAccessOption);
    connect(link->server, &QLocalServer::newConnection, this, [this, link] {
        while (link->server->hasPendingConnections()) {
            QLocalSocket *socket = link->server->nextPendingConnection();
            if (link->socket) {
                qWarning("Plot link %s already in use, rejecting extra connection",
                         qUtf8Printable(link->key));
                socket->abort();
                socket->deleteLater();
                continue;
            }
            attachSocket(link, socket);
        }
    });

    if (!link->server->listen(link->pipe)) {
        qWarning("Cannot listen on plot pipe %s: %s", qUtf8Printable(link->pipe),
                 qUtf8Printable(link->server->errorString()));
    }

    m_links.append(link);
    m_byGroup.insert(link->key, link);
    applySeries(link, group.series);
}

void PlotServer::applySeries(Link *link, const QVector<SeriesDefinition> &series)
{
    QSet<QString> wanted;
    for (const SeriesDefinition &definition : series)
        wanted.insert(definition.key);

    // 移除不再需要的系列
    for (int i = link->series.size() - 1; i >= 0; --i) {
        SeriesState &state = link->series[i];
        if (wanted.contains(state.definition.key))
            continue;
        send(link, PlotProtocol::MessageType::SeriesRemoved,
             PlotProtocol::encodeSeriesRemoved(state.id));
        m_bySeries.remove(state.definition.key);
        link->seriesIndexByKey.remove(state.definition.key);
        link->series.removeAt(i);
        m_endpointsDirty = true;
    }

    // 新增或更新
    for (const SeriesDefinition &definition : series) {
        const bool known = link->seriesIndexByKey.contains(definition.key);
        if (!known) {
            SeriesState state;
            state.definition = definition;
            state.id = link->nextSeriesId++;
            link->series.append(state);
            link->seriesIndexByKey.insert(definition.key, link->series.size() - 1);
            m_bySeries.insert(definition.key, link);
            m_endpointsDirty = true;
            sendSeriesInfo(link, link->series.last());
            continue;
        }

        SeriesState &state = link->series[link->seriesIndexByKey.value(definition.key)];
        if (state.definition.title == definition.title
            && state.definition.unit == definition.unit
            && state.definition.color == definition.color) {
            continue;
        }
        state.definition.title = definition.title;
        state.definition.unit = definition.unit;
        state.definition.color = definition.color;
        m_endpointsDirty = true;
        sendSeriesInfo(link, state);
    }
}

void PlotServer::removeLink(Link *link)
{
    m_byGroup.remove(link->key);
    for (const SeriesState &series : std::as_const(link->series))
        m_bySeries.remove(series.definition.key);
    m_links.removeOne(link);
    destroyLink(link);
}

void PlotServer::destroyLink(Link *link)
{
    if (!link)
        return;
    if (link->socket) {
        link->socket->disconnect(this);
        link->socket->abort();
        link->socket->deleteLater();
        link->socket = nullptr;
    }
    if (link->server) {
        link->server->close();
        link->server->deleteLater();
        link->server = nullptr;
    }
    delete link;
}

void PlotServer::attachSocket(Link *link, QLocalSocket *socket)
{
    link->socket = socket;
    link->lastReceivedMs = QDateTime::currentMSecsSinceEpoch();
    link->reader.clear();   // 上一个连接可能留下半帧字节，不能带进新连接
    link->pending.clear();  // 旧队列由重连补发（ResyncData）覆盖，避免重复/乱序
    socket->setParent(this);

    connect(socket, &QLocalSocket::readyRead, this, [this, link, socket] {
        if (link->socket != socket)
            return;
        handleSocketData(link);
    });
    connect(socket, &QLocalSocket::disconnected, this, [this, link, socket] {
        if (link->socket != socket)
            return;
        detachSocket(link);
    });
    connect(socket, &QLocalSocket::errorOccurred, this,
            [this, link, socket](QLocalSocket::LocalSocketError) {
                if (link->socket != socket)
                    return;
                detachSocket(link);
            });

    emit attachmentChanged(link->key, true);
    m_endpointsDirty = true;
}

void PlotServer::detachSocket(Link *link)
{
    if (!link->socket)
        return;
    QLocalSocket *socket = link->socket;
    link->socket = nullptr;
    link->reader.clear();
    link->pending.clear();
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();

    emit attachmentChanged(link->key, false);
    m_endpointsDirty = true;
}

void PlotServer::handleSocketData(Link *link)
{
    if (!link->socket)
        return;

    link->reader.append(link->socket->readAll());
    link->lastReceivedMs = QDateTime::currentMSecsSinceEpoch();

    PlotProtocol::MessageType type;
    QByteArray payload;
    while (link->socket && link->reader.next(&type, &payload))
        handleClientFrame(link, type, payload);

    if (link->socket && link->reader.hasError()) {
        qWarning("Plot link %s protocol error: %s", qUtf8Printable(link->key),
                 qUtf8Printable(link->reader.errorString()));
        detachSocket(link);
    }
}

void PlotServer::handleClientFrame(Link *link, PlotProtocol::MessageType type,
                                 const QByteArray &payload)
{
    if (!link->socket)
        return;

    switch (type) {
    case PlotProtocol::MessageType::Hello: {
        quint16 version = 0;
        if (!PlotProtocol::decodeHello(payload, &version))
            return;
        if (version != PlotProtocol::kVersion) {
            qWarning("Plot link %s version mismatch (client %u, server %u)",
                     qUtf8Printable(link->key), version, PlotProtocol::kVersion);
        }
        sendHandshake(link);
        break;
    }
    case PlotProtocol::MessageType::Resync: {
        qint64 fromMs = 0;
        if (!PlotProtocol::decodeResync(payload, &fromMs))
            return;
        sendResyncData(link, fromMs);
        break;
    }
    case PlotProtocol::MessageType::SeriesColor: {
        quint32 series = 0;
        QColor color;
        if (!PlotProtocol::decodeSeriesColor(payload, &series, &color))
            return;
        for (SeriesState &state : link->series) {
            if (state.id != series)
                continue;
            state.definition.color = color;
            emit seriesColorChanged(state.definition.key, color);
            m_endpointsDirty = true;
            break;
        }
        break;
    }
    case PlotProtocol::MessageType::Heartbeat:
    default:
        break;
    }
}

bool PlotServer::isSocketConnected(const Link *link)
{
    return link->socket && link->socket->state() == QLocalSocket::ConnectedState;
}

void PlotServer::send(Link *link, PlotProtocol::MessageType type, const QByteArray &payload)
{
    if (!isSocketConnected(link))
        return;
    link->socket->write(PlotProtocol::encodeFrame(type, payload));
}

void PlotServer::sendLinkMeta(Link *link)
{
    PlotProtocol::LinkMeta meta;
    meta.key = link->key;
    meta.title = link->title;
    meta.outDir = m_outputDir;
    send(link, PlotProtocol::MessageType::LinkMeta, PlotProtocol::encodeLinkMeta(meta));
}

// 补发热窗口里 timestamp >= fromMs 的点；热窗口起点晚于 fromMs 说明更早的数据已经丢了
void PlotServer::sendResyncData(Link *link, qint64 fromMs)
{
    QVector<PlotProtocol::Sample> recent;
    for (const PlotProtocol::Sample &sample : std::as_const(link->hot)) {
        if (sample.timestampMs >= fromMs)
            recent.append(sample);
    }
    const bool truncated = !link->hot.isEmpty() && link->hot.first().timestampMs > fromMs;
    send(link, PlotProtocol::MessageType::ResyncData,
         PlotProtocol::encodeResyncData(recent, truncated));
}

void PlotServer::sendHandshake(Link *link)
{
    send(link, PlotProtocol::MessageType::HelloAck,
         PlotProtocol::encodeHelloAck(PlotProtocol::kVersion, m_sessionStartMs));
    sendLinkMeta(link);

    for (const SeriesState &series : std::as_const(link->series))
        sendSeriesInfo(link, series);

    // 客户端重连：补发最近 60 s，让它立刻有内容可画
    if (!link->hot.isEmpty())
        sendResyncData(link, QDateTime::currentMSecsSinceEpoch() - kHotWindowMs);
}

void PlotServer::sendSeriesInfo(Link *link, const SeriesState &series)
{
    PlotProtocol::SeriesMeta meta;
    meta.series = series.id;
    meta.key = series.definition.key;
    meta.title = series.definition.title;
    meta.unit = series.definition.unit;
    meta.color = series.definition.color;
    meta.lastIndex = series.nextIndex - 1;
    send(link, PlotProtocol::MessageType::SeriesInfo, PlotProtocol::encodeSeriesInfo(meta));
}

void PlotServer::append(const QString &seriesKey, double value, qint64 timestampMs)
{
    int seriesIndex = -1;
    Link *link = findLinkBySeries(seriesKey, &seriesIndex);
    if (!link || seriesIndex < 0 || seriesIndex >= link->series.size())
        return;

    SeriesState &series = link->series[seriesIndex];

    PlotProtocol::Sample sample;
    sample.series = series.id;
    sample.index = series.nextIndex++;
    sample.timestampMs = timestampMs;
    sample.value = float(value);

    link->hot.append(sample);
    trimHot(link);

    if (!isSocketConnected(link))
        return; // 离线：只留在热窗口里，重连后由补发覆盖

    link->pending.append(sample);
    if (link->pending.size() > kMaxPendingSamples) {
        // 背压：管道长期跟不上时丢弃最旧的实时点
        link->pending.remove(0, kMaxPendingSamples / 10);
    }
}

void PlotServer::flushLink(Link *link)
{
    if (!isSocketConnected(link))
        return;
    if (link->socket->bytesToWrite() > kMaxBacklogBytes)
        return; // 客户端读不动：先攒着，别塞爆内核缓冲

    if (link->pending.isEmpty())
        return;

    const int total = link->pending.size();
    for (int offset = 0; offset < total; offset += kMaxBatchSamples) {
        const int count = qMin(kMaxBatchSamples, total - offset);
        QVector<PlotProtocol::Sample> batch;
        batch.reserve(count);
        for (int i = 0; i < count; ++i)
            batch.append(link->pending.at(offset + i));
        link->socket->write(PlotProtocol::encodeFrame(
                PlotProtocol::MessageType::Samples, PlotProtocol::encodeSamples(batch)));
    }
    link->pending.clear();
    link->socket->flush();
}

void PlotServer::trimHot(Link *link)
{
    const qint64 cutoff = link->hot.isEmpty() ? 0 : link->hot.last().timestampMs - kHotWindowMs;
    int drop = 0;
    while (drop < link->hot.size()
           && (link->hot.at(drop).timestampMs < cutoff || link->hot.size() - drop > kHotMaxSamples))
        ++drop;
    if (drop > 0)
        link->hot.remove(0, drop);
}

bool PlotServer::isAttached(const QString &groupKey) const
{
    const Link *link = findLink(groupKey);
    return link && isSocketConnected(link);
}

QString PlotServer::groupTitleOf(const QString &groupKey) const
{
    const Link *link = findLink(groupKey);
    return link ? link->title : QString();
}

QStringList PlotServer::groupKeys() const
{
    QStringList result;
    for (const Link *link : m_links)
        result.append(link->key);
    return result;
}

QPoint PlotServer::suggestPosition(int order) const
{
    return QPoint(140 + order * 32, 120 + order * 32);
}

void PlotServer::writeEndpointsFile()
{
    PlotEndpointFile file;
    file.sessionId = m_sessionId;
    file.sessionStartMs = m_sessionStartMs;
    file.pid = QCoreApplication::applicationPid();

    for (int i = 0; i < m_links.size(); ++i) {
        const Link *link = m_links.at(i);

        PlotGroup group;
        group.key = link->key;
        group.title = link->title;
        group.pipe = link->pipe;
        group.outDir = m_outputDir;
        group.position = suggestPosition(i);
        group.hasPosition = true;
        group.attached = isSocketConnected(link);
        for (const SeriesState &state : link->series) {
            PlotSeries series;
            series.id = state.id;
            series.key = state.definition.key;
            series.title = state.definition.title;
            series.unit = state.definition.unit;
            series.color = state.definition.color;
            group.series.append(series);
        }
        file.groups.append(group);
    }

    if (writePlotEndpoints(defaultEndpointsFilePath(), file))
        m_endpointsDirty = false;
}
