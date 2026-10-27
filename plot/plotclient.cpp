#include "plotclient.h"

#include <QDateTime>
#include <QDebug>
#include <QLocalSocket>
#include <QTimer>

namespace {

constexpr int kHeartbeatIntervalMs = 2000;
constexpr int kStaleTimeoutMs = 15000;

} // namespace

PlotClient::PlotClient(QObject *parent)
    : QObject(parent)
    , m_endpointsPath(defaultEndpointsFilePath())
{
    m_retryTimer = new QTimer(this);
    m_retryTimer->setSingleShot(true);
    m_retryTimer->setInterval(m_retryIntervalMs);
    connect(m_retryTimer, &QTimer::timeout, this, [this] {
        if (m_groupKey.isEmpty() && m_pipe.isEmpty()) {
            setState(State::Idle);
            return;
        }
        m_pipe.clear(); // 每次重试都重新解析，主程序可能换了管道名
        if (!m_groupKey.isEmpty()) {
            PlotGroup group;
            if (!resolveEndpoint(m_groupKey, &group)) {
                scheduleRetry();
                return;
            }
            m_pipe = group.pipe;
        }
        openSocket(m_pipe);
    });

    m_heartbeatTimer = new QTimer(this);
    m_heartbeatTimer->setInterval(kHeartbeatIntervalMs);
    connect(m_heartbeatTimer, &QTimer::timeout, this, [this] {
        if (!m_socket || m_socket->state() != QLocalSocket::ConnectedState)
            return;
        if (m_lastReceivedMs > 0
            && QDateTime::currentMSecsSinceEpoch() - m_lastReceivedMs > kStaleTimeoutMs) {
            qWarning("plot server stopped responding");
            handleDisconnected();
            return;
        }
        m_socket->write(PlotProtocol::encodeFrame(PlotProtocol::MessageType::Heartbeat,
                                                  QByteArray()));
    });
    m_heartbeatTimer->start();

}

PlotClient::~PlotClient()
{
    m_autoReconnect = false;
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
    }
}

void PlotClient::setEndpointsFilePath(const QString &path)
{
    if (path.isEmpty())
        return;
    m_endpointsPath = path;
}

void PlotClient::setRetryInterval(int ms)
{
    m_retryIntervalMs = qMax(200, ms);
    m_retryTimer->setInterval(m_retryIntervalMs);
}

void PlotClient::connectToGroup(const QString &groupKey)
{
    m_groupKey = groupKey;
    m_pipe.clear();
    m_retryTimer->stop();
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }

    PlotGroup group;
    if (!resolveEndpoint(groupKey, &group)) {
        setState(State::Retrying,
                 tr("Waiting for rts-tools: no endpoint for \"%1\" in %2")
                         .arg(groupKey, m_endpointsPath));
        scheduleRetry();
        return;
    }
    m_pipe = group.pipe;
    m_hasSuggestedPosition = group.hasPosition;
    m_suggestedPosition = group.position;
    openSocket(m_pipe);
}

void PlotClient::connectToPipe(const QString &pipe, const QString &groupKey)
{
    m_groupKey = groupKey;
    m_pipe = pipe;
    m_retryTimer->stop();
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    openSocket(pipe);
}

void PlotClient::disconnectFromServer()
{
    m_groupKey.clear();
    m_pipe.clear();
    m_retryTimer->stop();
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    resetData();
    setState(State::Idle);
}

void PlotClient::openSocket(const QString &pipe)
{
    if (pipe.isEmpty()) {
        setState(State::Idle);
        return;
    }
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }

    m_reader.clear();
    m_peerVersion = 0;

    m_socket = new QLocalSocket(this);
    connect(m_socket, &QLocalSocket::connected, this, [this] {
        m_lastReceivedMs = QDateTime::currentMSecsSinceEpoch();
        m_socket->write(PlotProtocol::encodeFrame(PlotProtocol::MessageType::Hello,
                                                  PlotProtocol::encodeHello(
                                                          PlotProtocol::kVersion)));
    });
    connect(m_socket, &QLocalSocket::readyRead, this, &PlotClient::handleReadyRead);
    connect(m_socket, &QLocalSocket::disconnected, this, &PlotClient::handleDisconnected);
    connect(m_socket, &QLocalSocket::errorOccurred, this,
            [this](QLocalSocket::LocalSocketError error) {
                if (error == QLocalSocket::PeerClosedError)
                    return;
                if (m_state == State::Retrying)
                    return;
                handleDisconnected();
            });

    setState(State::Connecting, tr("Connecting to %1 ...").arg(pipe));
    m_socket->connectToServer(pipe);
}

void PlotClient::handleDisconnected()
{
    if (!m_socket)
        return;

    QLocalSocket *socket = m_socket;
    m_socket = nullptr;
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
    m_reader.clear();

    if (m_autoReconnect && (!m_groupKey.isEmpty() || !m_pipe.isEmpty())) {
        setState(State::Retrying, tr("Disconnected - retrying in %1 s ...")
                                          .arg(m_retryIntervalMs / 1000.0, 0, 'g', 2));
        scheduleRetry();
    } else {
        setState(State::Idle);
    }
}

void PlotClient::scheduleRetry()
{
    if (m_retryTimer->isActive())
        return;
    m_retryTimer->start();
}

void PlotClient::handleReadyRead()
{
    if (!m_socket)
        return;

    m_reader.append(m_socket->readAll());
    m_lastReceivedMs = QDateTime::currentMSecsSinceEpoch();

    PlotProtocol::MessageType type;
    QByteArray payload;
    while (m_reader.next(&type, &payload))
        handleFrame(type, payload);

    if (m_reader.hasError()) {
        qWarning("plot protocol error: %s", qUtf8Printable(m_reader.errorString()));
        handleDisconnected();
    }
}

void PlotClient::handleFrame(PlotProtocol::MessageType type, const QByteArray &payload)
{
    switch (type) {
    case PlotProtocol::MessageType::HelloAck: {
        quint16 version = 0;
        qint64 sessionStartMs = 0;
        if (!PlotProtocol::decodeHelloAck(payload, &version, &sessionStartMs))
            return;
        m_peerVersion = version;
        if (version != PlotProtocol::kVersion) {
            setState(State::Failed,
                     tr("Protocol version mismatch: rts-plot %1, rts-tools %2")
                             .arg(PlotProtocol::kVersion)
                             .arg(version));
            m_autoReconnect = false;
            if (m_socket)
                m_socket->abort();
            return;
        }
        if (m_sessionStartMs != 0 && m_sessionStartMs != sessionStartMs)
            resetData(); // 主程序换了会话，旧数据作废
        m_sessionStartMs = sessionStartMs;
        setState(State::Connected,
                 m_groupKey.isEmpty() ? m_pipe : tr("Connected - %1").arg(m_pipe));
        break;
    }
    case PlotProtocol::MessageType::LinkMeta: {
        PlotProtocol::LinkMeta meta;
        if (!PlotProtocol::decodeLinkMeta(payload, &meta))
            return;
        m_title = meta.title;
        if (!meta.key.isEmpty())
            m_groupKey = meta.key;
        emit metaChanged();
        break;
    }
    case PlotProtocol::MessageType::SeriesInfo: {
        PlotProtocol::SeriesMeta meta;
        if (!PlotProtocol::decodeSeriesInfo(payload, &meta))
            return;
        applySeriesMeta(meta);
        break;
    }
    case PlotProtocol::MessageType::SeriesRemoved: {
        quint32 series = 0;
        if (!PlotProtocol::decodeSeriesRemoved(payload, &series))
            return;
        removeSeries(series);
        break;
    }
    case PlotProtocol::MessageType::Samples: {
        QVector<PlotProtocol::Sample> samples;
        if (!PlotProtocol::decodeSamples(payload, &samples))
            return;
        appendSamples(samples);
        break;
    }
    case PlotProtocol::MessageType::ResyncData: {
        QVector<PlotProtocol::Sample> samples;
        bool truncated = false;
        if (!PlotProtocol::decodeResyncData(payload, &samples, &truncated))
            return;
        appendSamples(samples);
        break;
    }
    case PlotProtocol::MessageType::Hello:
    case PlotProtocol::MessageType::Resync:
    case PlotProtocol::MessageType::Heartbeat:
        break;
    }
}

void PlotClient::applySeriesMeta(const PlotProtocol::SeriesMeta &meta)
{
    SeriesData *existing = &ensureSeries(meta.series);
    existing->key = meta.key;
    existing->title = meta.title;
    existing->unit = meta.unit;
    existing->color = meta.color;
    // 离线期间改过的颜色：主程序还不知道，这里补发一次让它落盘
    const auto override_ = m_colorOverrides.constFind(meta.key);
    if (override_ != m_colorOverrides.constEnd() && *override_ != meta.color) {
        existing->color = *override_;
        writeSeriesColor(existing->id, *override_);
    }
    emit metaChanged();
}

void PlotClient::removeSeries(quint32 series)
{
    const int index = indexOfSeries(series);
    if (index < 0)
        return;
    m_table.removeColumn(series);
    m_series.removeAt(index);
    emit metaChanged();
    emit samplesAppended();
}

int PlotClient::indexOfSeries(quint32 id) const
{
    for (int i = 0; i < m_series.size(); ++i) {
        if (m_series.at(i).id == id)
            return i;
    }
    return -1;
}

PlotClient::SeriesData &PlotClient::ensureSeries(quint32 id)
{
    if (SeriesData *existing = findSeries(id))
        return *existing;
    SeriesData data;
    data.id = id;
    data.samples = SampleView(&m_table, m_table.addColumn(id));
    m_series.append(data);
    return m_series.last();
}

PlotClient::SeriesData *PlotClient::findSeries(quint32 id)
{
    const int index = indexOfSeries(id);
    return index < 0 ? nullptr : &m_series[index];
}

void PlotClient::appendSamples(const QVector<PlotProtocol::Sample> &samples)
{
    if (samples.isEmpty())
        return;

    const int limit = m_sampleLimit > 0 ? qMin(m_sampleLimit, kMaxSamplesPerSeries)
                                        : kMaxSamplesPerSeries;
    bool changed = false;
    for (const PlotProtocol::Sample &sample : samples) {
        const bool known = findSeries(sample.series) != nullptr;
        SeriesData &data = ensureSeries(sample.series);
        if (!known)
            emit metaChanged(); // 系列信息还没到（极少见），按 id 先建一个占位
        if (sample.index <= data.lastIndex)
            continue; // 补发重叠的点
        data.lastIndex = sample.index;

        // 同一次采样的各条曲线共用一行：index 比表里最新的大就开新行，否则找到已有的行
        int row = -1;
        if (m_table.isEmpty() || sample.index > m_table.indexAt(m_table.size() - 1))
            row = m_table.appendRow(sample.index, sample.timestampMs);
        else
            row = m_table.find(sample.index);
        if (row < 0)
            continue;
        m_table.setValue(data.id, row, sample.value);
        m_table.trimTo(limit);

        ++data.received;
        changed = true;
    }

    if (changed)
        emit samplesAppended();
}

void PlotClient::setSampleLimit(int limit)
{
    m_sampleLimit = qMax(0, limit);
    if (m_sampleLimit == 0)
        return;
    m_table.trimTo(m_sampleLimit);
}

void PlotClient::setSeriesColor(quint32 seriesId, const QColor &color)
{
    SeriesData *data = findSeries(seriesId);
    if (!data || !color.isValid())
        return;
    data->color = color;
    if (!data->key.isEmpty())
        m_colorOverrides.insert(data->key, color);
    writeSeriesColor(seriesId, color);
}

void PlotClient::writeSeriesColor(quint32 seriesId, const QColor &color)
{
    if (!m_socket || m_socket->state() != QLocalSocket::ConnectedState)
        return;
    m_socket->write(PlotProtocol::encodeFrame(PlotProtocol::MessageType::SeriesColor,
                                             PlotProtocol::encodeSeriesColor(seriesId, color)));
    m_socket->flush();
}

void PlotClient::clearAllSamples()
{
    // 保留各系列的 lastIndex：清空显示后，补发的旧点不会再被收进来
    m_table.clear();
    for (SeriesData &data : m_series)
        data.received = 0;
    emit samplesAppended();
}

void PlotClient::resetData()
{
    m_series.clear();
    m_table = SampleTable();
    m_sessionStartMs = 0;
    emit metaChanged();
    emit samplesAppended();
}

bool PlotClient::resolveEndpoint(const QString &groupKey, PlotGroup *group)
{
    const PlotEndpointFile file = readPlotEndpoints(m_endpointsPath);
    if (!file.valid)
        return false;
    for (const PlotGroup &candidate : file.groups) {
        if (candidate.key != groupKey && candidate.pipe != groupKey)
            continue;
        *group = candidate;
        return true;
    }
    return false;
}

void PlotClient::setState(State state, const QString &placeholder)
{
    m_state = state;
    if (placeholder.isEmpty()) {
        switch (state) {
        case State::Idle:
            m_placeholder = tr("No group selected");
            break;
        case State::Connected:
            m_placeholder = tr("Connected");
            break;
        default:
            m_placeholder.clear();
            break;
        }
    } else {
        m_placeholder = placeholder;
    }
    emit stateChanged();
}

QString PlotClient::stateText() const
{
    switch (m_state) {
    case State::Idle:
        return tr("Idle");
    case State::Resolving:
        return tr("Resolving");
    case State::Connecting:
        return tr("Connecting");
    case State::Connected:
        return tr("Connected");
    case State::Retrying:
        return tr("Retrying");
    case State::Failed:
        return tr("Failed");
    }
    return QString();
}

quint64 PlotClient::totalSamples() const
{
    quint64 total = 0;
    for (const SeriesData &data : m_series)
        total += data.received;
    return total;
}

bool PlotClient::isEmpty() const
{
    for (const SeriesData &data : m_series) {
        if (!data.samples.isEmpty())
            return false;
    }
    return true;
}
