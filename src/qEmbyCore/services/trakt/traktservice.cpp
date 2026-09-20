#include "traktservice.h"

#include "../../api/networkmanager.h"
#include "../../config/config_keys.h"
#include "../../config/configstore.h"
#include "../../models/media/mediaitem.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <qcoronetwork.h>
#include <stdexcept>

namespace {

constexpr auto kApiBase = "https://api.trakt.tv";
constexpr auto kAuthBase = "https://trakt.tv";
constexpr int kTraktRequestTimeoutMs = 10000;

// Built-in shared Trakt application credentials. Trakt closed free app
// registration in August 2026 (VIP only), so users can no longer register
// their own apps. Like other community clients (Plex, Infuse, Kodi), qEmby
// ships one shared credential set. These are borrowed from the official
// Jellyfin Trakt plugin (GPL) whose app is registered for the device code
// flow with redirect "urn:ietf:wg:oauth:2.0:oob" (must match in refresh).
// Replace with an own (VIP-registered) application when one is available.
constexpr auto kBuiltInClientId =
    "bfdd2e032c30c35b368f97ef4ec81587b899bcb028b91a1d4ba5589a4b6a7267";
constexpr auto kBuiltInClientSecret =
    "bf9fce37cf45c1de91da009e7ac6fca905a35d7a718bf65a52f92199073a2503";
constexpr auto kRefreshRedirectUri = "urn:ietf:wg:oauth:2.0:oob";

NetworkRequestOptions traktRequestOptions()
{
    NetworkRequestOptions options;
    options.timeoutMs = kTraktRequestTimeoutMs;
    return options;
}

QString providerIdValue(const QVariantMap &providerIds,
                        std::initializer_list<const char *> keys)
{
    for (const char *key : keys) {
        const QString value =
            providerIds.value(QString::fromLatin1(key)).toString().trimmed();
        if (!value.isEmpty()) {
            return value;
        }
    }
    return {};
}

// Trakt search responses arrive as a JSON array (NetworkManager wraps arrays
// in a "data" object). Each entry: {type: "movie"|"show", movie:{...}|show:{...}}
TraktMediaIds firstSearchHit(const QJsonObject &response, const QString &wantType)
{
    const QJsonArray entries = response.value(QStringLiteral("data")).toArray();
    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("type")).toString() != wantType) {
            continue;
        }
        const QJsonObject obj =
            entry.value(wantType).toObject().value(QStringLiteral("ids")).toObject();
        TraktMediaIds ids;
        ids.type = wantType;
        ids.traktId = obj.value(QStringLiteral("trakt")).toInt();
        ids.tmdbId = obj.value(QStringLiteral("tmdb")).toInt();
        ids.imdbId = obj.value(QStringLiteral("imdb")).toString().trimmed();
        if (ids.hasUsableIds()) {
            return ids;
        }
    }
    return {};
}

bool idsMatch(const QJsonObject &remote, const TraktMediaIds &local)
{
    if (local.traktId > 0) {
        const int remoteTrakt = remote.value(QStringLiteral("trakt")).toInt();
        if (remoteTrakt > 0) {
            return remoteTrakt == local.traktId;
        }
    }
    if (local.tmdbId > 0) {
        const int remoteTmdb = remote.value(QStringLiteral("tmdb")).toInt();
        if (remoteTmdb > 0) {
            return remoteTmdb == local.tmdbId;
        }
    }
    if (!local.imdbId.isEmpty()) {
        const QString remoteImdb =
            remote.value(QStringLiteral("imdb")).toString().trimmed();
        if (!remoteImdb.isEmpty()) {
            return remoteImdb == local.imdbId;
        }
    }
    return false;
}

} // namespace

TraktService::TraktService(QObject *parent)
    : QObject(parent)
{
    m_networkManager = new NetworkManager(this);
}

TraktService *TraktService::instance()
{
    static TraktService service;
    return &service;
}

bool TraktService::isLoggedIn() const
{
    return !ConfigStore::instance()
                ->get<QString>(ConfigKeys::TraktAccessToken, QString())
                .trimmed()
                .isEmpty();
}

QString TraktService::userName() const
{
    return ConfigStore::instance()
        ->get<QString>(ConfigKeys::TraktUserName, QString());
}

QString TraktService::clientId() const
{
    return QLatin1String(kBuiltInClientId);
}

QString TraktService::clientSecret() const
{
    return QLatin1String(kBuiltInClientSecret);
}

QMap<QString, QString> TraktService::baseHeaders() const
{
    QMap<QString, QString> headers;
    headers.insert(QStringLiteral("trakt-api-version"), QStringLiteral("2"));
    headers.insert(QStringLiteral("trakt-api-key"), clientId());
    headers.insert(QStringLiteral("User-Agent"),
                   QStringLiteral("ReEmby/1.0 (Trakt)"));
    return headers;
}

QMap<QString, QString> TraktService::authorizedHeaders() const
{
    QMap<QString, QString> headers = baseHeaders();
    const QString token = ConfigStore::instance()
                              ->get<QString>(ConfigKeys::TraktAccessToken,
                                             QString())
                              .trimmed();
    if (!token.isEmpty()) {
        headers.insert(QStringLiteral("Authorization"),
                       QStringLiteral("Bearer %1").arg(token));
    }
    return headers;
}

QCoro::Task<TraktService::RawReply> TraktService::postFormRaw(
    QString url, const QUrlQuery &form)
{
    if (!m_rawNam) {
        m_rawNam = new QNetworkAccessManager(this);
    }
    QNetworkRequest request((QUrl(url)));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
    request.setTransferTimeout(kTraktRequestTimeoutMs);

    const QByteArray data = form.toString(QUrl::FullyEncoded).toUtf8();
    QNetworkReply *reply = m_rawNam->post(request, data);
    co_await reply;

    RawReply result;
    result.status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    reply->deleteLater();

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error == QJsonParseError::NoError) {
        result.body = doc.object();
    }
    co_return result;
}

void TraktService::storeTokenReply(const QJsonObject &body)
{
    auto *store = ConfigStore::instance();
    store->set(ConfigKeys::TraktAccessToken,
               body.value(QStringLiteral("access_token")).toString());
    store->set(ConfigKeys::TraktRefreshToken,
               body.value(QStringLiteral("refresh_token")).toString());
    const QJsonObject user = body.value(QStringLiteral("user")).toObject();
    store->set(ConfigKeys::TraktUserName,
               user.value(QStringLiteral("username")).toString());
    store->set(ConfigKeys::TraktUserSlug,
               user.value(QStringLiteral("ids"))
                       .toObject()
                       .value(QStringLiteral("slug"))
                       .toString());
}

QCoro::Task<TraktService::DeviceCode> TraktService::requestDeviceCode()
{
    DeviceCode result;

    QUrlQuery form;
    form.addQueryItem(QStringLiteral("client_id"), clientId());
    const RawReply reply = co_await postFormRaw(
        QStringLiteral("%1/oauth/device/code").arg(QLatin1String(kAuthBase)),
        form);
    if (reply.status != 200) {
        throw std::runtime_error(
            QStringLiteral("Trakt device code request failed (HTTP %1)")
                .arg(reply.status)
                .toStdString());
    }

    result.deviceCode =
        reply.body.value(QStringLiteral("device_code")).toString();
    result.userCode = reply.body.value(QStringLiteral("user_code")).toString();
    result.verificationUrl =
        reply.body.value(QStringLiteral("verification_url")).toString();
    result.expiresInSeconds =
        reply.body.value(QStringLiteral("expires_in")).toInt(600);
    result.intervalSeconds =
        reply.body.value(QStringLiteral("interval")).toInt(5);
    if (!result.isValid()) {
        throw std::runtime_error("Trakt device code response is invalid");
    }
    co_return result;
}

QCoro::Task<TraktPollStatus> TraktService::pollDeviceToken(
    const QString &deviceCode)
{
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("code"), deviceCode);
    form.addQueryItem(QStringLiteral("client_id"), clientId());
    form.addQueryItem(QStringLiteral("client_secret"), clientSecret());
    const RawReply reply = co_await postFormRaw(
        QStringLiteral("%1/oauth/device/token").arg(QLatin1String(kAuthBase)),
        form);

    if (reply.status == 200) {
        storeTokenReply(reply.body);
        qDebug().noquote() << "[Trakt] Device auth approved"
                           << "| user:" << userName();
        co_return TraktPollStatus::Approved;
    }

    const QString error =
        reply.body.value(QStringLiteral("error")).toString().trimmed();
    // 400 authorization_pending / 429 slow_down: keep waiting.
    if (reply.status == 400
        && (error == QLatin1String("authorization_pending")
            || error.isEmpty())) {
        co_return TraktPollStatus::Pending;
    }
    if (reply.status == 409 || reply.status == 429
        || error == QLatin1String("slow_down")) {
        co_return TraktPollStatus::Pending;
    }
    if (reply.status == 410) {
        co_return TraktPollStatus::Expired;
    }
    qWarning().noquote() << "[Trakt] Device poll failed"
                         << "| httpStatus:" << reply.status
                         << "| error:" << error;
    co_return TraktPollStatus::Failed;
}

void TraktService::signOut()
{
    auto *store = ConfigStore::instance();
    store->set(ConfigKeys::TraktAccessToken, QString());
    store->set(ConfigKeys::TraktRefreshToken, QString());
    store->set(ConfigKeys::TraktUserName, QString());
    store->set(ConfigKeys::TraktUserSlug, QString());
    qDebug().noquote() << "[Trakt] Signed out";
}

QCoro::Task<bool> TraktService::refreshAccessToken()
{
    const QString refreshToken =
        ConfigStore::instance()
            ->get<QString>(ConfigKeys::TraktRefreshToken, QString())
            .trimmed();
    if (refreshToken.isEmpty()) {
        co_return false;
    }

    QUrlQuery form;
    form.addQueryItem(QStringLiteral("refresh_token"), refreshToken);
    form.addQueryItem(QStringLiteral("client_id"), clientId());
    form.addQueryItem(QStringLiteral("client_secret"), clientSecret());
    // Must match the redirect URI registered for the built-in application.
    form.addQueryItem(QStringLiteral("redirect_uri"),
                      QLatin1String(kRefreshRedirectUri));
    form.addQueryItem(QStringLiteral("grant_type"),
                      QStringLiteral("refresh_token"));
    const RawReply reply = co_await postFormRaw(
        QStringLiteral("%1/oauth/token").arg(QLatin1String(kAuthBase)), form);
    if (reply.status != 200) {
        qWarning().noquote() << "[Trakt] Token refresh failed"
                             << "| httpStatus:" << reply.status;
        co_return false;
    }

    auto *store = ConfigStore::instance();
    store->set(ConfigKeys::TraktAccessToken,
               reply.body.value(QStringLiteral("access_token")).toString());
    store->set(ConfigKeys::TraktRefreshToken,
               reply.body.value(QStringLiteral("refresh_token")).toString());
    qDebug().noquote() << "[Trakt] Token refreshed";
    co_return true;
}

QCoro::Task<QJsonObject> TraktService::authorizedCall(
    const std::function<QCoro::Task<QJsonObject>(
        const QMap<QString, QString> &)> &request)
{
    try {
        co_return co_await request(authorizedHeaders());
    } catch (const std::exception &e) {
        if (!QString::fromUtf8(e.what()).contains(QLatin1String("HTTP 401"))) {
            throw;
        }
    }
    // Access token expired: refresh once and retry with the fresh headers.
    if (!co_await refreshAccessToken()) {
        throw std::runtime_error("Trakt authorization failed (401)");
    }
    co_return co_await request(authorizedHeaders());
}

QJsonObject TraktService::idsObject(const TraktMediaIds &ids) const
{
    QJsonObject obj;
    if (ids.traktId > 0) {
        obj.insert(QStringLiteral("trakt"), ids.traktId);
    }
    if (ids.tmdbId > 0) {
        obj.insert(QStringLiteral("tmdb"), ids.tmdbId);
    }
    if (!ids.imdbId.isEmpty()) {
        obj.insert(QStringLiteral("imdb"), ids.imdbId);
    }
    return obj;
}

QCoro::Task<bool> TraktService::scrobble(QString action,
                                         const TraktMediaIds &ids,
                                         double progressPercent)
{
    if (!isLoggedIn() || clientId().isEmpty() || !ids.valid ||
        !ids.hasUsableIds() || ids.isShow()) {
        co_return false;
    }

    QJsonObject payload;
    payload.insert(QStringLiteral("progress"), progressPercent);
    payload.insert(QStringLiteral("app_version"),
                   QCoreApplication::applicationVersion().isEmpty()
                       ? QStringLiteral("1.0")
                       : QCoreApplication::applicationVersion());
    payload.insert(QStringLiteral("app_date"),
                   QDate::currentDate().toString(Qt::ISODate));
    if (ids.isMovie()) {
        payload.insert(QStringLiteral("movie"), QJsonObject{
                                                    {QStringLiteral("ids"),
                                                     idsObject(ids)}});
    } else {
        payload.insert(QStringLiteral("show"),
                       QJsonObject{{QStringLiteral("ids"), idsObject(ids)}});
        payload.insert(QStringLiteral("episode"),
                       QJsonObject{{QStringLiteral("season"), ids.season},
                                   {QStringLiteral("number"), ids.number}});
    }

    try {
        co_await authorizedCall(
            [action, payload](const QMap<QString, QString> &headers)
                -> QCoro::Task<QJsonObject> {
                co_return co_await TraktService::instance()->m_networkManager->post(
                    QStringLiteral("%1/scrobble/%2")
                        .arg(QLatin1String(kApiBase), action),
                    headers, payload, traktRequestOptions());
            });
        co_return true;
    } catch (const std::exception &e) {
        qWarning().noquote() << "[Trakt] Scrobble failed"
                             << "| action:" << action
                             << "| type:" << ids.type
                             << "| progress:" << progressPercent
                             << "| error:" << e.what();
        co_return false;
    }
}

QCoro::Task<bool> TraktService::writeHistory(const TraktMediaIds &ids,
                                             bool add)
{
    if (!isLoggedIn() || clientId().isEmpty() || !ids.valid ||
        !ids.hasUsableIds()) {
        co_return false;
    }

    const QJsonObject entryIds = idsObject(ids);
    QJsonObject payload;
    if (ids.isMovie()) {
        payload.insert(QStringLiteral("movies"),
                       QJsonArray{QJsonObject{{QStringLiteral("ids"), entryIds}}});
    } else if (ids.isEpisode()) {
        payload.insert(
            QStringLiteral("shows"),
            QJsonArray{QJsonObject{
                {QStringLiteral("ids"), entryIds},
                {QStringLiteral("seasons"),
                 QJsonArray{QJsonObject{
                     {QStringLiteral("number"), ids.season},
                     {QStringLiteral("episodes"),
                      QJsonArray{QJsonObject{
                          {QStringLiteral("number"), ids.number}}}}}}}}});
    } else {
        payload.insert(QStringLiteral("shows"),
                       QJsonArray{QJsonObject{{QStringLiteral("ids"), entryIds}}});
    }

    const QString path =
        add ? QStringLiteral("/sync/history") : QStringLiteral("/sync/history/remove");
    try {
        co_await authorizedCall(
            [path, payload](const QMap<QString, QString> &headers)
                -> QCoro::Task<QJsonObject> {
                co_return co_await TraktService::instance()->m_networkManager->post(
                    QStringLiteral("%1%2").arg(QLatin1String(kApiBase), path),
                    headers, payload, traktRequestOptions());
            });
        qDebug().noquote() << "[Trakt] History updated"
                           << "| add:" << add
                           << "| type:" << ids.type
                           << "| traktId:" << ids.traktId;
        co_return true;
    } catch (const std::exception &e) {
        qWarning().noquote() << "[Trakt] History update failed"
                             << "| add:" << add
                             << "| type:" << ids.type
                             << "| error:" << e.what();
        co_return false;
    }
}

QCoro::Task<bool> TraktService::addHistory(const TraktMediaIds &ids)
{
    co_return co_await writeHistory(ids, true);
}

QCoro::Task<bool> TraktService::removeHistory(const TraktMediaIds &ids)
{
    co_return co_await writeHistory(ids, false);
}

QCoro::Task<double> TraktService::fetchStoredProgressPercent(
    const TraktMediaIds &ids)
{
    if (!isLoggedIn() || clientId().isEmpty() || !ids.valid ||
        !ids.hasUsableIds() || ids.isShow()) {
        co_return -1.0;
    }

    const QString path = ids.isMovie() ? QStringLiteral("/sync/playback/movies")
                                       : QStringLiteral("/sync/playback/episodes");
    const TraktMediaIds local = ids;
    const QJsonObject response = co_await authorizedCall(
        [path](const QMap<QString, QString> &headers)
            -> QCoro::Task<QJsonObject> {
            co_return co_await TraktService::instance()->m_networkManager->get(
                QStringLiteral("%1%2").arg(QLatin1String(kApiBase), path),
                headers, traktRequestOptions());
        });

    const QJsonArray entries = response.value(QStringLiteral("data")).toArray();
    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        bool matched = false;
        if (local.isMovie()) {
            matched = idsMatch(entry.value(QStringLiteral("movie"))
                                   .toObject()
                                   .value(QStringLiteral("ids"))
                                   .toObject(),
                               local);
        } else {
            const QJsonObject episode =
                entry.value(QStringLiteral("episode")).toObject();
            matched = idsMatch(entry.value(QStringLiteral("show"))
                                   .toObject()
                                   .value(QStringLiteral("ids"))
                                   .toObject(),
                               local) &&
                      episode.value(QStringLiteral("season")).toInt() ==
                          local.season &&
                      episode.value(QStringLiteral("number")).toInt() ==
                          local.number;
        }
        if (matched) {
            co_return entry.value(QStringLiteral("progress")).toDouble(-1.0);
        }
    }
    co_return -1.0;
}

QCoro::Task<TraktMediaIds> TraktService::resolveIds(const MediaItem &item)
{
    const bool isMovieItem = item.type == QLatin1String("Movie");
    const bool isEpisodeItem = item.type == QLatin1String("Episode");
    const bool isSeriesItem = item.type == QLatin1String("Series");
    if (!isMovieItem && !isEpisodeItem && !isSeriesItem) {
        co_return {};
    }

    // ---- Movie: provider ids are authoritative, no network needed.
    if (isMovieItem) {
        const auto cached = m_movieCache.constFind(item.id);
        if (cached != m_movieCache.constEnd()) {
            co_return cached.value();
        }
        const QString imdb = providerIdValue(item.providerIds,
                                             {"Imdb", "imdb", "IMDb", "imdbid"});
        const QString tmdbStr = providerIdValue(
            item.providerIds, {"Tmdb", "tmdb", "TMDb", "tmdbid"});
        if (!imdb.isEmpty() || !tmdbStr.isEmpty()) {
            TraktMediaIds ids;
            ids.valid = true;
            ids.type = QStringLiteral("movie");
            ids.imdbId = imdb;
            ids.tmdbId = tmdbStr.toInt();
            m_movieCache.insert(item.id, ids);
            co_return ids;
        }

        // No provider ids: fall back to a title (+year) search, one attempt
        // per session per item.
        const QString failedKey = QStringLiteral("movie/%1").arg(item.id);
        if (m_failedKeys.contains(failedKey)) {
            co_return {};
        }
        QString queryText = item.name.trimmed();
        if (queryText.isEmpty()) {
            queryText = item.originalTitle.trimmed();
        }
        if (queryText.isEmpty()) {
            co_return {};
        }
        QUrl url(QStringLiteral("%1/search/movie").arg(QLatin1String(kApiBase)));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("query"), queryText);
        if (item.productionYear > 0) {
            query.addQueryItem(QStringLiteral("year"),
                               QString::number(item.productionYear));
        }
        url.setQuery(query);
        try {
            const QJsonObject response = co_await m_networkManager->get(
                url.toString(), baseHeaders(), traktRequestOptions());
            TraktMediaIds ids = firstSearchHit(response, QStringLiteral("movie"));
            if (ids.hasUsableIds()) {
                ids.valid = true;
                m_movieCache.insert(item.id, ids);
                qDebug().noquote() << "[Trakt] Movie resolved via search"
                                   << "| title:" << queryText
                                   << "| traktId:" << ids.traktId;
                co_return ids;
            }
        } catch (const std::exception &e) {
            qWarning().noquote() << "[Trakt] Movie search failed"
                                 << "| title:" << queryText
                                 << "| error:" << e.what();
            co_return {};
        }
        m_failedKeys.insert(failedKey);
        qDebug().noquote() << "[Trakt] Movie not found on Trakt"
                           << "| title:" << queryText;
        co_return {};
    }

    // ---- Episode/Series: resolve the show (cached by series name).
    QString seriesName;
    int season = 0;
    int number = 0;
    if (isEpisodeItem) {
        seriesName = item.seriesName.trimmed();
        season = item.parentIndexNumber;
        number = item.indexNumber;
        if (season <= 0) {
            season = 1;
        }
        if (number <= 0) {
            co_return {}; // cannot address an episode without its number
        }
    } else {
        seriesName = item.name.trimmed();
    }
    if (seriesName.isEmpty()) {
        seriesName = item.originalTitle.trimmed();
    }
    if (seriesName.isEmpty()) {
        co_return {};
    }

    const QString showKey = seriesName.toLower();
    const auto cachedShow = m_showCache.constFind(showKey);
    TraktMediaIds showIds;
    if (cachedShow != m_showCache.constEnd()) {
        showIds = cachedShow.value();
    } else {
        const QString failedKey = QStringLiteral("show/%1").arg(showKey);
        if (m_failedKeys.contains(failedKey)) {
            co_return {};
        }
        QUrl url(QStringLiteral("%1/search/show").arg(QLatin1String(kApiBase)));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("query"), seriesName);
        url.setQuery(query);
        try {
            const QJsonObject response = co_await m_networkManager->get(
                url.toString(), baseHeaders(), traktRequestOptions());
            showIds = firstSearchHit(response, QStringLiteral("show"));
        } catch (const std::exception &e) {
            qWarning().noquote() << "[Trakt] Show search failed"
                                 << "| series:" << seriesName
                                 << "| error:" << e.what();
            co_return {};
        }
        if (!showIds.hasUsableIds()) {
            m_failedKeys.insert(failedKey);
            qDebug().noquote() << "[Trakt] Show not found on Trakt"
                               << "| series:" << seriesName;
            co_return {};
        }
        m_showCache.insert(showKey, showIds);
        qDebug().noquote() << "[Trakt] Show resolved via search"
                           << "| series:" << seriesName
                           << "| traktId:" << showIds.traktId;
    }

    TraktMediaIds result = showIds;
    if (isEpisodeItem) {
        result.type = QStringLiteral("episode");
        result.season = season;
        result.number = number;
    } else {
        result.type = QStringLiteral("show");
    }
    result.valid = true;
    co_return result;
}
