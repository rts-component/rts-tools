#ifndef PLOTPATHS_H
#define PLOTPATHS_H

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>

// 两个可执行文件的名字带平台后缀：Windows 是 .exe，其它平台没有后缀。
inline QString plotExecutableName()
{
#ifdef Q_OS_WIN
    return QStringLiteral("rts-plot.exe");
#else
    return QStringLiteral("rts-plot");
#endif
}

inline QString toolsExecutableName()
{
#ifdef Q_OS_WIN
    return QStringLiteral("rts-tools.exe");
#else
    return QStringLiteral("rts-tools");
#endif
}

inline QString plotExecutablePath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(plotExecutableName());
}

// 可写数据目录（logs / settings / run）。默认是可执行文件目录，便于随程序拷贝；
// AppImage 的挂载目录、deb 安装到 /usr/bin 后的目录都是只读的，此时改用用户数据目录。
inline QString appDataDir()
{
    if (!qEnvironmentVariableIsEmpty("APPIMAGE")
            || !QFileInfo(QCoreApplication::applicationDirPath()).isWritable()) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                + QStringLiteral("/rts-tools");
        QDir().mkpath(dir);
        return dir;
    }
    return QCoreApplication::applicationDirPath();
}

#endif // PLOTPATHS_H
