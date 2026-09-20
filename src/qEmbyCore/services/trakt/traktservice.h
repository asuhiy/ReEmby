#ifndef TRAKTSERVICE_H
#define TRAKTSERVICE_H

#include "../../qEmbyCore_global.h"
#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUrlQuery>
#include <functional>
#include <qcorotask.h>

class NetworkManager;
class QNetworkAccessManager;

struct MediaItem;

// Trakt ids resolved for a media item. For episodes the trakt id refers to
// the *show* (scrobble/history endpoints address episodes via show ids +
// season/number, so no per-episode lookup is needed).
struct QEMBYCORE_EXPORT TraktMediaIds {
    bool valid = false;
    // "movie" | "episode" | "show" (whole series, history-only)
    QString type;
    int traktId = 0;
    QString imdbId;
    int tmdbId = 0;
    int season = 0; // episode only
    int number = 0; // episode only

    bool isMovie() const { return type == QLatin1String("movie"); }
    bool isEpisode() const { return type == QLatin1String("episode"); }
    bool isShow() const { return type == QLatin1String("show"); }
    bool hasUsableIds() const
    {
        return traktId > 0 || tmdbId > 0 || !imdbId.isEmpty();
    }
};

enum class TraktPollStatus { Approved, Pending, Expired, Failed };

class QEMBYCORE_EXPORT TraktService : public QObject
{
    Q_OBJECT
public:
    static TraktService *instance();

    TraktService(const TraktService &) = delete;
    TraktService &operator=(const TraktService &) = delete;

    bool isLoggedIn() const;
    QString userName() const;
    // Built-in shared credentials (see .cpp) — not user-configurable.
    QString clientId() const;
    QString clientSecret() const;

    struct DeviceCode {
        QString deviceCode;
        QString userCode;
        QString verificationUrl;
        int intervalSeconds = 5;
        int expiresInSeconds = 600;

        bool isValid() const
        {
            return !deviceCode.isEmpty() && !userCode.isEmpty();
        }
    };

    // Step 1 of the device auth flow: ask Trakt for a user code.
    QCoro::Task<DeviceCode> requestDeviceCode();
    // Step 2: poll once for approval. Stores the token and returns Approved
    // on success; Pending/Expired/Failed to continue/stop polling.
    QCoro::Task<TraktPollStatus> pollDeviceToken(const QString &deviceCode);

    void signOut();

    // Scrobble: action is "start" | "pause" | "stop" (stop >80% marks watched).
    QCoro::Task<bool> scrobble(QString action, const TraktMediaIds &ids,
                               double progressPercent);

    // History: mark/unmark watched. A show ids entry marks every episode.
    QCoro::Task<bool> addHistory(const TraktMediaIds &ids);
    QCoro::Task<bool> removeHistory(const TraktMediaIds &ids);

    // GET /sync/playback/{movies|episodes}: percentage stored by Trakt from
    // scrobble pause/stop, or -1 when there is no entry for this item.
    QCoro::Task<double> fetchStoredProgressPercent(const TraktMediaIds &ids);

    // Resolve a MediaItem (Movie/Episode/Series) to Trakt ids. Provider ids
    // are used directly when present; otherwise falls back to Trakt search.
    // Results are cached per session (keyed by Emby id / series name).
    QCoro::Task<TraktMediaIds> resolveIds(const MediaItem &item);

private:
    explicit TraktService(QObject *parent = nullptr);

    struct RawReply {
        int status = 0;
        QJsonObject body;
    };

    // Form POST that reports the HTTP status instead of throwing, so the
    // OAuth token endpoints can surface 4xx errors as plain values.
    QCoro::Task<RawReply> postFormRaw(QString url, const QUrlQuery &form);

    QMap<QString, QString> baseHeaders() const;
    QMap<QString, QString> authorizedHeaders() const;
    // Runs request() with auth headers; on HTTP 401 refreshes the token once
    // and retries. Throws on failure like plain NetworkManager calls.
    QCoro::Task<QJsonObject> authorizedCall(
        const std::function<QCoro::Task<QJsonObject>(
            const QMap<QString, QString> &)> &request);

    QCoro::Task<bool> refreshAccessToken();
    QCoro::Task<bool> writeHistory(const TraktMediaIds &ids, bool add);

    // Persists access/refresh tokens and the user identity from a token
    // endpoint response (shared by the device flow and the refresh flow).
    void storeTokenReply(const QJsonObject &body);

    QJsonObject idsObject(const TraktMediaIds &ids) const;

    NetworkManager *m_networkManager = nullptr;
    QNetworkAccessManager *m_rawNam = nullptr;

    // Session caches for id resolution. Negative cache prevents repeated
    // search round trips for items Trakt cannot identify.
    QHash<QString, TraktMediaIds> m_movieCache;   // key: emby item id
    QHash<QString, TraktMediaIds> m_showCache;    // key: series name (lower)
    QSet<QString> m_failedKeys;
};

#endif // TRAKTSERVICE_H
