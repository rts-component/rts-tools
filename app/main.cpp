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

    UiLanguage::apply(QStringLiteral("rts-tools"));
    SerialPanel w;
    w.show();
    return QApplication::exec();
}
