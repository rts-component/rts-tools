#include "unpack/csvparser.h"

#include <QtGlobal>

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
