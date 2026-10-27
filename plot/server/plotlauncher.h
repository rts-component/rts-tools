#ifndef PLOTLAUNCHER_H
#define PLOTLAUNCHER_H

#include "plot/server/plotserver.h"

#include <QHash>
#include <QObject>
#include <QVector>

class QProcess;

// 勾选曲线时自动为每个“组”（CSV 的一行）拉起一个 rts-plot：
// 该行勾选曲线的数字列都画在同一个图表窗口里。
// 只管理自己启动的进程：手动启动并已连接的图表不会被重复拉起，也不会被关掉。
class PlotLauncher : public QObject
{
    Q_OBJECT

public:
    explicit PlotLauncher(PlotServer *link, QObject *parent = nullptr);
    ~PlotLauncher() override;

    // 与 PlotServer::syncSeries 保持同一份分组集合：新增的拉起，移除的关闭
    void syncSeries(const QVector<PlotServer::GroupDefinition> &groups);
    void stopAll();

signals:
    void launchFailed(const QString &message);
    // 图表窗口已经不存在了（用户关掉窗口，或崩溃后放弃重启）：勾选状态要跟着取消
    void viewerClosed(const QString &groupKey);

private:
    void launch(const QString &groupKey);
    void stop(const QString &groupKey);
    void handleFinished(const QString &groupKey, int exitCode);

    PlotServer *m_link = nullptr;
    QString m_executablePath;
    QHash<QString, QProcess *> m_processes;
    QHash<QString, int> m_restarts;
    QHash<QString, QString> m_errors;
    bool m_exeMissingReported = false;
};

#endif // PLOTLAUNCHER_H
