#include "csv/csvparser.h"

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
    {"byte8", int(ProtoFieldType::Byte8)},   {"checksum", int(ProtoFieldType::Checksum)},
    {"tail", int(ProtoFieldType::Tail)},
};
const NamedType kConversions[] = {
    {"uint", int(ProtoConversion::UInt)}, {"int", int(ProtoConversion::Int)},
    {"float", int(ProtoConversion::Float)}, {"hex", int(ProtoConversion::Hex)},
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
    object.insert(QLatin1String("lengthOffset"), field.lengthOffset);
    object.insert(QLatin1String("checksum"), nameOf(kChecksums, int(field.checksum)));
    object.insert(QLatin1String("checksumFromHeader"), field.checksumFromHeader);
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
    field->lengthOffset = object.value(QLatin1String("lengthOffset")).toInt(0);
    if (valueOf(kChecksums, object.value(QLatin1String("checksum")).toString(), &value))
        field->checksum = ProtoChecksum(value);
    field->checksumFromHeader = object.value(QLatin1String("checksumFromHeader")).toBool(true);
    return true;
}

QString configFilePath()
{
    return JsonStore::settingsPath(QStringLiteral("csv_parse.json"));
}

} // namespace

QString newGroupId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString decodeSeparator(const QString &text)
{
    if (text == QLatin1String("\\t"))
        return QStringLiteral("\t");
    if (text == QLatin1String("\\n"))
        return QStringLiteral("\n");
    return text;
}

QStringList splitCsvLine(const QString &line, const QString &separator)
{
    QStringList fields;
    if (separator.isEmpty()) {
        fields.append(line);
        return fields;
    }

    QString field;
    bool inQuotes = false;
    int index = 0;
    while (index < line.size()) {
        const QChar ch = line.at(index);

        if (inQuotes) {
            if (ch == QLatin1Char('"')) {
                if (index + 1 < line.size() && line.at(index + 1) == QLatin1Char('"')) {
                    field.append(QLatin1Char('"')); // "" 转义成一个引号
                    index += 2;
                    continue;
                }
                inQuotes = false;
                ++index;
                continue;
            }
            field.append(ch);
            ++index;
            continue;
        }

        if (ch == QLatin1Char('"') && field.trimmed().isEmpty()) {
            field.clear();
            inQuotes = true;
            ++index;
            continue;
        }

        if (line.mid(index, separator.size()) == separator) {
            fields.append(field.trimmed());
            field.clear();
            index += separator.size();
            continue;
        }

        field.append(ch);
        ++index;
    }
    fields.append(field.trimmed());
    return fields;
}

bool looksLikeCsvRecord(const QStringList &fields)
{
    if (fields.size() < 2)
        return false;
    for (const QString &field : fields) {
        if (!field.isEmpty())
            return true;
    }
    return false;
}

bool isNumericValue(const QString &text)
{
    if (text.isEmpty())
        return false;

    bool ok = false;
    const double value = text.toDouble(&ok);
    return ok && qIsFinite(value); // 同时排除 "nan" / "inf"
}

CsvParseConfig loadCsvParseConfig()
{
    CsvParseConfig config;

    const QJsonObject object = JsonStore::read(configFilePath()).object();
    // 顶层 enabled（Start parsing）与行级 curve（Curve 勾选）属于运行期状态，不恢复：
    // 重启后一律从不勾选开始，避免自动拉起图表窗口 / 自动开始解析。

    const QJsonArray groups = object.value(QLatin1String("groups")).toArray();
    for (const QJsonValue &groupValue : groups) {
        const QJsonObject groupObject = groupValue.toObject();

        CsvGroup group;
        group.id = groupObject.value(QLatin1String("id")).toString();
        group.enabled = groupObject.value(QLatin1String("enabled")).toBool(true);
        group.name = groupObject.value(QLatin1String("name")).toString();
        const QString separator = groupObject.value(QLatin1String("separator")).toString();
        if (!separator.isEmpty())
            group.separator = separator;

        group.protocol = groupObject.value(QLatin1String("protocol")).toBool(false);
        const QJsonArray layout = groupObject.value(QLatin1String("layout")).toArray();
        for (const QJsonValue &layoutValue : layout) {
            ProtoField protoField;
            if (protoFieldFromJson(layoutValue.toObject(), &protoField))
                group.layout.append(protoField);
        }

        const QJsonArray fields = groupObject.value(QLatin1String("fields")).toArray();
        for (const QJsonValue &fieldValue : fields) {
            const QJsonObject fieldObject = fieldValue.toObject();
            CsvField field;
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

void saveCsvParseConfig(const CsvParseConfig &config)
{
    QJsonArray groups;
    for (const CsvGroup &group : config.groups) {
        QJsonArray fields;
        for (const CsvField &field : group.fields) {
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
        groupObject.insert(QLatin1String("protocol"), group.protocol);
        QJsonArray layout;
        for (const ProtoField &protoField : group.layout)
            layout.append(protoFieldToJson(protoField));
        groupObject.insert(QLatin1String("layout"), layout);
        // 行级 Curve 勾选是运行期状态，不落盘（重启后不会自己弹图表窗口）
        groupObject.insert(QLatin1String("fields"), fields);
        groups.append(groupObject);
    }

    QJsonObject object;
    // 顶层 enabled（Start parsing）同样不落盘
    object.insert(QLatin1String("groups"), groups);

    JsonStore::write(configFilePath(), QJsonDocument(object), "CSV parse settings");
}

QVector<CsvField> protocolFields(const QVector<ProtoField> &layout, const QVector<CsvField> &oldFields)
{
    QVector<CsvField> fields;
    const QVector<int> indexes = protocolDisplayIndexes(layout);
    const QStringList names = protocolDisplayNames(layout);
    for (int i = 0; i < indexes.size(); ++i) {
        CsvField field;
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
