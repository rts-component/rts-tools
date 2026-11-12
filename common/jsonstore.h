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

// 把 settings 目录下的全部配置（*.json / *.csv）打成一个文件，便于备份或拷到别的机器
bool exportBundle(const QString &path, QString *error);
// 读取 exportBundle 生成的文件并覆盖对应的配置文件；返回导入的文件数，失败返回 -1。
// 只接受不带路径的 .json / .csv 文件名，JSON 内容必须合法。
int importBundle(const QString &path, QString *error);

} // namespace JsonStore

#endif // JSONSTORE_H
