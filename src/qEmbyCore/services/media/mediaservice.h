#ifndef MEDIASERVICE_H
#define MEDIASERVICE_H

#include "../../qEmbyCore_global.h"
#include "../../models/media/mediaitem.h"
#include "../../models/media/playbackinfo.h"
#include "../../models/profile/serverprofile.h"
#include <QHash>
#include <QCache>
#include <QImage>
#include <QObject>
#include <QList>
#include <QPixmap>
#include <QPointer>
#include <QDateTime>
#include <QFuture>
#include <QPromise>
#include <QSharedPointer>
#include <QSet>
#include <qcorotask.h>


struct RecommendCache {
    QList<MediaItem> items;
    QDateTime fetchTime;
    QString userId;
    QString serverId;  
    int requestLimit = 0; 

    bool isValid(const QString& currentServerId, const QString& currentUserId,
                 int hours = 24, int requiredLimit = -1) const {
        bool limitSatisfied = requiredLimit < 0;
        if (!limitSatisfied) {
            limitSatisfied = requiredLimit == 0
                                 ? requestLimit == 0
                                 : (requestLimit == 0 ||
                                    requestLimit >= requiredLimit);
        }

        return !items.isEmpty()
            && serverId == currentServerId
            && userId == currentUserId
            && limitSatisfied
            && fetchTime.secsTo(QDateTime::currentDateTime()) < hours * 3600;
    }

    void clear();
    void saveToDisk() const;
    bool loadFromDisk(const QString& currentServerId, const QString& currentUserId, int cacheDurationHours = 24);

    static QString cacheFilePath(const QString& serverId);
};

struct UserViewsCache {
    QList<MediaItem> views;
    QString userId;
    QString serverId;
    bool includesHidden = false;

    bool isValid(const QString& currentServerId, const QString& currentUserId,
                 bool requireHidden = false) const {
        return serverId == currentServerId
            && userId == currentUserId
            && (!requireHidden || includesHidden);
    }

    void clear() {
        views.clear();
        userId.clear();
        serverId.clear();
        includesHidden = false;
    }
};

struct QEMBYCORE_EXPORT DownloadedImageData {
    QByteArray data;
    QString mimeType;

    bool isValid() const {
        return !data.isEmpty();
    }
};

struct QEMBYCORE_EXPORT MediaQueryPage {
    QList<MediaItem> items;
    int totalRecordCount = 0;
    int startIndex = 0;
    int limit = 0;
    bool hasTotalRecordCount = false;

    bool hasMore() const {
        return hasTotalRecordCount &&
               startIndex + items.size() < totalRecordCount;
    }
};

class ServerManager;
class QNetworkAccessManager;

enum class ImageRequestPriority
{
    High,
    Normal,
    Low
};

enum class ImageFetchPolicy
{
    CachePreferred,
    NetworkOnly
};

class QEMBYCORE_EXPORT MediaService : public QObject
{
    Q_OBJECT
public:
    explicit MediaService(ServerManager* serverManager, QObject *parent = nullptr);

    
    
    
    QCoro::Task<QList<MediaItem>> getUserViews(bool includeHidden = false);
    void clearUserViewsCache();
    
    QCoro::Task<MediaQueryPage> getLibraryItemsPage(const QString& parentId, const QString& sortBy = "IsFolder,SortName", const QString& sortOrder = "Ascending", const QString& filters = "", const QString& includeItemTypes = "", int startIndex = 0, int limit = 50, bool recursive = false, bool includeChildCount = false);
    QCoro::Task<QList<MediaItem>> getLibraryItems(const QString& parentId, const QString& sortBy = "IsFolder,SortName", const QString& sortOrder = "Ascending", const QString& filters = "", const QString& includeItemTypes = "", int startIndex = 0, int limit = 50, bool recursive = false, bool includeChildCount = false);
    
    QCoro::Task<QList<MediaItem>> getResumeItems(int limit = 0, const QString& sortBy = "", const QString& sortOrder = "", const QString& serverId = "");
    QCoro::Task<QList<MediaItem>> getLatestItems(int limit = 1000, const QString& sortBy = "DateCreated", const QString& sortOrder = "Descending");
    QCoro::Task<QList<MediaItem>> getPlayedItems(int limit = 0, const QString& sortBy = "DatePlayed", const QString& sortOrder = "Descending");
    QCoro::Task<QList<MediaItem>> getRecommendedMovies(int limit = 1000, const QString& sortBy = "Random", const QString& sortOrder = "Ascending");
    // Dashboard "为你推荐" source: prefers the server-native
    // /Movies/Recommendations endpoint (based on the user's playback
    // history, e.g. "because you watched X") and falls back to
    // getRecommendedMovies (Random) when the endpoint fails, times out or
    // returns nothing. Shares the same RecommendCache as the random path so
    // a successful fetch of either kind is served from cache for the
    // configured DataCacheDuration.
    QCoro::Task<QList<MediaItem>> getDashboardRecommendations(int limit = 0);
    QCoro::Task<MediaQueryPage> getResumeItemsPage(const QString& sortBy = "", const QString& sortOrder = "", int startIndex = 0, int limit = 50);
    QCoro::Task<MediaQueryPage> getLatestItemsPage(const QString& sortBy = "DateCreated", const QString& sortOrder = "Descending", int startIndex = 0, int limit = 50);
    QCoro::Task<MediaQueryPage> getPlayedItemsPage(const QString& sortBy = "DatePlayed", const QString& sortOrder = "Descending", int startIndex = 0, int limit = 50);
    QCoro::Task<MediaQueryPage> getRecommendedMoviesPage(const QString& sortBy = "Random", const QString& sortOrder = "Ascending", int startIndex = 0, int limit = 50);
    
    QCoro::Task<QList<MediaItem>> searchMedia(const QString& searchTerm, const QString& includeItemTypes = "Movie,Series,BoxSet,Person", const QString& sortBy = "", const QString& sortOrder = "Ascending", int limit = 50);

    QCoro::Task<MediaItem> getItemDetail(const QString& itemId,
                                         QString serverId = QString());

    QCoro::Task<PlaybackInfo> getPlaybackInfo(const QString& itemId,
                                              QString serverId = QString());

    // Player capability declaration sent with PlaybackInfo. Official clients
    // always include one; servers (and emby2Alist-style proxies that rewrite
    // the PlaybackInfo response) use it to decide direct-play vs transcode
    // and to emit a usable DirectStreamUrl.
    static QJsonObject buildDeviceProfile();

    // True when a media path can be direct-played from this client: it is an
    // http(s) URL whose host is reachable and routes traffic outside the
    // media server (loopback hosts like alist's 127.0.0.1:5244 fail both).
    // Used consistently by the playback-negotiation trigger and getStreamUrl
    // so the two never disagree.
    static bool isDirectPlayablePath(const QString& path);

    
    
    
    QCoro::Task<QList<MediaItem>> getSeasons(const QString& seriesId,
                                             QString serverId = QString());
    QCoro::Task<QList<MediaItem>> getEpisodes(const QString& seriesId, const QString& seasonId, const QString& sortBy = "ParentIndexNumber,IndexNumber", const QString& sortOrder = "Ascending", QString serverId = QString());
    
    
    QCoro::Task<QList<MediaItem>> getNextUp(const QString& seriesId = "",
                                            QString serverId = QString());

    QCoro::Task<QList<MediaItem>> getAdditionalParts(const QString& itemId,
                                                    QString serverId = QString());

    
    QCoro::Task<bool> toggleFavorite(const QString& itemId, bool isFavorite,
                                     QString serverId = QString());
    
    
    QCoro::Task<void> markAsPlayed(const QString& itemId, QString serverId = QString());
    QCoro::Task<void> markAsUnplayed(const QString& itemId, QString serverId = QString());
    QCoro::Task<void> removeFromResume(const QString& itemId, QString serverId = QString());

    
    void clearRecommendCache();
    void removeRecommendCacheItem(const QString& itemId);

    QCoro::Task<QList<MediaItem>> getSimilarItems(const QString& itemId, int limit = 15,
                                                   QString serverId = QString());
    QCoro::Task<QList<MediaItem>> getItemCollections(const QString& itemId,
                                                      QString serverId = QString());
    QCoro::Task<QList<MediaItem>> getCollectionItems(const QString& collectionId);
    QCoro::Task<MediaQueryPage> getItemsByPersonPage(const QString& personId, const QString& sortBy = "SortName", const QString& sortOrder = "Ascending", int startIndex = 0, int limit = 0);
    QCoro::Task<QList<MediaItem>> getItemsByPerson(const QString& personId, const QString& sortBy = "SortName", const QString& sortOrder = "Ascending");
    QCoro::Task<MediaQueryPage> getItemsByFilterPage(const QString& genreFilter = "", const QString& tagFilter = "", const QString& studioFilter = "", const QString& sortBy = "SortName", const QString& sortOrder = "Ascending", int startIndex = 0, int limit = 0);
    QCoro::Task<QList<MediaItem>> getItemsByFilter(const QString& genreFilter = "", const QString& tagFilter = "", const QString& studioFilter = "", const QString& sortBy = "SortName", const QString& sortOrder = "Ascending", int limit = 0);

    QCoro::Task<QPixmap> fetchImage(QString itemId, QString imageType,
                                    QString imageTag, int maxWidth,
                                    int imageIndex = -1,
                                    ImageRequestPriority priority =
                                        ImageRequestPriority::Normal,
                                    QObject* requestContext = nullptr,
                                    ImageFetchPolicy fetchPolicy =
                                        ImageFetchPolicy::CachePreferred,
                                    QString serverId = QString());
    QCoro::Task<QPixmap> fetchImageByUrl(QString imageUrl);
    QCoro::Task<DownloadedImageData> downloadImageByUrl(QString imageUrl);
    void clearImageCaches();
    void invalidateImageCache(QString itemId, QString imageType,
                              int imageIndex = -1);
    
    QCoro::Task<QList<MediaItem>> getFavoriteMovies(int limit = 50, const QString& sortBy = "SortName", const QString& sortOrder = "Ascending");
    QCoro::Task<QList<MediaItem>> getFavoriteSeries(int limit = 50, const QString& sortBy = "SortName", const QString& sortOrder = "Ascending");
    QCoro::Task<QList<MediaItem>> getFavoriteCollections(int limit = 50, const QString& sortBy = "SortName", const QString& sortOrder = "Ascending");
    
    
    QCoro::Task<QList<MediaItem>> getFavoritePlaylists(int limit = 50, const QString& sortBy = "SortName", const QString& sortOrder = "Ascending");
    QCoro::Task<QList<MediaItem>> getFavoriteFolders(int limit = 50, const QString& sortBy = "SortName", const QString& sortOrder = "Ascending");
    
    QCoro::Task<QList<MediaItem>> getFavoritePeople(int limit = 50, const QString& sortBy = "SortName", const QString& sortOrder = "Ascending");

    QString getStreamUrl(const QString& itemId, const QString& mediaSourceId,
                         QString serverId = QString()) const;
    QString getStreamUrl(const QString& itemId, const MediaSourceInfo& sourceInfo,
                         QString serverId = QString()) const;

    QCoro::Task<QString> reportPlaybackStart(QString itemId, QString mediaSourceId, long long positionTicks);
    QCoro::Task<void> reportPlaybackProgress(QString itemId, QString mediaSourceId, long long positionTicks, bool isPaused, QString playSessionId, QString serverId = QString());
    QCoro::Task<void> reportPlaybackStopped(QString itemId, QString mediaSourceId, long long positionTicks, QString playSessionId, QString serverId = QString());

Q_SIGNALS:
    
    void recommendCacheCleared();

private:
    struct InFlightImageRequest {
        quint64 requestId = 0;
        
        
        QFuture<QImage> future;
    };

    struct PendingImageNetworkSlot {
        quint64 requestId = 0;
        ImageRequestPriority priority = ImageRequestPriority::Normal;
        bool hasRequestContext = false;
        QPointer<QObject> requestContext;
        QSharedPointer<QPromise<bool>> promise;
    };

    ServerManager* m_serverManager;
    QNetworkAccessManager* m_imageManager;
    RecommendCache m_recommendCache; 
    UserViewsCache m_userViewsCache;
    QHash<QString, quint64> m_invalidatedImageRequestVersions;
    
    
    
    QCache<QString, QImage> m_decodedImageCache;
    QHash<QString, InFlightImageRequest> m_inFlightImageRequests;
    quint64 m_imageCacheGeneration = 0;
    quint64 m_nextInFlightImageRequestId = 0;
    quint64 m_nextImageNetworkSlotRequestId = 0;
    quint64 m_nextImageRequestVersion = 0;
    QList<PendingImageNetworkSlot> m_pendingImageNetworkSlots;
    QSet<quint64> m_grantedImageNetworkSlots;
    int m_activeImageNetworkRequests = 0;
    bool m_dispatchingImageNetworkSlots = false;
    
    
    void ensureValidProfile() const;
    QCoro::Task<MediaQueryPage> fetchItemPage(QString basePath,
                                              int startIndex, int limit,
                                              QString context,
                                              QString serverId = QString());
    QCoro::Task<QList<MediaItem>> fetchPagedItemList(QString basePath,
                                                     int requestedLimit,
                                                     QString context,
                                                     bool deduplicateItems = false,
                                                     QString serverId = QString());
    void updateUserViewsCache(QList<MediaItem> views, QString serverId,
                              QString userId, bool includesHidden);
    void updateUserViewsCache(MediaItem view, QString serverId,
                              QString userId);
    void removeInFlightImageRequest(const QString& cacheKey,
                                    quint64 requestId);
    QFuture<bool> acquireImageNetworkSlot(
        ImageRequestPriority priority,
        QPointer<QObject> requestContext,
        bool hasRequestContext,
        quint64* outRequestId);
    void releaseImageNetworkSlot(quint64 requestId);
    void dispatchPendingImageNetworkSlots();

    // 跨服路由：按 serverId 从 servers() 找 profile；空或找不到回退 active。
    // fetchImage / getItemDetail / getPlaybackInfo 等跨服入口共用。
    ServerProfile resolveProfile(QString serverId) const;
};

#endif 
