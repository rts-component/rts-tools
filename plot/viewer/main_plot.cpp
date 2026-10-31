#include "common/plotendpoints.h"
#include "common/jsonstore.h"
#include "common/translation.h"
#include "plot/viewer/endpointdialog.h"
#include "plot/viewer/plotclient.h"
#include "plot/viewer/plotwindow.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QIcon>
#include <QMessageBox>
#include <QStyleFactory>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("rts-plot"));
    app.setApplicationDisplayName(QStringLiteral("rts-plot"));
    app.setWindowIcon(QIcon(QStringLiteral(":/theme/images/rts-plot.png")));
    // 与主程序保持一致的样式，避免同一套配色在两边观感不同
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    UiLanguage::apply(QStringLiteral("rts-plot"));

    // 语言由 rts-tools 里切换并写入共用的 language.json；监视它，已打开的窗口也跟着换。
    // 保存时文件会被整体替换，所以监视所在目录（先建好），再按文件名过滤。
    const QString languageFile = JsonStore::settingsPath(QStringLiteral("language.json"));
    QDir().mkpath(QFileInfo(languageFile).absolutePath());
    QFileSystemWatcher languageWatcher;
    languageWatcher.addPath(QFileInfo(languageFile).absolutePath());
    QString appliedLanguage = UiLanguage::saved();
    QObject::connect(&languageWatcher, &QFileSystemWatcher::directoryChanged, &app,
                     [&appliedLanguage] {
                         const QString now = UiLanguage::saved();
                         if (now == appliedLanguage)
                             return;
                         appliedLanguage = now;
                         UiLanguage::apply(QStringLiteral("rts-plot"));
                     });

    QCommandLineParser parser;
    parser.setApplicationDescription(
            QStringLiteral("Curve viewer for rts-tools: one window per CSV row, all of the row's "
                           "curves in one chart."));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption groupOption(
            QStringLiteral("group"), QStringLiteral("Connect to the group with this key."),
            QStringLiteral("key"));
    const QCommandLineOption pipeOption(
            QStringLiteral("pipe"), QStringLiteral("Connect to a named pipe directly."),
            QStringLiteral("name"));
    const QCommandLineOption seriesOption(
            QStringLiteral("series"),
            QStringLiteral("Connect to the Nth group of the running rts-tools session."),
            QStringLiteral("n"));
    const QCommandLineOption endpointsOption(
            QStringLiteral("endpoints"),
            QStringLiteral("Endpoints file written by rts-tools (default: "
                           "<exe>/run/plot-endpoints.json)."),
            QStringLiteral("path"));
    parser.addOptions({groupOption, pipeOption, seriesOption, endpointsOption});
    parser.process(app);

    PlotClient client;
    if (parser.isSet(endpointsOption))
        client.setEndpointsFilePath(parser.value(endpointsOption));

    PlotWindow window(&client);
    window.show();

    if (parser.isSet(groupOption)) {
        client.connectToGroup(parser.value(groupOption));
    } else if (parser.isSet(pipeOption)) {
        client.connectToPipe(parser.value(pipeOption));
    } else if (parser.isSet(seriesOption)) {
        bool ok = false;
        const int index = parser.value(seriesOption).toInt(&ok);
        const PlotEndpointFile file = readPlotEndpoints(client.endpointsFilePath());
        if (!ok || index < 1 || !file.valid || index > file.groups.size()) {
            QMessageBox::warning(&window, QApplication::applicationDisplayName(),
                                 QCoreApplication::translate("main", "No group #%1 in %2")
                                         .arg(parser.value(seriesOption),
                                              client.endpointsFilePath()));
        } else {
            client.connectToGroup(file.groups.at(index - 1).key);
        }
    } else {
        QTimer::singleShot(0, &window, [&window] { window.chooseGroup(); });
    }

    return QApplication::exec();
}
