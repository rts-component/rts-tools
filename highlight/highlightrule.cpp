#include "highlight/highlightrule.h"

#include "common/jsonstore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

// 规则文件放在数据目录的 settings 下，便于随程序一起拷贝/查看
QString highlightRulesFilePath()
{
    return JsonStore::settingsPath(QStringLiteral("highlight_rules.json"));
}

} // namespace

void HighlightRule::compile()
{
    regex = QRegularExpression(
            pattern,
            matchCase ? QRegularExpression::NoPatternOption
                      : QRegularExpression::CaseInsensitiveOption);
}

bool HighlightRule::isValid() const
{
    return !pattern.isEmpty() && regex.isValid();
}

QVector<HighlightSpan> findHighlightSpans(const QString &text, const QVector<HighlightRule> &rules)
{
    QVector<HighlightSpan> spans;
    if (text.isEmpty() || rules.isEmpty())
        return spans;

    // -1 表示未命中；否则存规则下标（下标越小优先级越高）
    QVector<int> owner(text.size(), -1);
    for (int index = 0; index < rules.size(); ++index) {
        const HighlightRule &rule = rules.at(index);
        if (!rule.enabled || !rule.isValid() || !rule.color.isValid())
            continue;

        QRegularExpressionMatchIterator it = rule.regex.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            if (match.capturedLength() <= 0)
                continue;
            const int end = match.capturedEnd();
            for (int i = match.capturedStart(); i < end; ++i) {
                if (owner.at(i) < 0)
                    owner[i] = index;
            }
        }
    }

    for (int i = 0; i < owner.size(); ++i) {
        const int index = owner.at(i);
        if (index < 0)
            continue;
        int end = i + 1;
        while (end < owner.size() && owner.at(end) == index)
            ++end;
        spans.append({i, end - i, rules.at(index).color});
        i = end - 1;
    }
    return spans;
}

QVector<HighlightRule> loadHighlightRules()
{
    QVector<HighlightRule> rules;

    const QJsonArray array = JsonStore::read(highlightRulesFilePath()).array();
    rules.reserve(array.size());
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        HighlightRule rule;
        rule.pattern = object.value(QLatin1String("pattern")).toString();
        rule.color = QColor::fromString(object.value(QLatin1String("color")).toString());
        rule.matchCase = object.value(QLatin1String("matchCase")).toBool();
        rule.enabled = object.value(QLatin1String("enabled")).toBool(true);
        rule.compile();
        if (rule.isValid() && rule.color.isValid())
            rules.append(rule);
    }
    return rules;
}

void saveHighlightRules(const QVector<HighlightRule> &rules)
{
    QJsonArray array;
    for (const HighlightRule &rule : rules) {
        if (rule.pattern.isEmpty())
            continue;
        QJsonObject object;
        object.insert(QLatin1String("pattern"), rule.pattern);
        object.insert(QLatin1String("color"), rule.color.name());
        object.insert(QLatin1String("matchCase"), rule.matchCase);
        object.insert(QLatin1String("enabled"), rule.enabled);
        array.append(object);
    }

    JsonStore::write(highlightRulesFilePath(), QJsonDocument(array), "highlight rules");
}
