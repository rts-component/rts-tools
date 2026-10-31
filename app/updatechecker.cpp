#include "app/updatechecker.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QVersionNumber>

namespace {
const char kLatestPageUrl[] = "https://github.com/rts-component/rts-tools/releases/latest";
const char kLatestReleaseUrl[] =
        "https://api.github.com/repos/rts-component/rts-tools/releases/latest";

QString stripPrefix(QString version)
{
    version = version.trimmed();
    if (version.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
        version.remove(0, 1);
    return version;
}
} // namespace

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
    , m_manager(new QNetworkAccessManager(this))
{
}

void UpdateChecker::check()
{
    if (m_reply)
        return;
    QNetworkRequest request{QUrl(QString::fromLatin1(kLatestReleaseUrl))};
    // GitHub API 要求带 User-Agent
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("rts-tools"));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setTransferTimeout(10000);
    m_reply = m_manager->get(request);
    connect(m_reply, &QNetworkReply::finished, this, &UpdateChecker::onReplyFinished);
}

void UpdateChecker::onReplyFinished()
{
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();

    ReleaseInfo info;
    if (reply->error() == QNetworkReply::NoError
        && parseRelease(reply->readAll(), platformAssetSuffix(), &info)) {
        emit finished(true, info, {});
        return;
    }
    // 匿名访问 API 每小时只有 60 次，共用出口 IP 时很容易被限流（403）；
    // 改走网页的 releases/latest 跳转，拿到版本号就够了
    checkByRedirect();
}

void UpdateChecker::checkByRedirect()
{
    QNetworkRequest request{QUrl(QString::fromLatin1(kLatestPageUrl))};
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("rts-tools"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(10000);
    m_reply = m_manager->get(request);
    connect(m_reply, &QNetworkReply::finished, this, &UpdateChecker::onRedirectFinished);
}

void UpdateChecker::onRedirectFinished()
{
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();

    const QUrl target = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    const QString tag = target.path().section(QLatin1Char('/'), -1);
    if (target.isEmpty() || tag.isEmpty()) {
        emit finished(false, {}, reply->error() != QNetworkReply::NoError
                                         ? reply->errorString()
                                         : tr("Unexpected response from the update server"));
        return;
    }
    ReleaseInfo info;
    info.version = stripPrefix(tag);
    info.pageUrl = reply->url().resolved(target);
    const QString name = assetNameFor(info.version);
    if (!name.isEmpty()) {
        info.assetName = name;
        info.assetUrl = QUrl(QStringLiteral("https://github.com/rts-component/rts-tools/releases/download/%1/%2")
                                     .arg(tag, name));
    }
    emit finished(true, info, {});
}

// 与 CI 产物的命名保持一致：deb 是 CPack 默认名，zip 见 build.yml
QString UpdateChecker::assetNameFor(const QString &version)
{
#if defined(Q_OS_WIN)
    return QStringLiteral("rts-tools_%1_x64.zip").arg(version);
#elif defined(Q_OS_LINUX)
    return QStringLiteral("rts-tools_%1_amd64.deb").arg(version);
#else
    Q_UNUSED(version);
    return {};
#endif
}

bool UpdateChecker::isNewer(const QString &remote, const QString &local)
{
    const QVersionNumber remoteVersion = QVersionNumber::fromString(stripPrefix(remote));
    const QVersionNumber localVersion = QVersionNumber::fromString(stripPrefix(local));
    if (remoteVersion.isNull())
        return false;
    return QVersionNumber::compare(remoteVersion, localVersion) > 0;
}

bool UpdateChecker::parseRelease(const QByteArray &json, const QString &assetSuffix, ReleaseInfo *info)
{
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const QString tag = root.value(QStringLiteral("tag_name")).toString();
    if (tag.isEmpty())
        return false;

    ReleaseInfo result;
    result.version = stripPrefix(tag);
    result.notes = root.value(QStringLiteral("body")).toString();
    result.pageUrl = QUrl(root.value(QStringLiteral("html_url")).toString());
    if (!assetSuffix.isEmpty()) {
        const QJsonArray assets = root.value(QStringLiteral("assets")).toArray();
        for (const QJsonValue &value : assets) {
            const QJsonObject asset = value.toObject();
            const QString name = asset.value(QStringLiteral("name")).toString();
            if (!name.endsWith(assetSuffix, Qt::CaseInsensitive))
                continue;
            result.assetName = name;
            result.assetUrl = QUrl(asset.value(QStringLiteral("browser_download_url")).toString());
            result.assetSize = static_cast<qint64>(asset.value(QStringLiteral("size")).toDouble());
            break;
        }
    }
    *info = result;
    return true;
}

QString UpdateChecker::platformAssetSuffix()
{
#if defined(Q_OS_WIN)
    return QStringLiteral(".zip");
#elif defined(Q_OS_LINUX)
    return QStringLiteral(".deb");
#else
    return {};
#endif
}
