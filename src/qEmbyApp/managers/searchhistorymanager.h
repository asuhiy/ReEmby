#ifndef SEARCHHISTORYMANAGER_H
#define SEARCHHISTORYMANAGER_H

#include <QObject>
#include <QHash>
#include <QList>
#include <QSet>
#include <QStringList>

class SearchHistoryManager : public QObject {
    Q_OBJECT
public:
    struct SearchHistoryEntry {
        QString term;
        QString normalizedTerm;
        int searchCount = 0;
        qint64 lastSearchedAtMs = 0;
    };

    static SearchHistoryManager *instance();

    // 聚合搜索历史的保留 bucket：与单服务器历史（按 serverId 分桶）隔离，
    // 所有服务器共享一份（跨服务器搜索的关键词记录）。
    static QString aggregatedBucket() { return QStringLiteral("__aggregated__"); }

    bool isEnabled() const;
    bool isAutocompleteEnabled() const;
    void recordSearch(const QString &serverId, QString query);
    void clearHistory(const QString &serverId);
    void clearAllHistory();
    void removeHistoryTerm(const QString &serverId, QString term);

    QStringList completionSuggestions(const QString &serverId,
                                      const QString &prefix = QString(),
                                      int limit = 8) const;
    QList<SearchHistoryEntry> historyEntries(const QString &serverId,
                                             const QString &prefix = QString()) const;
    QStringList recentSearches(const QString &serverId, int limit = 6,
                               const QString &prefix = QString()) const;
    QStringList frequentSearches(const QString &serverId, int limit = 6,
                                 const QString &prefix = QString(),
                                 const QStringList &excludeTerms = {}) const;

Q_SIGNALS:
    void historyChanged(const QString &serverId);
    void enabledChanged(bool enabled);
    void autocompleteEnabledChanged(bool enabled);

private:
    explicit SearchHistoryManager(QObject *parent = nullptr);

    QString effectiveServerBucket(const QString &serverId) const;
    QString historyConfigKey(const QString &serverId) const;
    QStringList allHistoryBuckets() const;

    void ensureLoaded(const QString &serverId) const;
    void saveEntries(const QString &serverId,
                     const QList<SearchHistoryEntry> &entries);
    QList<SearchHistoryEntry> filteredEntries(const QString &serverId,
                                              const QString &prefix) const;

    static QString normalizeQuery(QString query);

    // 一次性迁移：旧 per-server 桶（search/<serverId>/history_records）
    // → 共享 __global__ 桶。44fc82e 切换桶 key 但旧数据没迁移会卡在旧
    // key 导致 popup 显示空。构造时自动调用一次。
    void migrateLegacyPerServerBuckets();

    mutable QHash<QString, QList<SearchHistoryEntry>> m_cache;
    mutable QSet<QString> m_loadedBuckets;
};

#endif 
