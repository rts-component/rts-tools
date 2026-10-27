#include "plot/server/plotlauncher.h"

#include "common/plotpaths.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QTimer>

namespace {
constexpr int kRestartDelayMs = 1500;
constexpr int kMaxRestarts = 3;
constexpr int kTerminateTimeoutMs = 1500; // terminate() 之后等多久，超时改用 kill()
} // namespace

PlotLauncher::PlotLauncher(PlotServer *link, QObject *parent)
    : QObject(parent)
    , m_link(link)
    , m_executablePath(plotExecutablePath())
{
}

PlotLauncher::~PlotLauncher()
{
    stopAll();
}

void PlotLauncher::syncSeries(const QVector<PlotServer::GroupDefinition> &groups)
{
    QSet<QString> wanted;
    for (const PlotServer::GroupDefinition &group : groups)
        wanted.insert(group.key);

    const QStringList running = m_processes.keys();
    for (const QString &key : running) {
        if (!wanted.contains(key))
            stop(key); // 取消勾选/删除行：关掉我们自己拉起的窗口
    }

    if (groups.isEmpty())
        return;

    if (QFileInfo::exists(m_executablePath)) {
        m_exeMissingReported = false; // 之后又丢了要能再提示
    } else {
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

    for (const PlotServer::GroupDefinition &group : groups) {
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
    connect(process, &QProcess::started, this, [this, groupKey] {
        m_errors.remove(groupKey); // 启动成功了：同一原因以后再失败要能再提示
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
    m_errors.remove(groupKey);
    process->disconnect(this);
    process->terminate(); // GUI 程序收到 WM_CLOSE / SIGTERM，窗口正常关闭

    // 不在界面线程里等：退出后释放对象；超时还没退出就强杀
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), process,
            &QObject::deleteLater);
    QTimer::singleShot(kTerminateTimeoutMs, process, [process] {
        if (process->state() != QProcess::NotRunning)
            process->kill();
    });
}

// 只在析构时用：先让所有窗口同时收到关闭请求，再按总预算等待，最多卡 kTerminateTimeoutMs
void PlotLauncher::stopAll()
{
    const QList<QProcess *> processes = m_processes.values();
    m_processes.clear();
    m_restarts.clear();

    for (QProcess *process : processes) {
        process->disconnect(this);
        process->terminate();
    }

    QElapsedTimer budget;
    budget.start();
    for (QProcess *process : processes) {
        const int remaining = qMax(0, kTerminateTimeoutMs - int(budget.elapsed()));
        if (!process->waitForFinished(remaining))
            process->kill();
        process->deleteLater();
    }
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
