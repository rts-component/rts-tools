#include "unpack/unpackconfig.h"

#include "common/jsonstore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

namespace {

// 协议字段类型 / 转换 / 校验算法落盘用稳定的字符串，不依赖枚举数值
struct NamedType { const char *name; int value; };

const NamedType kFieldTypes[] = {
    {"header", int(ProtoFieldType::Header)}, {"seq", int(ProtoFieldType::Seq)},
    {"id", int(ProtoFieldType::Id)},         {"length", int(ProtoFieldType::Length)},
    {"byte1", int(ProtoFieldType::Byte1)},   {"byte2", int(ProtoFieldType::Byte2)},
    {"byte3", int(ProtoFieldType::Byte3)},   {"byte4", int(ProtoFieldType::Byte4)},
    {"byte8", int(ProtoFieldType::Byte8)},   {"data", int(ProtoFieldType::Data)},
    {"checksum", int(ProtoFieldType::Checksum)},
    {"tail", int(ProtoFieldType::Tail)},
};
const NamedType kConversions[] = {
    {"uint", int(ProtoConversion::UInt)}, {"int", int(ProtoConversion::Int)},
    {"float", int(ProtoConversion::Float)}, {"hex", int(ProtoConversion::Hex)}, {"bin", int(ProtoConversion::Binary)},
    {"text", int(ProtoConversion::Text)},
};
const NamedType kChecksums[] = {
    {"sum8", int(ProtoChecksum::Sum8)},         {"xor8", int(ProtoChecksum::Xor8)},
    {"crc8", int(ProtoChecksum::Crc8)},         {"sum16", int(ProtoChecksum::Sum16)},
    {"crc16modbus", int(ProtoChecksum::Crc16Modbus)},
    {"crc16ccitt", int(ProtoChecksum::Crc16Ccitt)},
};

template <size_t N>
QString nameOf(const NamedType (&table)[N], int value)
{
    for (const NamedType &entry : table) {
        if (entry.value == value)
            return QLatin1String(entry.name);
    }
    return QString();
}

template <size_t N>
bool valueOf(const NamedType (&table)[N], const QString &name, int *value)
{
    for (const NamedType &entry : table) {
        if (name == QLatin1String(entry.name)) {
            *value = entry.value;
            return true;
        }
    }
    return false;
}

QJsonObject protoFieldToJson(const ProtoField &field)
{
    QJsonObject object;
    object.insert(QLatin1String("type"), nameOf(kFieldTypes, int(field.type)));
    object.insert(QLatin1String("name"), field.name);
    object.insert(QLatin1String("value"), formatHexBytes(field.value));
    object.insert(QLatin1String("conversion"), nameOf(kConversions, int(field.conversion)));
    object.insert(QLatin1String("bigEndian"), field.bigEndian);
    object.insert(QLatin1String("show"), field.show);
    object.insert(QLatin1String("rangeFrom"), field.rangeFrom);
    object.insert(QLatin1String("rangeTo"), field.rangeTo);
    object.insert(QLatin1String("checksum"), nameOf(kChecksums, int(field.checksum)));
    object.insert(QLatin1String("invertChecksum"), field.invertChecksum);
    if (!field.bitGroups.isEmpty()) {
        QJsonArray groups;
        for (const ProtoBitGroup &group : field.bitGroups) {
            QJsonObject item;
            item.insert(QLatin1String("name"), group.name);
            item.insert(QLatin1String("low"), group.low);
            item.insert(QLatin1String("high"), group.high);
            QJsonObject names;
            for (auto it = group.valueNames.cbegin(); it != group.valueNames.cend(); ++it)
                names.insert(QString::number(it.key()), it.value());
            item.insert(QLatin1String("values"), names);
            groups.append(item);
        }
        object.insert(QLatin1String("bitGroups"), groups);
    }
    return object;
}

bool protoFieldFromJson(const QJsonObject &object, ProtoField *field)
{
    int value = 0;
    if (!valueOf(kFieldTypes, object.value(QLatin1String("type")).toString(), &value))
        return false;
    field->type = ProtoFieldType(value);
    field->name = object.value(QLatin1String("name")).toString();
    parseHexBytes(object.value(QLatin1String("value")).toString(), &field->value);
    if (valueOf(kConversions, object.value(QLatin1String("conversion")).toString(), &value))
        field->conversion = ProtoConversion(value);
    field->bigEndian = object.value(QLatin1String("bigEndian")).toBool(false);
    field->show = object.value(QLatin1String("show")).toBool(true);
    // 旧版本的 lengthOffset / checksumFromHeader 无法对应到字段范围，忽略（回到自动范围）
    field->rangeFrom = object.value(QLatin1String("rangeFrom")).toInt(-1);
    field->rangeTo = object.value(QLatin1String("rangeTo")).toInt(-1);
    if (valueOf(kChecksums, object.value(QLatin1String("checksum")).toString(), &value))
        field->checksum = ProtoChecksum(value);
    field->bitGroups.clear();
    const QJsonArray groups = object.value(QLatin1String("bitGroups")).toArray();
    for (const QJsonValue &entry : groups) {
        const QJsonObject item = entry.toObject();
        ProtoBitGroup group;
        group.name = item.value(QLatin1String("name")).toString();
        group.low = item.value(QLatin1String("low")).toInt();
        group.high = item.value(QLatin1String("high")).toInt();
        if (group.low < 0 || group.high < group.low || group.high > 63)
            continue;
        const QJsonObject names = item.value(QLatin1String("values")).toObject();
        for (auto it = names.begin(); it != names.end(); ++it)
            group.valueNames.insert(it.key().toInt(), it.value().toString());
        field->bitGroups.append(group);
    }
    field->invertChecksum = object.value(QLatin1String("invertChecksum")).toBool(false);
    return true;
}

QString configFilePath()
{
    return JsonStore::settingsPath(QStringLiteral("unpack.json"));
}

// 旧版本（面板还叫 CSV 时）的配置文件名；新文件不存在时读它，下次保存即迁移
QString legacyConfigFilePath()
{
    return JsonStore::settingsPath(QStringLiteral("csv_parse.json"));
}

} // namespace

QString newGroupId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}





QJsonArray protoLayoutToJson(const QVector<ProtoField> &layout)
{
    QJsonArray array;
    for (const ProtoField &field : layout)
        array.append(protoFieldToJson(field));
    return array;
}

QVector<ProtoField> protoLayoutFromJson(const QJsonArray &array)
{
    QVector<ProtoField> layout;
    for (const QJsonValue &value : array) {
        ProtoField field;
        if (protoFieldFromJson(value.toObject(), &field))
            layout.append(field);
    }
    return layout;
}

UnpackConfig loadUnpackConfig()
{
    UnpackConfig config;

    QJsonObject object = JsonStore::read(configFilePath()).object();
    if (object.isEmpty())
        object = JsonStore::read(legacyConfigFilePath()).object();
    // 顶层 enabled（Start parsing）与行级 curve（Curve 勾选）属于运行期状态，不恢复：
    // 重启后一律从不勾选开始，避免自动拉起图表窗口 / 自动开始解析。

    const QJsonArray groups = object.value(QLatin1String("groups")).toArray();
    for (const QJsonValue &groupValue : groups) {
        const QJsonObject groupObject = groupValue.toObject();

        UnpackGroup group;
        group.id = groupObject.value(QLatin1String("id")).toString();
        group.enabled = groupObject.value(QLatin1String("enabled")).toBool(true);
        group.name = groupObject.value(QLatin1String("name")).toString();
        const QString separator = groupObject.value(QLatin1String("separator")).toString();
        if (!separator.isEmpty())
            group.separator = separator;

        group.mode = groupObject.value(QLatin1String("mode")).toString() == QLatin1String("protocol")
                ? UnpackMode::Protocol
                : UnpackMode::Csv;
        group.layout = protoLayoutFromJson(groupObject.value(QLatin1String("layout")).toArray());

        const QJsonArray fields = groupObject.value(QLatin1String("fields")).toArray();
        for (const QJsonValue &fieldValue : fields) {
            const QJsonObject fieldObject = fieldValue.toObject();
            UnpackField field;
            field.name = fieldObject.value(QLatin1String("name")).toString();
            field.formula = fieldObject.value(QLatin1String("formula")).toString();
            field.color = fieldObject.value(QLatin1String("color")).toString();
            field.numeric = fieldObject.value(QLatin1String("numeric")).toBool(true);
            field.curve = fieldObject.value(QLatin1String("curve")).toBool(false);
            if (!field.numeric)
                field.curve = false; // 非数字字段没有曲线
            group.fields.append(field);
        }

        config.groups.append(group);
    }

    return config;
}

void saveUnpackConfig(const UnpackConfig &config)
{
    QJsonArray groups;
    for (const UnpackGroup &group : config.groups) {
        QJsonArray fields;
        for (const UnpackField &field : group.fields) {
            QJsonObject fieldObject;
            fieldObject.insert(QLatin1String("name"), field.name);
            fieldObject.insert(QLatin1String("formula"), field.formula);
            fieldObject.insert(QLatin1String("color"), field.color);
            fieldObject.insert(QLatin1String("numeric"), field.numeric);
            fieldObject.insert(QLatin1String("curve"), field.numeric && field.curve);
            fields.append(fieldObject);
        }

        QJsonObject groupObject;
        groupObject.insert(QLatin1String("id"), group.id);
        groupObject.insert(QLatin1String("enabled"), group.enabled);
        groupObject.insert(QLatin1String("name"), group.name);
        groupObject.insert(QLatin1String("separator"), group.separator);
        groupObject.insert(QLatin1String("mode"),
                           group.isProtocol() ? QLatin1String("protocol") : QLatin1String("csv"));
        groupObject.insert(QLatin1String("layout"), protoLayoutToJson(group.layout));
        // 行级 Curve 勾选是运行期状态，不落盘（重启后不会自己弹图表窗口）
        groupObject.insert(QLatin1String("fields"), fields);
        groups.append(groupObject);
    }

    QJsonObject object;
    // 顶层 enabled（Start parsing）同样不落盘
    object.insert(QLatin1String("groups"), groups);

    JsonStore::write(configFilePath(), QJsonDocument(object), "Unpack settings");
}

QVector<UnpackField> protocolFields(const QVector<ProtoField> &layout, const QVector<UnpackField> &oldFields)
{
    QVector<UnpackField> fields;
    const QVector<int> indexes = protocolDisplayIndexes(layout);
    const QStringList names = protocolDisplayNames(layout);
    for (int i = 0; i < indexes.size(); ++i) {
        UnpackField field;
        if (i < oldFields.size()) {
            field.color = oldFields.at(i).color;
            field.formula = oldFields.at(i).formula;
            field.curve = oldFields.at(i).curve;
        }
        field.name = names.at(i);
        field.numeric = protoFieldNumeric(layout.at(indexes.at(i)));
        if (!field.numeric)
            field.curve = false;
        fields.append(field);
    }
    return fields;
}
