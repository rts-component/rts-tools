#include "common/jsonstore.h"

#include "common/plotpaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>

namespace JsonStore {

QString settingsPath(const QString &fileName)
{
    return appDataDir() + QStringLiteral("/settings/") + fileName;
}

QJsonDocument read(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll());
}

bool write(const QString &path, const QJsonDocument &document, const char *what)
{
    QDir().mkpath(QFileInfo(path).absolutePath());

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning("Cannot write %s to %s: %s", what, qUtf8Printable(path),
                 qUtf8Printable(file.errorString()));
        return false;
    }
    file.write(document.toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        qWarning("Cannot save %s to %s: %s", what, qUtf8Printable(path),
                 qUtf8Printable(file.errorString()));
        return false;
    }
    return true;
}

namespace {

constexpr const char *kBundleFormat = "rts-tools-settings";

bool allowedName(const QString &name)
{
    return !name.isEmpty() && QFileInfo(name).fileName() == name && !name.startsWith(QLatin1Char('.'))
            && (name.endsWith(QLatin1String(".json")) || name.endsWith(QLatin1String(".csv")));
}

} // namespace

bool exportBundle(const QString &path, QString *error)
{
    QJsonObject files;
    const QDir dir(settingsPath(QString()));
    const QFileInfoList entries = dir.entryInfoList(QDir::Files, QDir::Name);
    for (const QFileInfo &info : entries) {
        if (!allowedName(info.fileName()))
            continue;
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QByteArray data = file.readAll();
        // JSON 文件以对象嵌入，导出文件才是格式化可读的；其余(csv)保持字符串
        const QJsonDocument doc = QJsonDocument::fromJson(data);
        if (info.fileName().endsWith(QLatin1String(".json")) && !doc.isNull())
            files.insert(info.fileName(), doc.isObject() ? QJsonValue(doc.object()) : QJsonValue(doc.array()));
        else
            files.insert(info.fileName(), QString::fromUtf8(data));
    }

    QJsonObject root;
    root.insert(QLatin1String("format"), QLatin1String(kBundleFormat));
    root.insert(QLatin1String("version"), 1);
    root.insert(QLatin1String("files"), files);

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error)
            *error = out.errorString();
        return false;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!out.commit()) {
        if (error)
            *error = out.errorString();
        return false;
    }
    return true;
}

int importBundle(const QString &path, QString *error)
{
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) {
        if (error)
            *error = in.errorString();
        return -1;
    }
    const QJsonObject root = QJsonDocument::fromJson(in.readAll()).object();
    if (root.value(QLatin1String("format")).toString() != QLatin1String(kBundleFormat)) {
        if (error)
            *error = QCoreApplication::translate("JsonStore", "Not an rts-tools settings file.");
        return -1;
    }

    // 先整体校验，再写入，避免导入一半
    const QJsonObject files = root.value(QLatin1String("files")).toObject();
    for (auto it = files.begin(); it != files.end(); ++it) {
        const bool isJson = it.key().endsWith(QLatin1String(".json"));
        const bool valid = allowedName(it.key())
                && (it.value().isString() ? (!isJson || !QJsonDocument::fromJson(it.value().toString().toUtf8()).isNull())
                                          : (isJson && (it.value().isObject() || it.value().isArray())));
        if (!valid) {
            if (error)
                *error = QCoreApplication::translate("JsonStore", "Invalid entry: %1").arg(it.key());
            return -1;
        }
    }

    int count = 0;
    QDir().mkpath(settingsPath(QString()));
    for (auto it = files.begin(); it != files.end(); ++it) {
        QSaveFile out(settingsPath(it.key()));
        if (!out.open(QIODevice::WriteOnly)) {
            if (error)
                *error = out.errorString();
            return -1;
        }
        if (it.value().isString())
            out.write(it.value().toString().toUtf8());
        else if (it.value().isObject())
            out.write(QJsonDocument(it.value().toObject()).toJson(QJsonDocument::Indented));
        else
            out.write(QJsonDocument(it.value().toArray()).toJson(QJsonDocument::Indented));
        if (!out.commit()) {
            if (error)
                *error = out.errorString();
            return -1;
        }
        ++count;
    }
    return count;
}

} // namespace JsonStore
