#include "csv/csvparser.h"

#include "common/jsonstore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

namespace {

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
        // 行级 Curve 勾选是运行期状态，不落盘（重启后不会自己弹图表窗口）
        groupObject.insert(QLatin1String("fields"), fields);
        groups.append(groupObject);
    }

    QJsonObject object;
    // 顶层 enabled（Start parsing）同样不落盘
    object.insert(QLatin1String("groups"), groups);

    JsonStore::write(configFilePath(), QJsonDocument(object), "CSV parse settings");
}
