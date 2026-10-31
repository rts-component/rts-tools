#ifndef UPDATECHECKER_H
#define UPDATECHECKER_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

// GitHub Release 里和当前平台对应的那一份安装包
struct ReleaseInfo {
    QString version;   // 去掉前缀 v 的版本号
    QString notes;     // Release 说明（Markdown）
    QUrl pageUrl;      // Release 网页，没有可用安装包时让用户自己去下载
    QUrl assetUrl;     // 当前平台的安装包；为空表示没有对应的包
    QString assetName;
    qint64 assetSize = 0;
};

class UpdateChecker : public QObject
{
    Q_OBJECT

public:
    explicit UpdateChecker(QObject *parent = nullptr);

    // 向 GitHub 查询最新的正式版本，结果通过 finished 返回
    void check();
    bool isChecking() const { return m_reply != nullptr; }

    // remote 是否比 local 新；两边都允许带 v 前缀和 -beta 之类的后缀
    static bool isNewer(const QString &remote, const QString &local);
    // 解析 releases/latest 的响应；assetSuffix 为空表示不挑安装包
    static bool parseRelease(const QByteArray &json, const QString &assetSuffix, ReleaseInfo *info);
    // 当前平台的安装包后缀：Linux 是 .deb，Windows 是 .zip，其余为空
    static QString platformAssetSuffix();

signals:
    // ok 为 false 表示查询失败，error 是原因；成功时 info 是最新版本
    void finished(bool ok, const ReleaseInfo &info, const QString &error);

private:
    void onReplyFinished();
    void checkByRedirect();
    void onRedirectFinished();
    static QString assetNameFor(const QString &version);

    QNetworkAccessManager *m_manager;
    QNetworkReply *m_reply = nullptr;
};

#endif // UPDATECHECKER_H
