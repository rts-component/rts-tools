#ifndef JLINKPATHS_H
#define JLINKPATHS_H

#include <QString>

class QProcess;

// 定位 SEGGER J-Link 软件包里的命令行工具（JLinkExe、JLinkGDBServerCLExe 等）
namespace JLinkPaths {

// name 是该平台上的可执行文件名（Windows 带 .exe）；先查 PATH，再查注册表和默认安装目录，找不到返回空
QString findJLinkTool(const QString &name);

// Windows 下不让控制台程序弹黑窗口，其他平台什么也不做
void hideConsoleWindow(QProcess *process);

} // namespace JLinkPaths

#endif // JLINKPATHS_H
