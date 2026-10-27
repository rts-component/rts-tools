#include "endpointdialog.h"
#include "plotclient.h"
#include "plotendpoints.h"
#include "plotwindow.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QIcon>
#include <QMessageBox>
#include <QStyleFactory>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("rts-plot"));
    app.setApplicationDisplayName(QStringLiteral("rts-plot"));
    app.setWindowIcon(QIcon(QStringLiteral(":/theme/rts-plot.png")));
    // 与主程序保持一致的样式，避免同一套配色在两边观感不同
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

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
                                 QObject::tr("No group #%1 in %2")
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
