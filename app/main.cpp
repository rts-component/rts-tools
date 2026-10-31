#include "app/serialpanel.h"
#include "common/translation.h"

#include <QApplication>
#include <QIcon>
#include <QStyleFactory>

#ifndef RTS_TOOLS_VERSION
#define RTS_TOOLS_VERSION "0.0.0"
#endif

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setApplicationName(QStringLiteral("rts-tools"));
    a.setApplicationVersion(QStringLiteral(RTS_TOOLS_VERSION));
    a.setWindowIcon(QIcon(QStringLiteral(":/theme/images/rts-tools.png")));
    // Fusion 完全遵循 QSS 与调色板，避免原生样式在自定义边框下产生绘制伪影。
    a.setStyle(QStyleFactory::create("Fusion"));
    // 消息框的宽度由正文决定；正文很短时窗口过窄，标题栏的文字会被按钮挤掉
    a.setStyleSheet(QStringLiteral("QMessageBox QLabel#qt_msgbox_label { min-width: 320px; }"));

    UiLanguage::apply(QStringLiteral("rts-tools"));
    SerialPanel w;
    w.show();
    return QApplication::exec();
}
