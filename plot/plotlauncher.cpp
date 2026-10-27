#include "plotlauncher.h"

#include "plotpaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QTimer>

namespace {
constexpr int kRestartDelayMs = 1500;
constexpr int kMaxRestarts = 3;
} // namespace

PlotLauncher::PlotLauncher(PlotLink *link, QObject *parent)
    : QObject(parent)
    , m_link(link)
    , m_executablePath(plotExecutablePath())
{
}

PlotLauncher::~PlotLauncher()
{
    stopAll();
}

void PlotLauncher::setExecutablePath(const QString &path)
{
    if (!path.isEmpty())
        m_executablePath = path;
}

void PlotLauncher::syncSeries(const QVector<PlotLink::GroupDefinition> &groups)
{
    QSet<QString> wanted;
    for (const PlotLink::GroupDefinition &group : groups)
        wanted.insert(group.key);

    const QStringList running = m_processes.keys();
    for (const QString &key : running) {
        if (!wanted.contains(key))
            stop(key); // 取消勾选/删除行：关掉我们自己拉起的窗口
    }

    if (groups.isEmpty())
        return;

    if (!QFileInfo::exists(m_executablePath)) {
        if (!m_exeMissingReported) {
            m_exeMissingReported = true;
            emit launchFailed(tr("Cannot find %1. Build the rts-plot target or copy it next to %2.")
                                      .arg(QDir::toNativeSeparators(m_executablePath),
                                           toolsExecutableName()));
        }
        return;
    }

    // rts-plot 用 --group 在端点文件里查管道名，所以先落盘再启动
    m_link->writeEndpointsFile();

    for (const PlotLink::GroupDefinition &group : groups) {
        if (m_processes.contains(group.key))
            continue;
        if (m_link->isAttached(group.key))
            continue; // 已经有人（多半是手动启动的窗口）连上了
        launch(group.key);
    }
}

void PlotLauncher::launch(const QString &groupKey)
{
    auto *process = new QProcess(this);
    process->setProgram(m_executablePath);
    process->setArguments({QStringLiteral("--group"), groupKey});
    process->setWorkingDirectory(QCoreApplication::applicationDirPath());

    connect(process, &QProcess::errorOccurred, this,
            [this, groupKey, process](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                const QString detail = process->errorString();
                if (m_errors.value(groupKey) == detail)
                    return; // 同一原因只提示一次
                m_errors.insert(groupKey, detail);
                emit launchFailed(tr("Cannot start %1: %2")
                                          .arg(QDir::toNativeSeparators(m_executablePath), detail));
            });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, groupKey](int exitCode, QProcess::ExitStatus) {
                handleFinished(groupKey, exitCode);
            });

    m_processes.insert(groupKey, process);
    process->start();
}

void PlotLauncher::stop(const QString &groupKey)
{
    QProcess *process = m_processes.take(groupKey);
    if (!process)
        return;

    m_restarts.remove(groupKey);
    process->disconnect(this);
    process->terminate(); // GUI 程序收到 WM_CLOSE，窗口正常关闭
    if (!process->waitForFinished(1500))
        process->kill();
    process->deleteLater();
}

void PlotLauncher::stopAll()
{
    const QStringList keys = m_processes.keys();
    for (const QString &key : keys)
        stop(key);
}

void PlotLauncher::handleFinished(const QString &groupKey, int exitCode)
{
    QProcess *process = m_processes.take(groupKey);
    if (process)
        process->deleteLater();

    // 用户自己关掉窗口（正常退出）时不要再弹回来，同时让主界面取消勾选
    if (exitCode == 0) {
        m_restarts.remove(groupKey);
        emit viewerClosed(groupKey);
        return;
    }

    const int restarts = m_restarts.value(groupKey, 0) + 1;
    if (restarts > kMaxRestarts) {
        m_restarts.remove(groupKey);
        emit viewerClosed(groupKey);
        emit launchFailed(tr("rts-plot for \"%1\" exited unexpectedly (code %2) and will not be "
                             "restarted automatically.")
                                  .arg(m_link->groupTitleOf(groupKey))
                                  .arg(exitCode));
        return;
    }
    m_restarts.insert(groupKey, restarts);

    QTimer::singleShot(kRestartDelayMs, this, [this, groupKey] {
        if (m_processes.contains(groupKey))
            return;
        if (!m_link->groupKeys().contains(groupKey))
            return; // 期间取消了勾选
        launch(groupKey);
    });
}
