#include "pack/packconfig.h"

#include "common/jsonstore.h"
#include "unpack/unpackconfig.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

QString configFilePath()
{
    return JsonStore::settingsPath(QStringLiteral("pack.json"));
}

} // namespace

QByteArray packEndingBytes(PackEnding ending)
{
    switch (ending) {
    case PackEnding::CrLf:
        return QByteArrayLiteral("\r\n");
    case PackEnding::Lf:
        return QByteArrayLiteral("\n");
    case PackEnding::Cr:
        return QByteArrayLiteral("\r");
    case PackEnding::None:
        break;
    }
    return {};
}

QVector<ProtoField> defaultPackLayout()
{
    ProtoField header;
    header.type = ProtoFieldType::Header;
    header.value = QByteArray::fromHex("AA55");
    ProtoField first;
    first.type = ProtoFieldType::Byte2;
    first.name = QStringLiteral("Data1");
    ProtoField second = first;
    second.name = QStringLiteral("Data2");
    ProtoField checksum;
    checksum.type = ProtoFieldType::Checksum;
    return {header, first, second, checksum};
}

PackConfig loadPackConfig()
{
    PackConfig config;
    const QJsonObject object = JsonStore::read(configFilePath()).object();

    for (const QJsonValue &value : object.value(QLatin1String("formats")).toArray()) {
        const QJsonObject item = value.toObject();
        PackFormat format;
        format.id = item.value(QLatin1String("id")).toString();
        if (format.id.isEmpty())
            format.id = newGroupId();
        format.name = item.value(QLatin1String("name")).toString();
        format.layout = protoLayoutFromJson(item.value(QLatin1String("layout")).toArray());
        config.formats.append(format);
    }

    for (const QJsonValue &value : object.value(QLatin1String("rows")).toArray()) {
        const QJsonObject item = value.toObject();
        PackRow row;
        row.checked = item.value(QLatin1String("checked")).toBool(false);
        row.name = item.value(QLatin1String("name")).toString();
        row.text = item.value(QLatin1String("text")).toString();
        row.hex = item.value(QLatin1String("hex")).toBool(false);
        row.delayMs = qBound(0, item.value(QLatin1String("delay")).toInt(50), 600000);
        row.formatId = item.value(QLatin1String("format")).toString();
        const int ending = item.value(QLatin1String("ending")).toInt(int(PackEnding::CrLf));
        row.ending = ending >= 0 && ending <= int(PackEnding::Cr) ? PackEnding(ending) : PackEnding::CrLf;
        config.rows.append(row);
    }
    return config;
}

void savePackConfig(const PackConfig &config)
{
    QJsonArray formats;
    for (const PackFormat &format : config.formats) {
        QJsonObject item;
        item.insert(QLatin1String("id"), format.id);
        item.insert(QLatin1String("name"), format.name);
        item.insert(QLatin1String("layout"), protoLayoutToJson(format.layout));
        formats.append(item);
    }

    QJsonArray rows;
    for (const PackRow &row : config.rows) {
        QJsonObject item;
        item.insert(QLatin1String("checked"), row.checked);
        item.insert(QLatin1String("name"), row.name);
        item.insert(QLatin1String("text"), row.text);
        item.insert(QLatin1String("hex"), row.hex);
        item.insert(QLatin1String("delay"), row.delayMs);
        item.insert(QLatin1String("format"), row.formatId);
        item.insert(QLatin1String("ending"), int(row.ending));
        rows.append(item);
    }

    QJsonObject object;
    object.insert(QLatin1String("formats"), formats);
    object.insert(QLatin1String("rows"), rows);
    JsonStore::write(configFilePath(), QJsonDocument(object), "Pack settings");
}
