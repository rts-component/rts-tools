#include "common/translation.h"

#include "common/jsonstore.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QTranslator>

namespace {

const char kLanguageKey[] = "language";

QString languageFilePath()
{
    return JsonStore::settingsPath(QStringLiteral("language.json"));
}

// 翻译对象要比应用活得久：切换语言时复用同一个对象，只换它加载的 .qm
QTranslator *uiTranslator()
{
    static QTranslator *translator = nullptr;
    if (!translator)
        translator = new QTranslator(QCoreApplication::instance());
    return translator;
}

} // namespace

namespace UiLanguage {

QStringList available()
{
    return {QString::fromLatin1(kSystem), QString::fromLatin1(kChinese),
            QString::fromLatin1(kEnglish)};
}

QString saved()
{
    const QString value = JsonStore::read(languageFilePath())
                                  .object()
                                  .value(QLatin1String(kLanguageKey))
                                  .toString();
    return available().contains(value) ? value : QString::fromLatin1(kSystem);
}

void save(const QString &language)
{
    const QString value =
            available().contains(language) ? language : QString::fromLatin1(kSystem);

    QJsonObject object = JsonStore::read(languageFilePath()).object();
    object.insert(QLatin1String(kLanguageKey), value);
    JsonStore::write(languageFilePath(), QJsonDocument(object), "language setting");
}

bool apply(const QString &baseName)
{
    QTranslator *translator = uiTranslator();
    if (!translator)
        return false;

    // 先摘掉旧翻译，再按新的选择加载；找不到翻译文件就是源语言（英文）
    QCoreApplication::removeTranslator(translator);

    const QString preference = saved();
    const QStringList locales = preference == QLatin1String(kSystem)
            ? QLocale::system().uiLanguages()
            : QStringList{preference};

    for (const QString &locale : locales) {
        const QString resource =
                QStringLiteral(":/i18n/%1_%2").arg(baseName, QLocale(locale).name());
        if (translator->load(resource)) {
            QCoreApplication::installTranslator(translator);
            return true;
        }
    }
    return false;
}

} // namespace UiLanguage
