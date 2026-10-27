#include "plotpaths.h"
#include "plotendpoints.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

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

    result.sessionId = root.value(QStringLiteral("sessionId")).toString();
    result.sessionStartMs = qint64(root.value(QStringLiteral("sessionStartMs")).toDouble());
    result.pid = qint64(root.value(QStringLiteral("pid")).toDouble());

    const QJsonArray groups = root.value(QStringLiteral("groups")).toArray();
    for (const QJsonValue &value : groups) {
        const QJsonObject object = value.toObject();

        PlotGroup group;
        group.key = object.value(QStringLiteral("key")).toString();
        group.title = object.value(QStringLiteral("title")).toString();
        group.pipe = object.value(QStringLiteral("pipe")).toString();
        group.outDir = object.value(QStringLiteral("out")).toString();
        group.attached = object.value(QStringLiteral("attached")).toBool();

        const QStringList parts =
                object.value(QStringLiteral("pos")).toString().split(QLatin1Char(','));
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

        const QJsonArray series = object.value(QStringLiteral("series")).toArray();
        for (const QJsonValue &seriesValue : series) {
            const QJsonObject seriesObject = seriesValue.toObject();
            PlotSeries item;
            item.id = quint32(seriesObject.value(QStringLiteral("id")).toInt());
            item.key = seriesObject.value(QStringLiteral("key")).toString();
            item.title = seriesObject.value(QStringLiteral("title")).toString();
            item.unit = seriesObject.value(QStringLiteral("unit")).toString();
            item.color = QColor(seriesObject.value(QStringLiteral("color")).toString());
            group.series.append(item);
        }

        if (group.pipe.isEmpty())
            continue;
        result.groups.append(group);
    }

    result.valid = true;
    return result;
}
