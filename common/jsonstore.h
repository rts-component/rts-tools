#ifndef JSONSTORE_H
#define JSONSTORE_H

#include <QJsonDocument>
#include <QString>

// 配置/清单类 JSON 文件的统一读写：
// 写入走 QSaveFile（先写临时文件再整体替换，避免写到一半崩溃把文件写坏），并自动创建目录。
namespace JsonStore {

// <数据目录>/settings/<fileName>
QString settingsPath(const QString &fileName);

// 文件不存在、打不开或不是合法 JSON 时返回空文档（isNull()）
QJsonDocument read(const QString &path);

// 失败时 qWarning(带 what 描述) 并返回 false
bool write(const QString &path, const QJsonDocument &document, const char *what);

} // namespace JsonStore

#endif // JSONSTORE_H
