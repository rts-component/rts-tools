#include "common/plotendpoints.h"

#include "common/jsonstore.h"
#include "common/plotpaths.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

namespace {

// 清单文件格式版本（仅供人工排查，读取端不校验；协议版本见 PlotProtocol::kVersion）
constexpr int kFileFormatVersion = 2;

// JSON 字段名：读写共用，避免两端手抄不一致
const QString kVersion = QStringLiteral("version");
const QString kRole = QStringLiteral("role");
const QString kSessionId = QStringLiteral("sessionId");
const QString kSessionStartMs = QStringLiteral("sessionStartMs");
const QString kPid = QStringLiteral("pid");
const QString kUpdatedAtMs = QStringLiteral("updatedAtMs");
const QString kGroups = QStringLiteral("groups");
const QString kKey = QStringLiteral("key");
const QString kTitle = QStringLiteral("title");
const QString kPipe = QStringLiteral("pipe");
const QString kPos = QStringLiteral("pos");
const QString kOut = QStringLiteral("out");
const QString kAttached = QStringLiteral("attached");
const QString kSeries = QStringLiteral("series");
const QString kId = QStringLiteral("id");
const QString kUnit = QStringLiteral("unit");
const QString kColor = QStringLiteral("color");

} // namespace

QString defaultEndpointsFilePath()
{
    return appDataDir() + QStringLiteral("/run/plot-endpoints.json");
}

PlotEndpointFile readPlotEndpoints(const QString &path)
{
    PlotEndpointFile result;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("cannot open %1").arg(path);
        return result;
    }

    QJsonParseError parseError{};
    const QJsonObject root = QJsonDocument::fromJson(file.readAll(), &parseError).object();
    if (parseError.error != QJsonParseError::NoError) {
        result.error = parseError.errorString();
        return result;
    }

    result.sessionId = root.value(kSessionId).toString();
    result.sessionStartMs = qint64(root.value(kSessionStartMs).toDouble());
    result.pid = qint64(root.value(kPid).toDouble());

    const QJsonArray groups = root.value(kGroups).toArray();
    for (const QJsonValue &value : groups) {
        const QJsonObject object = value.toObject();

        PlotGroup group;
        group.key = object.value(kKey).toString();
        group.title = object.value(kTitle).toString();
        group.pipe = object.value(kPipe).toString();
        group.outDir = object.value(kOut).toString();
        group.attached = object.value(kAttached).toBool();

        const QStringList parts = object.value(kPos).toString().split(QLatin1Char(','));
        if (parts.size() == 2) {
            bool okX = false;
            bool okY = false;
            const int x = parts.at(0).trimmed().toInt(&okX);
            const int y = parts.at(1).trimmed().toInt(&okY);
            if (okX && okY) {
                group.position = QPoint(x, y);
                group.hasPosition = true;
            }
        }

        const QJsonArray series = object.value(kSeries).toArray();
        for (const QJsonValue &seriesValue : series) {
            const QJsonObject seriesObject = seriesValue.toObject();
            PlotSeries item;
            item.id = quint32(seriesObject.value(kId).toInt());
            item.key = seriesObject.value(kKey).toString();
            item.title = seriesObject.value(kTitle).toString();
            item.unit = seriesObject.value(kUnit).toString();
            item.color = QColor(seriesObject.value(kColor).toString());
            group.series.append(item);
        }

        if (group.pipe.isEmpty())
            continue;
        result.groups.append(group);
    }

    result.valid = true;
    return result;
}

bool writePlotEndpoints(const QString &path, const PlotEndpointFile &file)
{
    QJsonArray groups;
    for (const PlotGroup &group : file.groups) {
        QJsonArray series;
        for (const PlotSeries &item : group.series) {
            QJsonObject entry;
            entry.insert(kId, int(item.id));
            entry.insert(kKey, item.key);
            entry.insert(kTitle, item.title);
            entry.insert(kUnit, item.unit);
            entry.insert(kColor, item.color.name());
            series.append(entry);
        }

        QJsonObject entry;
        entry.insert(kKey, group.key);
        entry.insert(kTitle, group.title);
        entry.insert(kPipe, group.pipe);
        if (group.hasPosition) {
            entry.insert(kPos, QStringLiteral("%1,%2").arg(group.position.x()).arg(group.position.y()));
        }
        entry.insert(kOut, group.outDir);
        entry.insert(kAttached, group.attached);
        entry.insert(kSeries, series);
        groups.append(entry);
    }

    QJsonObject root;
    root.insert(kVersion, kFileFormatVersion);
    root.insert(kRole, QStringLiteral("rts-tools"));
    root.insert(kSessionId, file.sessionId);
    root.insert(kSessionStartMs, file.sessionStartMs);
    root.insert(kPid, file.pid);
    root.insert(kUpdatedAtMs, QDateTime::currentMSecsSinceEpoch());
    root.insert(kGroups, groups);

    return JsonStore::write(path, QJsonDocument(root), "plot endpoints");
}
