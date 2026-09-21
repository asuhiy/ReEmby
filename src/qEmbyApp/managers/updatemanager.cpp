#include "updatemanager.h"

#include "config/config_keys.h"
#include "config/configstore.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStringList>
#include <QVersionNumber>

namespace {

// 更新源：本 fork 自己的 GitHub releases（不再检测上游 AlanHJ/qEmby）。
const QUrl kLatestReleaseApi(
    QStringLiteral("https://api.github.com/repos/asuhiy/ReEmby/releases/latest"));

// 自动检查的最小间隔。GitHub 未认证 API 的配额是 60 次/小时/IP，而本程序
// 每次启动都会自动查一次 —— 开发期反复启动客户端就能把配额耗光（实测踩到
// 「已达到 GitHub 访问频率上限」）。6 小时 ⇒ 一天最多 4 次，远低于上限，
// 更新提示也不至于太滞后。想立刻检查可以走「关于」页的手动检查。
constexpr qint64 kAutomaticCheckMinIntervalSecs = 6 * 3600;

QString normalizedVersion(QString version)
{
    version = version.trimmed();
    if (version.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        version.remove(0, 1);
    }
    const qsizetype suffixPos = version.indexOf(QRegularExpression(QStringLiteral("[-+]")));
    return suffixPos >= 0 ? version.left(suffixPos) : version;
}

struct SelectedReleaseAsset {
    QUrl url;
    QString name;
    QString sha256Digest;
    qint64 size = 0;
};

SelectedReleaseAsset platformReleaseAsset(const QJsonArray &assets)
{
#if defined(Q_OS_WIN)
    const QStringList preferredExtensions = {QStringLiteral(".exe"),
                                              QStringLiteral(".msi")};
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    const QStringList preferredExtensions = {QStringLiteral(".dmg"),
                                              QStringLiteral(".pkg")};
#else
    const QStringList preferredExtensions = {QStringLiteral(".appimage"),
                                              QStringLiteral(".deb"),
                                              QStringLiteral(".rpm")};
#endif

    for (const QString &extension : preferredExtensions) {
        for (const QJsonValue &value : assets) {
            const QJsonObject asset = value.toObject();
            const QString name = asset.value(QStringLiteral("name")).toString();
            if (!name.endsWith(extension, Qt::CaseInsensitive)) {
                continue;
            }
            const QUrl url(asset.value(QStringLiteral("browser_download_url")).toString());
            if (url.isValid() && url.scheme() == QStringLiteral("https")) {
                QString digest =
                    asset.value(QStringLiteral("digest")).toString().trimmed();
                if (digest.startsWith(QStringLiteral("sha256:"),
                                      Qt::CaseInsensitive)) {
                    digest.remove(0, 7);
                }
                return {url, name, digest.toLower(),
                        asset.value(QStringLiteral("size")).toInteger()};
            }
        }
    }
    return {};
}

} 

UpdateManager *UpdateManager::instance()
{
    static auto *manager = new UpdateManager(QCoreApplication::instance());
    return manager;
}

UpdateManager::UpdateManager(QObject *parent)
    : QObject(parent), m_networkManager(new QNetworkAccessManager(this))
{
    const qint64 lastCheckSecs = ConfigStore::instance()->get<qint64>(
        QString::fromLatin1(ConfigKeys::UpdateLastAutomaticCheck), 0);
    if (lastCheckSecs > 0) {
        m_lastAutomaticCheck = QDateTime::fromSecsSinceEpoch(lastCheckSecs);
    }

    // 用户重新打开「检查更新」开关时清掉节流记录 —— 否则要等满一个间隔
    // 才会去查，看起来像是坏了。
    connect(ConfigStore::instance(), &ConfigStore::valueChanged, this,
            [this](const QString &key, const QVariant &value) {
                if (key == ConfigKeys::CheckForUpdates && value.toBool()) {
                    m_lastAutomaticCheck = QDateTime();
                }
            });
}

bool UpdateManager::isChecking(CheckMode mode) const
{
    return mode == CheckMode::Automatic ? m_automaticCheckInProgress
                                        : m_manualCheckInProgress;
}

bool UpdateManager::shouldRunAutomaticCheck() const
{
    if (!m_lastAutomaticCheck.isValid()) {
        return true;  // 从没查过，或刚被「重新打开开关」重置
    }
    return m_lastAutomaticCheck.secsTo(QDateTime::currentDateTime()) >=
           kAutomaticCheckMinIntervalSecs;
}

void UpdateManager::rememberAutomaticCheck()
{
    m_lastAutomaticCheck = QDateTime::currentDateTime();
    ConfigStore::instance()->set(
        QString::fromLatin1(ConfigKeys::UpdateLastAutomaticCheck),
        m_lastAutomaticCheck.toSecsSinceEpoch());
}

void UpdateManager::checkForUpdates(CheckMode mode)
{
    bool &inProgress = mode == CheckMode::Automatic
                           ? m_automaticCheckInProgress
                           : m_manualCheckInProgress;
    if (inProgress) {
        qInfo() << "UpdateManager: check already in progress"
                << "| mode="
                << (mode == CheckMode::Automatic ? "automatic" : "manual");
        return;
    }

    // 自动检查要节流，手动检查不节流（那是用户的明确意图）。
    if (mode == CheckMode::Automatic && !shouldRunAutomaticCheck()) {
        qInfo() << "UpdateManager: automatic check throttled"
                << "| lastCheck=" << m_lastAutomaticCheck.toString(Qt::ISODate);
        return;
    }

    inProgress = true;

    // 记账放在"发起请求"时、而不是等成功之后：否则一旦失败，下次启动又
    // 会重试，配额反而更容易被耗光。
    if (mode == CheckMode::Automatic) {
        rememberAutomaticCheck();
    }

    qInfo() << "UpdateManager: checking GitHub release"
            << "| mode=" << (mode == CheckMode::Automatic ? "automatic" : "manual")
            << "| currentVersion=" << QCoreApplication::applicationVersion();

    QNetworkRequest request(kLatestReleaseApi);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("ReEmby/%1")
                          .arg(QCoreApplication::applicationVersion()));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2026-03-10");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, mode]() { handleReply(reply, mode); });
}

void UpdateManager::handleReply(QNetworkReply *reply, CheckMode mode)
{
    if (mode == CheckMode::Automatic) {
        m_automaticCheckInProgress = false;
    } else {
        m_manualCheckInProgress = false;
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray payload = reply->readAll();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkErrorText = reply->errorString();
    reply->deleteLater();

    
    if (status == 404) {
        qInfo() << "UpdateManager: repository has no published release";
        Q_EMIT noUpdateAvailable(mode);
        return;
    }
    if (status == 403
        && reply->rawHeader("x-ratelimit-remaining") == QByteArrayLiteral("0")) {
        // Unauthenticated GitHub API access allows 60 requests per hour per
        // address, and the answer once that is used up is a bare 403. Report it
        // as a quota problem rather than as a connection failure.
        const qint64 resetEpoch = reply->rawHeader("x-ratelimit-reset").toLongLong();
        const QString resetText =
            resetEpoch > 0
                ? QDateTime::fromSecsSinceEpoch(resetEpoch).toString(QStringLiteral("HH:mm"))
                : QString();
        qWarning() << "UpdateManager: GitHub API rate limit reached"
                   << "| remaining=0 | resets at=" << resetText;
        Q_EMIT checkFailed(
            tr("GitHub rate limit reached (60 checks per hour without a token). Try again after %1.")
                .arg(resetText),
            mode);
        return;
    }
    if (networkError != QNetworkReply::NoError) {
        qWarning() << "UpdateManager: release request failed"
                   << "| status=" << status << "| error=" << networkErrorText;
        Q_EMIT checkFailed(tr("Could not connect to GitHub: %1").arg(networkErrorText),
                           mode);
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        qWarning() << "UpdateManager: invalid GitHub response"
                   << "| parseError=" << parseError.errorString();
        Q_EMIT checkFailed(tr("GitHub returned invalid update information."), mode);
        return;
    }

    const QJsonObject release = document.object();
    const QString tag = release.value(QStringLiteral("tag_name")).toString();
    const QString remoteVersionText = normalizedVersion(tag);
    const QString localVersionText =
        normalizedVersion(QCoreApplication::applicationVersion());
    const QVersionNumber remoteVersion = QVersionNumber::fromString(remoteVersionText);
    const QVersionNumber localVersion = QVersionNumber::fromString(localVersionText);
    if (remoteVersion.isNull() || localVersion.isNull()) {
        qWarning() << "UpdateManager: invalid version"
                   << "| local=" << localVersionText << "| remote=" << tag;
        Q_EMIT checkFailed(tr("The release contains an invalid version number: %1")
                               .arg(tag),
                           mode);
        return;
    }

    qInfo() << "UpdateManager: release response parsed"
            << "| local=" << localVersionText << "| remote=" << remoteVersionText
            << "| assets=" << release.value(QStringLiteral("assets")).toArray().size();
    if (QVersionNumber::compare(remoteVersion, localVersion) <= 0) {
        Q_EMIT noUpdateAvailable(mode);
        return;
    }

    UpdateInfo info;
    info.version = tag.trimmed().isEmpty() ? remoteVersionText : tag.trimmed();
    info.name = release.value(QStringLiteral("name")).toString().trimmed();
    info.releaseNotes = release.value(QStringLiteral("body")).toString().trimmed();
    info.publishedAt = QDateTime::fromString(
        release.value(QStringLiteral("published_at")).toString(), Qt::ISODate);
    info.releaseUrl = QUrl(release.value(QStringLiteral("html_url")).toString());
    const SelectedReleaseAsset selectedAsset = platformReleaseAsset(
        release.value(QStringLiteral("assets")).toArray());
    info.downloadUrl = selectedAsset.url;
    info.assetName = selectedAsset.name;
    info.sha256Digest = selectedAsset.sha256Digest;
    info.assetSize = selectedAsset.size;

    const QString ignoredVersion = ConfigStore::instance()->get<QString>(
        ConfigKeys::IgnoredUpdateVersion, QString());
    if (mode == CheckMode::Automatic && ignoredVersion == info.version) {
        qInfo() << "UpdateManager: release ignored by user"
                << "| version=" << info.version;
        return;
    }
    if (mode == CheckMode::Automatic && m_openedVersion == info.version) {
        qInfo() << "UpdateManager: release already opened this session"
                << "| version=" << info.version;
        return;
    }

    Q_EMIT updateAvailable(info, mode);
}

bool UpdateManager::openUpdate(const UpdateInfo &info)
{
    const QUrl target = info.downloadUrl.isValid() ? info.downloadUrl
                                                    : info.releaseUrl;
    if (!target.isValid() || target.scheme() != QStringLiteral("https")) {
        qWarning() << "UpdateManager: refusing invalid update URL" << target;
        return false;
    }

    qInfo() << "UpdateManager: opening update"
            << "| version=" << info.version
            << "| directAsset=" << info.downloadUrl.isValid();
    const bool opened = QDesktopServices::openUrl(target);
    if (opened) {
        m_openedVersion = info.version;
        Q_EMIT updateOpened(info.version);
    }
    return opened;
}
