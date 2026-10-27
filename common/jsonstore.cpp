#include "common/jsonstore.h"

#include "common/plotpaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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

} // namespace JsonStore
