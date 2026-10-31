#include "app/updatedialog.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {
// 更新包和解压内容都放在这里，每次升级前清掉上一次的残留
QString updateWorkDir()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath(QStringLiteral("rts-tools-update"));
}
} // namespace

UpdateDialog::UpdateDialog(const ReleaseInfo &info, QWidget *parent)
    : QDialog(parent)
    , m_info(info)
    , m_manager(new QNetworkAccessManager(this))
{
    setWindowTitle(tr("Update available"));
    setMinimumWidth(480);

    auto *title = new QLabel(tr("<h3>rts-tools %1 is available</h3><p>You are using %2.</p>")
                                     .arg(info.version, QCoreApplication::applicationVersion()),
                             this);
    title->setTextFormat(Qt::RichText);

    auto *notes = new QTextBrowser(this);
    notes->setOpenExternalLinks(true);
    if (info.notes.isEmpty())
        // 拿不到说明时（API 被限流走了备用方式）把 Release 页面链接放上，让用户自己去看
        notes->setHtml(tr("<p>Release notes are not available here.</p><p><a href=\"%1\">View release notes on GitHub</a></p>")
                               .arg(info.pageUrl.toString().toHtmlEscaped()));
    else
        notes->setMarkdown(info.notes);
    notes->setMinimumHeight(160);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->hide();

    m_updateButton = new QPushButton(info.assetUrl.isEmpty() ? tr("Open download page") : tr("Update now"), this);
    m_updateButton->setDefault(true);
    m_closeButton = new QPushButton(tr("Later"), this);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(m_updateButton);
    buttons->addWidget(m_closeButton);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(title);
    layout->addWidget(notes);
    layout->addWidget(m_status);
    layout->addWidget(m_progress);
    layout->addLayout(buttons);

    connect(m_updateButton, &QPushButton::clicked, this, [this] {
        if (m_info.assetUrl.isEmpty())
            QDesktopServices::openUrl(m_info.pageUrl);
        else
            startDownload();
    });
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::reject);
    // 下载中途关窗口 / 点“取消”都要中止，免得后台继续写文件
    connect(this, &QDialog::finished, this, &UpdateDialog::abortDownload);
}

void UpdateDialog::setBusy(bool busy)
{
    m_updateButton->setEnabled(!busy);
    m_progress->setVisible(busy);
}

void UpdateDialog::abortDownload()
{
    if (!m_reply)
        return;
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->abort();
    reply->deleteLater();
    m_file.close();
}

void UpdateDialog::showError(const QString &message)
{
    setBusy(false);
    m_status->setText(tr("<span style=\"color:#c0392b\">%1</span>").arg(message.toHtmlEscaped()));
}

void UpdateDialog::startDownload()
{
    const QString dir = updateWorkDir();
    QDir(dir).removeRecursively();
    if (!QDir().mkpath(dir)) {
        showError(tr("Cannot create the download folder: %1").arg(dir));
        return;
    }
    m_file.setFileName(QDir(dir).filePath(m_info.assetName));
    if (!m_file.open(QIODevice::WriteOnly)) {
        showError(tr("Cannot write %1: %2").arg(m_file.fileName(), m_file.errorString()));
        return;
    }

    QNetworkRequest request(m_info.assetUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("rts-tools"));
    // 附件下载地址会 302 到对象存储
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    m_reply = m_manager->get(request);

    m_status->setText(tr("Downloading %1 ...").arg(m_info.assetName));
    m_progress->setValue(0);
    setBusy(true);

    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        if (m_reply)
            m_file.write(m_reply->readAll());
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        // GitHub 的 302 响应可能不带长度，退回用 Release 里记录的大小
        if (total <= 0)
            total = m_info.assetSize;
        if (total > 0)
            m_progress->setValue(static_cast<int>(received * 100 / total));
    });
    connect(m_reply, &QNetworkReply::finished, this, &UpdateDialog::onDownloadFinished);
}

void UpdateDialog::onDownloadFinished()
{
    if (!m_reply)
        return; // 已被 abortDownload 中止
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();

    m_file.write(reply->readAll());
    m_file.close();
    if (reply->error() != QNetworkReply::NoError) {
        showError(tr("Download failed: %1").arg(reply->errorString()));
        return;
    }
    if (m_info.assetSize > 0 && m_file.size() != m_info.assetSize) {
        showError(tr("Download is incomplete, please try again."));
        return;
    }
    m_progress->setValue(100);
    install(m_file.fileName());
}

void UpdateDialog::install(const QString &packagePath)
{
    if (packagePath.endsWith(QLatin1String(".deb"), Qt::CaseInsensitive))
        installDeb(packagePath);
    else
        installZip(packagePath);
}

void UpdateDialog::installDeb(const QString &packagePath)
{
    const QString pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
    if (pkexec.isEmpty()) {
        // 没有提权工具时交给系统的软件安装器
        QDesktopServices::openUrl(QUrl::fromLocalFile(packagePath));
        showError(tr("Downloaded to %1. Please install it with your package manager.").arg(packagePath));
        return;
    }

    m_status->setText(tr("Installing, please authenticate when prompted ..."));
    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::finished, this, [this, process, packagePath](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        if (status == QProcess::NormalExit && code == 0) {
            restartApplication();
            return;
        }
        // pkexec 126 = 用户取消授权，127 = 授权失败
        const QString output = QString::fromLocal8Bit(process->readAll()).trimmed();
        showError(code == 126 || code == 127
                          ? tr("Installation was cancelled. The package is saved at %1.").arg(packagePath)
                          : tr("Installation failed: %1").arg(output.isEmpty() ? QString::number(code) : output));
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            showError(tr("Cannot start the installer: %1").arg(process->errorString()));
    });
    process->start(pkexec, {QStringLiteral("dpkg"), QStringLiteral("-i"), packagePath});
}

void UpdateDialog::installZip(const QString &packagePath)
{
#ifdef Q_OS_WIN
    const QString extractDir = QDir(updateWorkDir()).filePath(QStringLiteral("extracted"));
    m_status->setText(tr("Extracting ..."));
    QProcess unzip;
    unzip.start(QStringLiteral("powershell"),
                {QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
                 QStringLiteral("Expand-Archive -LiteralPath '%1' -DestinationPath '%2' -Force")
                         .arg(QDir::toNativeSeparators(packagePath), QDir::toNativeSeparators(extractDir))});
    if (!unzip.waitForFinished(120000) || unzip.exitCode() != 0) {
        showError(tr("Extraction failed: %1").arg(QString::fromLocal8Bit(unzip.readAllStandardError()).trimmed()));
        return;
    }
    // 压缩包里程序在 bin 子目录；兼容直接放在根目录的情况
    const QString sourceDir = QDir(extractDir).exists(QStringLiteral("bin"))
            ? QDir(extractDir).filePath(QStringLiteral("bin"))
            : extractDir;

    // 当前程序还在运行，文件被占用；写个脚本等进程退出后再覆盖并重启
    const QString script = QDir(updateWorkDir()).filePath(QStringLiteral("update.bat"));
    QFile file(script);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        showError(tr("Cannot write %1: %2").arg(script, file.errorString()));
        return;
    }
    const QString appDir = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString text = QStringLiteral(
            "@echo off\r\n"
            "set tries=0\r\n"
            ":retry\r\n"
            "ping 127.0.0.1 -n 3 >nul\r\n"
            "xcopy /E /Y /I /Q \"%1\\*\" \"%2\\\" >nul\r\n"
            "if errorlevel 1 (\r\n"
            "  set /a tries+=1\r\n"
            "  if %tries% LSS 15 goto retry\r\n"
            ")\r\n"
            "start \"\" \"%3\"\r\n"
            "(goto) 2>nul & del \"%~f0\"\r\n")
            .arg(QDir::toNativeSeparators(sourceDir), appDir, exe);
    file.write(text.toLocal8Bit());
    file.close();

    QProcess runner;
    runner.setProgram(QStringLiteral("cmd.exe"));
    runner.setArguments({QStringLiteral("/c"), QDir::toNativeSeparators(script)});
    runner.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= 0x08000000; // CREATE_NO_WINDOW
    });
    if (!runner.startDetached()) {
        showError(tr("Cannot start the updater: %1").arg(runner.errorString()));
        return;
    }
    QCoreApplication::quit();
#else
    // 没有自动安装方案的平台：把包放到下载目录让用户自己处理
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(packagePath).absolutePath()));
    showError(tr("Downloaded to %1. Please install it manually.").arg(packagePath));
#endif
}

void UpdateDialog::restartApplication()
{
    m_status->setText(tr("Update installed. Restarting ..."));
    QProcess::startDetached(QCoreApplication::applicationFilePath(), {});
    QCoreApplication::quit();
}
