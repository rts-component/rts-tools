#include "common/jlinkpaths.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>

namespace JLinkPaths {

QString findJLinkTool(const QString &name)
{
    QString path = QStandardPaths::findExecutable(name);
    if (!path.isEmpty())
        return path;

#ifdef Q_OS_WIN
    const QSettings::Format registryFormats[] = {
        QSettings::Registry64Format,
        QSettings::Registry32Format,
    };
    const QSettings::Scope registryScopes[] = {
        QSettings::UserScope,
        QSettings::SystemScope,
    };
    for (QSettings::Format format : registryFormats) {
        for (QSettings::Scope scope : registryScopes) {
            const QSettings settings(format, scope, QStringLiteral("SEGGER"),
                                     QStringLiteral("J-Link"));
            const QString installPath = settings.value(QStringLiteral("InstallPath")).toString();
            if (installPath.isEmpty())
                continue;

            const QString candidate = QDir(installPath).filePath(name);
            if (QFileInfo(candidate).isFile())
                return candidate;
        }
    }
#endif

    // 再检查 SEGGER 的默认安装位置，兼容未写入注册表的安装。
    QStringList roots;
#ifdef Q_OS_WIN
    roots << QStringLiteral("C:/Program Files/SEGGER") << QStringLiteral("C:/Program Files (x86)/SEGGER");
#elif defined(Q_OS_MACOS)
    roots << QStringLiteral("/Applications/SEGGER");
#else
    roots << QStringLiteral("/opt/SEGGER");
#endif
    for (const QString &root : roots) {
        const QDir rootDir(root);
        const QStringList dirs =
                rootDir.entryList({QStringLiteral("JLink*")}, QDir::Dirs, QDir::Name | QDir::Reversed);
        for (const QString &dir : dirs) {
            const QString candidate = rootDir.filePath(dir + QLatin1Char('/') + name);
            if (QFileInfo::exists(candidate))
                return candidate;
        }
    }
    return {};
}

void hideConsoleWindow(QProcess *process)
{
#ifdef Q_OS_WIN
    // J-Link 的命令行工具是控制台程序；不加这个标志，从 GUI 程序启动时会弹出一个黑窗口
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= 0x08000000; // CREATE_NO_WINDOW
    });
#else
    Q_UNUSED(process)
#endif
}

} // namespace JLinkPaths
