#ifndef UPDATEDIALOG_H
#define UPDATEDIALOG_H

#include "app/updatechecker.h"

#include <QDialog>
#include <QFile>

class QDialogButtonBox;
class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QProgressBar;
class QPushButton;
class QTextBrowser;

// 展示新版本说明，并负责下载与安装。
// Linux：下载 .deb，用 pkexec dpkg -i 安装后重启；Windows：下载 .zip，
// 解压后由脚本在程序退出后覆盖当前目录并重新启动。
class UpdateDialog : public QDialog
{
    Q_OBJECT

public:
    explicit UpdateDialog(const ReleaseInfo &info, QWidget *parent = nullptr);

private:
    void startDownload();
    void onDownloadFinished();
    void install(const QString &packagePath);
    void installDeb(const QString &packagePath);
    void installZip(const QString &packagePath);
    void restartApplication();
    void showError(const QString &message);
    void setBusy(bool busy);
    void abortDownload();

    ReleaseInfo m_info;
    QNetworkAccessManager *m_manager;
    QNetworkReply *m_reply = nullptr;
    QFile m_file;
    QLabel *m_status;
    QProgressBar *m_progress;
    QPushButton *m_updateButton;
    QPushButton *m_closeButton;
};

#endif // UPDATEDIALOG_H
