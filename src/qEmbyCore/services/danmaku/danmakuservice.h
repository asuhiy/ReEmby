#ifndef DANMAKUSERVICE_H
#define DANMAKUSERVICE_H

#include "../../models/danmaku/danmakumodels.h"

#include <QObject>
#include <qcorotask.h>

class NetworkManager;
class ServerManager;
class DandanplayProvider;
class DanmuApiProvider;
class BiliBiliDanmakuProvider;
class DanmakuCacheStore;

class QEMBYCORE_EXPORT DanmakuService : public QObject
{
    Q_OBJECT
public:
    explicit DanmakuService(NetworkManager *networkManager,
                            ServerManager *serverManager,
                            QObject *parent = nullptr);
    ~DanmakuService() override;

    static DanmakuMatchCandidate createLocalFileCandidate(
        QString filePath);
    static QString localDanmakuDirectoryPath(
        QString serverId = QString());
    static bool ensureLocalDanmakuDirectory(
        QString serverId = QString());

    DanmakuRenderOptions renderOptions() const;
    DanmakuProviderConfig providerConfig(
        QString serverId = QString()) const;

    QCoro::Task<DanmakuLoadResult> prepareDanmaku(
        DanmakuMediaContext context,
        QString manualKeyword = QString());
    QCoro::Task<DanmakuLoadResult> prepareDanmakuForCandidate(
        DanmakuMediaContext context,
        DanmakuMatchCandidate candidate);
    QCoro::Task<QList<DanmakuMatchCandidate>> searchCandidates(
        DanmakuMediaContext context,
        QString manualKeyword = QString());
    QCoro::Task<QList<DanmakuMatchCandidate>> searchAllCandidates(
        DanmakuMediaContext context,
        QString manualKeyword = QString());
    // Series-level picker only: aggregate every enabled danmaku provider
    // across ALL configured Emby servers (not just the active one), so
    // users who happen to have their danmaku sources attached to a
    // different server still see them in the series matcher.
    QCoro::Task<QList<DanmakuMatchCandidate>> searchAllCandidatesAcrossServers(
        DanmakuMediaContext context,
        QString manualKeyword = QString());
    QCoro::Task<DanmakuMatchResult> resolveMatch(
        DanmakuMediaContext context,
        QString manualKeyword = QString());

    void saveManualMatch(const DanmakuMediaContext &context,
                         const DanmakuMatchCandidate &candidate);

    // Per-episode series bindings: the detail-page multi matcher saves one
    // candidate per picked episode, keyed by (seriesId, seasonNumber,
    // episodeNumber). The player uses these as the manual override when
    // an episode plays back.
    void saveSeriesBindings(const QString &serverId,
                            const QString &seriesId,
                            int seasonNumber,
                            const QList<DanmakuMatchCandidate> &candidates);
    void clearSeriesBindings(const QString &serverId,
                             const QString &seriesId);
    int countSeriesBindings(const QString &serverId,
                            const QString &seriesId) const;
    QList<int> listBoundEpisodeNumbers(const QString &serverId,
                                       const QString &seriesId) const;

    void clearCache();

private:
    struct ProviderSearchOutcome {
        DanmakuProviderConfig config;
        QList<DanmakuMatchCandidate> candidates;
        QString errorMessage;
    };

    bool autoMatchEnabled(const QString &serverId) const;
    QList<DanmakuProviderConfig> enabledProviderConfigs(
        QString serverId = QString()) const;
    DanmakuProviderConfig providerConfigForCandidate(
        QString serverId,
        const DanmakuMatchCandidate &candidate) const;
    QCoro::Task<QList<DanmakuMatchCandidate>> searchCandidatesForConfig(
        DanmakuMediaContext context,
        DanmakuProviderConfig config,
        QString manualKeyword = QString());
    QCoro::Task<QList<ProviderSearchOutcome>> searchProvidersInParallel(
        DanmakuMediaContext context,
        QList<DanmakuProviderConfig> configs,
        QString manualKeyword = QString());
    QCoro::Task<ProviderSearchOutcome> searchProviderSafely(
        DanmakuMediaContext context,
        DanmakuProviderConfig config,
        QString manualKeyword = QString());
    QString assCacheKey(const DanmakuMatchCandidate &candidate,
                        const DanmakuRenderOptions &options) const;

    // Collapse flat episode-level candidates (one row per episode, as
    // returned by dandanplay / danmu_api) into series-level groups keyed
    // by (provider, animeTitle, seasonNumber). Bilibili already returns
    // series-level candidates with embedded episodes[] and pass through
    // unchanged. The two-stage SeriesDanmakuMatchDialog wants one row per
    // (work, provider, season) so multi-season works split naturally.
    QList<DanmakuMatchCandidate> aggregateEpisodesToSeries(
        const QList<DanmakuMatchCandidate> &flatCandidates) const;

    NetworkManager *m_networkManager;
    ServerManager *m_serverManager;
    DandanplayProvider *m_dandanplayProvider;
    DanmuApiProvider *m_danmuApiProvider;
    BiliBiliDanmakuProvider *m_bilibiliProvider;
    DanmakuCacheStore *m_cacheStore;
};

#endif 
