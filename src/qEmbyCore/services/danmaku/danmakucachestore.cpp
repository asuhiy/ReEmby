#include "danmakucachestore.h"

#include <algorithm>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <utils/apppaths.h>

namespace {

QJsonObject episodeToJson(const DanmakuEpisode &episode)
{
    QJsonObject obj;
    obj["episodeNumber"] = episode.episodeNumber;
    obj["cid"] = episode.cid;
    obj["title"] = episode.title;
    obj["longTitle"] = episode.longTitle;
    obj["seasonName"] = episode.seasonName;
    obj["durationMs"] = QString::number(episode.durationMs);
    return obj;
}

DanmakuEpisode episodeFromJson(const QJsonObject &obj)
{
    DanmakuEpisode episode;
    episode.episodeNumber = obj["episodeNumber"].toInt(-1);
    episode.cid = obj["cid"].toString();
    episode.title = obj["title"].toString();
    episode.longTitle = obj["longTitle"].toString();
    episode.seasonName = obj["seasonName"].toString();
    episode.durationMs = obj["durationMs"].toVariant().toLongLong();
    return episode;
}

QJsonObject candidateToJson(const DanmakuMatchCandidate &candidate)
{
    QJsonObject obj;
    obj["provider"] = candidate.provider;
    obj["cacheScope"] = candidate.cacheScope;
    obj["endpointId"] = candidate.endpointId;
    obj["endpointName"] = candidate.endpointName;
    obj["targetId"] = candidate.targetId;
    obj["title"] = candidate.title;
    obj["subtitle"] = candidate.subtitle;
    obj["seasonNumber"] = candidate.seasonNumber;
    obj["episodeNumber"] = candidate.episodeNumber;
    obj["durationMs"] = QString::number(candidate.durationMs);
    obj["score"] = candidate.score;
    obj["matchReason"] = candidate.matchReason;
    obj["commentCount"] = candidate.commentCount;
    // Series-level candidates carry the full per-episode list (cid,
    // longTitle, ...) so fetchComments can still resolve an episode cid
    // after a roundtrip through the cache. Without this, isSeries() goes
    // false on reload and the wrong targetId is used as the cid.
    QJsonArray episodesArray;
    for (const DanmakuEpisode &ep : candidate.episodes) {
        episodesArray.append(episodeToJson(ep));
    }
    obj["episodes"] = episodesArray;
    return obj;
}

DanmakuMatchCandidate candidateFromJson(const QJsonObject &obj)
{
    DanmakuMatchCandidate candidate;
    candidate.provider = obj["provider"].toString();
    candidate.cacheScope = obj["cacheScope"].toString();
    candidate.endpointId = obj["endpointId"].toString();
    candidate.endpointName = obj["endpointName"].toString();
    candidate.targetId = obj["targetId"].toString();
    candidate.title = obj["title"].toString();
    candidate.subtitle = obj["subtitle"].toString();
    candidate.seasonNumber = obj["seasonNumber"].toInt(-1);
    candidate.episodeNumber = obj["episodeNumber"].toInt(-1);
    candidate.durationMs = obj["durationMs"].toVariant().toLongLong();
    candidate.score = obj["score"].toDouble();
    candidate.matchReason = obj["matchReason"].toString();
    candidate.commentCount = obj["commentCount"].toInt();
    const QJsonArray episodesArray = obj["episodes"].toArray();
    for (const QJsonValue &value : episodesArray) {
        candidate.episodes.append(episodeFromJson(value.toObject()));
    }
    return candidate;
}

QJsonObject commentToJson(const DanmakuComment &comment)
{
    QJsonObject obj;
    obj["timeMs"] = QString::number(comment.timeMs);
    obj["mode"] = comment.mode;
    obj["color"] = comment.color.name(QColor::HexRgb);
    obj["fontLevel"] = comment.fontLevel;
    obj["sender"] = comment.sender;
    obj["text"] = comment.text;
    obj["createdAt"] = comment.createdAt.toString(Qt::ISODate);
    return obj;
}

DanmakuComment commentFromJson(const QJsonObject &obj)
{
    DanmakuComment comment;
    comment.timeMs = obj["timeMs"].toVariant().toLongLong();
    comment.mode = obj["mode"].toInt(1);
    comment.color = QColor(obj["color"].toString(QStringLiteral("#FFFFFF")));
    comment.fontLevel = obj["fontLevel"].toInt(25);
    comment.sender = obj["sender"].toString();
    comment.text = obj["text"].toString();
    comment.createdAt =
        QDateTime::fromString(obj["createdAt"].toString(), Qt::ISODate);
    return comment;
}

bool ensureParentDir(const QString &path)
{
    return QDir().mkpath(QFileInfo(path).absolutePath());
}

QString commentCacheKey(const QString &provider,
                        const QString &cacheScope,
                        const QString &targetId)
{
    const QByteArray rawKey =
        QStringLiteral("%1|%2|%3").arg(provider, cacheScope, targetId).toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(rawKey, QCryptographicHash::Sha1).toHex());
}

} 

bool DanmakuCacheStore::loadMatch(const DanmakuMediaContext &context,
                                  DanmakuMatchCandidate *candidate,
                                  bool *manualOverride,
                                  int automaticMaxAgeHours) const
{
    if (!candidate) {
        return false;
    }

    QJsonObject entry;
    bool loadedLegacyEntry = false;
    QFile entryFile(matchEntryFilePath(context));
    if (entryFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        entry = QJsonDocument::fromJson(entryFile.readAll()).object();
    } else {
        
        QFile legacyFile(matchesFilePath(context.serverId));
        if (!legacyFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return false;
        }
        const QJsonObject legacyRoot =
            QJsonDocument::fromJson(legacyFile.readAll()).object();
        entry = legacyRoot.value(context.cacheKey()).toObject();
        loadedLegacyEntry = !entry.isEmpty();
    }
    if (entry.isEmpty()) {
        return false;
    }

    const bool isManual = entry.value("manualOverride").toBool(false);
    const qint64 cachedMediaFileSize =
        entry.value("mediaFileSize").toVariant().toLongLong();
    const QDateTime cachedMediaModifiedAt = QDateTime::fromString(
        entry.value("mediaModifiedAt").toString(), Qt::ISODate);
    if (context.fileSize > 0 && cachedMediaFileSize > 0 &&
        context.fileSize != cachedMediaFileSize) {
        return false;
    }
    if (context.mediaModifiedAt.isValid() && cachedMediaModifiedAt.isValid() &&
        context.mediaModifiedAt != cachedMediaModifiedAt) {
        return false;
    }
    if (!isManual) {
        const int algorithmVersion = entry.value("algorithmVersion").toInt(0);
        const QDateTime updatedAt = QDateTime::fromString(
            entry.value("updatedAt").toString(), Qt::ISODate);
        const qint64 maximumAgeSeconds =
            static_cast<qint64>(automaticMaxAgeHours) * 3600;
        if (algorithmVersion != CurrentMatchAlgorithmVersion ||
            !updatedAt.isValid() || automaticMaxAgeHours <= 0 ||
            updatedAt.secsTo(QDateTime::currentDateTimeUtc()) >
                maximumAgeSeconds) {
            return false;
        }
    }

    *candidate = candidateFromJson(entry.value("candidate").toObject());
    if (manualOverride) {
        *manualOverride = isManual;
    }
    const bool valid = candidate->isValid();
    if (valid && loadedLegacyEntry && isManual) {
        saveMatch(context, *candidate, true);
    }
    return valid;
}

void DanmakuCacheStore::saveMatch(const DanmakuMediaContext &context,
                                  const DanmakuMatchCandidate &candidate,
                                  bool manualOverride) const
{
    const QString filePath = matchEntryFilePath(context);
    ensureParentDir(filePath);
    QJsonObject entry;
    entry["candidate"] = candidateToJson(candidate);
    entry["manualOverride"] = manualOverride;
    entry["algorithmVersion"] = CurrentMatchAlgorithmVersion;
    entry["mediaFileSize"] = QString::number(context.fileSize);
    entry["mediaModifiedAt"] = context.mediaModifiedAt.toString(Qt::ISODate);
    entry["updatedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    QSaveFile file(filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write(QJsonDocument(entry).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

void DanmakuCacheStore::removeMatch(const DanmakuMediaContext &context) const
{
    const QString filePath = matchEntryFilePath(context);
    if (!filePath.isEmpty() && QFile::exists(filePath) &&
        !QFile::remove(filePath)) {
        qWarning().noquote()
            << "[Danmaku][Cache] Failed to remove match entry"
            << "| mediaId:" << context.mediaId
            << "| sourceId:" << context.mediaSourceId;
    }
}

QString seriesBindingDirPath(const QString &baseDir,
                             const QString &serverId,
                             const QString &seriesId)
{
    const QByteArray rawKey =
        QStringLiteral("%1|%2").arg(serverId, seriesId).toUtf8();
    const QString hashedKey = QString::fromLatin1(
        QCryptographicHash::hash(rawKey, QCryptographicHash::Sha1).toHex());
    return baseDir + QStringLiteral("/series-bindings/%1/%2").arg(serverId, hashedKey);
}

QString seriesBindingFilePath(const QString &baseDir,
                              const QString &serverId,
                              const QString &seriesId,
                              int seasonNumber,
                              int episodeNumber)
{
    return seriesBindingDirPath(baseDir, serverId, seriesId) +
           QStringLiteral("/s%1e%2.json")
               .arg(seasonNumber >= 0 ? seasonNumber : 0)
               .arg(episodeNumber > 0 ? episodeNumber : 0);
}

void DanmakuCacheStore::saveSeriesBinding(const QString &serverId,
                                          const QString &seriesId,
                                          int seasonNumber,
                                          int episodeNumber,
                                          const DanmakuMatchCandidate &candidate) const
{
    if (serverId.isEmpty() || seriesId.isEmpty() || episodeNumber <= 0 ||
        !candidate.isValid()) {
        return;
    }
    const QString filePath = seriesBindingFilePath(baseDirPath(), serverId,
                                                  seriesId, seasonNumber,
                                                  episodeNumber);
    if (!ensureParentDir(filePath)) {
        return;
    }
    QJsonObject entry;
    entry["candidate"] = candidateToJson(candidate);
    entry["serverId"] = serverId;
    entry["seriesId"] = seriesId;
    entry["seasonNumber"] = seasonNumber;
    entry["episodeNumber"] = episodeNumber;
    entry["manualOverride"] = true;
    entry["updatedAt"] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    QSaveFile file(filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write(QJsonDocument(entry).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

bool DanmakuCacheStore::loadSeriesBinding(const QString &serverId,
                                          const QString &seriesId,
                                          int seasonNumber,
                                          int episodeNumber,
                                          DanmakuMatchCandidate *candidate) const
{
    if (serverId.isEmpty() || seriesId.isEmpty() || episodeNumber <= 0 ||
        !candidate) {
        return false;
    }
    const QString filePath = seriesBindingFilePath(baseDirPath(), serverId,
                                                  seriesId, seasonNumber,
                                                  episodeNumber);
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    const QJsonObject entry = QJsonDocument::fromJson(file.readAll()).object();
    if (entry.isEmpty()) {
        return false;
    }
    *candidate = candidateFromJson(entry.value("candidate").toObject());
    return candidate->isValid();
}

void DanmakuCacheStore::clearSeriesBindings(const QString &serverId,
                                            const QString &seriesId) const
{
    if (serverId.isEmpty() || seriesId.isEmpty()) {
        return;
    }
    const QString dir =
        seriesBindingDirPath(baseDirPath(), serverId, seriesId);
    QDir d(dir);
    if (d.exists()) {
        d.removeRecursively();
    }
}

int DanmakuCacheStore::countSeriesBindings(const QString &serverId,
                                           const QString &seriesId) const
{
    if (serverId.isEmpty() || seriesId.isEmpty()) {
        return 0;
    }
    const QString dir =
        seriesBindingDirPath(baseDirPath(), serverId, seriesId);
    QDir d(dir);
    if (!d.exists()) {
        return 0;
    }
    const QFileInfoList files =
        d.entryInfoList(QStringList{"s*e*.json"}, QDir::Files);
    return files.size();
}

QList<int> DanmakuCacheStore::listBoundEpisodeNumbers(const QString &serverId,
                                                      const QString &seriesId) const
{
    QList<int> episodes;
    if (serverId.isEmpty() || seriesId.isEmpty()) {
        return episodes;
    }
    const QString dir =
        seriesBindingDirPath(baseDirPath(), serverId, seriesId);
    QDir d(dir);
    if (!d.exists()) {
        return episodes;
    }
    const QFileInfoList files =
        d.entryInfoList(QStringList{"s*e*.json"}, QDir::Files);
    // Filename pattern: s{season}e{episode}.json. We only need the episode
    // number to feed back into the matcher.
    QRegularExpression re(QStringLiteral("^s\\d+e(-?\\d+)\\.json$"));
    for (const QFileInfo &info : files) {
        const auto match = re.match(info.fileName());
        if (match.hasMatch()) {
            const int ep = match.captured(1).toInt();
            if (ep > 0) {
                episodes.append(ep);
            }
        }
    }
    std::sort(episodes.begin(), episodes.end());
    return episodes;
}

bool DanmakuCacheStore::loadFingerprint(const DanmakuMediaContext &context,
                                        QString *fileHash,
                                        int maxAgeHours) const
{
    if (!fileHash || context.fileSize <= 0) {
        return false;
    }
    QFile file(fingerprintEntryFilePath(context));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    const QJsonObject entry = QJsonDocument::fromJson(file.readAll()).object();
    const QDateTime updatedAt = QDateTime::fromString(
        entry.value("updatedAt").toString(), Qt::ISODate);
    const qint64 cachedSize =
        entry.value("fileSize").toVariant().toLongLong();
    const QString cachedHash = entry.value("fileHash").toString().trimmed();
    const QDateTime cachedMediaModifiedAt = QDateTime::fromString(
        entry.value("mediaModifiedAt").toString(), Qt::ISODate);
    if (cachedSize != context.fileSize || cachedHash.size() != 32 ||
        !updatedAt.isValid() || maxAgeHours <= 0 ||
        updatedAt.secsTo(QDateTime::currentDateTimeUtc()) >
            static_cast<qint64>(maxAgeHours) * 3600) {
        return false;
    }
    if (context.mediaModifiedAt.isValid() && cachedMediaModifiedAt.isValid() &&
        context.mediaModifiedAt != cachedMediaModifiedAt) {
        return false;
    }
    *fileHash = cachedHash;
    return true;
}

void DanmakuCacheStore::saveFingerprint(const DanmakuMediaContext &context,
                                        const QString &fileHash) const
{
    const QString normalizedHash = fileHash.trimmed().toLower();
    if (context.fileSize <= 0 || normalizedHash.size() != 32) {
        return;
    }
    const QString filePath = fingerprintEntryFilePath(context);
    ensureParentDir(filePath);
    QJsonObject entry;
    entry["fileSize"] = QString::number(context.fileSize);
    entry["fileHash"] = normalizedHash;
    entry["updatedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    entry["mediaModifiedAt"] = context.mediaModifiedAt.toString(Qt::ISODate);
    QSaveFile file(filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write(QJsonDocument(entry).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

QList<DanmakuComment> DanmakuCacheStore::loadComments(const QString &provider,
                                                      const QString &cacheScope,
                                                      const QString &targetId,
                                                      int maxAgeHours) const
{
    QList<DanmakuComment> comments;
    QFile file(commentsFilePath(provider, cacheScope, targetId));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return comments;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    const QJsonObject root = doc.object();
    const QDateTime fetchedAt = QDateTime::fromString(
        root.value("fetchedAt").toString(), Qt::ISODate);
    if (!fetchedAt.isValid() ||
        fetchedAt.secsTo(QDateTime::currentDateTimeUtc()) >
            static_cast<qint64>(maxAgeHours) * 3600) {
        return comments;
    }

    const QJsonArray arr = root.value("comments").toArray();
    comments.reserve(arr.size());
    for (const QJsonValue &value : arr) {
        const DanmakuComment comment = commentFromJson(value.toObject());
        if (comment.isValid()) {
            comments.append(comment);
        }
    }
    return comments;
}

void DanmakuCacheStore::saveComments(const QString &provider,
                                     const QString &cacheScope,
                                     const QString &targetId,
                                     const QString &sourceTitle,
                                     const QList<DanmakuComment> &comments) const
{
    const QString filePath = commentsFilePath(provider, cacheScope, targetId);
    ensureParentDir(filePath);

    QJsonArray arr;
    for (const DanmakuComment &comment : comments) {
        arr.append(commentToJson(comment));
    }

    QJsonObject root;
    root["provider"] = provider;
    root["cacheScope"] = cacheScope;
    root["targetId"] = targetId;
    root["sourceTitle"] = sourceTitle;
    root["fetchedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    root["comments"] = arr;

    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    }
}

bool DanmakuCacheStore::loadAssPath(const QString &assKey,
                                    QString *path,
                                    int maxAgeHours) const
{
    if (!path) {
        return false;
    }

    const QString filePath = assFilePath(assKey);
    QFileInfo info(filePath);
    if (!info.exists()) {
        return false;
    }

    const qint64 ageSeconds = info.lastModified().secsTo(QDateTime::currentDateTime());
    if (ageSeconds > static_cast<qint64>(maxAgeHours) * 3600) {
        return false;
    }

    *path = filePath;
    return true;
}

QString DanmakuCacheStore::saveAssFile(const QString &assKey,
                                       const QString &content) const
{
    const QString filePath = assFilePath(assKey);
    if (!ensureParentDir(filePath)) {
        return {};
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return {};
    }

    file.write(content.toUtf8());
    return filePath;
}

void DanmakuCacheStore::clearAll() const
{
    QDir dir(baseDirPath());
    if (dir.exists()) {
        dir.removeRecursively();
    }
}

QString DanmakuCacheStore::baseDirPath() const
{
    return AppPaths::dataRoot() + QStringLiteral("/danmaku");
}

QString DanmakuCacheStore::matchesFilePath(const QString &serverId) const
{
    return baseDirPath() + QStringLiteral("/matches/%1.json").arg(serverId);
}

QString DanmakuCacheStore::matchEntryFilePath(
    const DanmakuMediaContext &context) const
{
    const QByteArray rawKey =
        QStringLiteral("%1|%2").arg(context.serverId, context.cacheKey()).toUtf8();
    const QString hashedKey = QString::fromLatin1(
        QCryptographicHash::hash(rawKey, QCryptographicHash::Sha1).toHex());
    return baseDirPath() +
           QStringLiteral("/matches-v2/%1/%2.json")
               .arg(context.serverId, hashedKey);
}

QString DanmakuCacheStore::fingerprintEntryFilePath(
    const DanmakuMediaContext &context) const
{
    const QByteArray rawKey =
        QStringLiteral("%1|%2").arg(context.serverId, context.cacheKey()).toUtf8();
    const QString hashedKey = QString::fromLatin1(
        QCryptographicHash::hash(rawKey, QCryptographicHash::Sha1).toHex());
    return baseDirPath() +
           QStringLiteral("/fingerprints/%1/%2.json")
               .arg(context.serverId, hashedKey);
}

QString DanmakuCacheStore::commentsFilePath(const QString &provider,
                                            const QString &cacheScope,
                                            const QString &targetId) const
{
    const QString hashedKey = commentCacheKey(provider, cacheScope, targetId);
    return baseDirPath() +
           QStringLiteral("/comments/%1/%2.json").arg(provider, hashedKey);
}

QString DanmakuCacheStore::assFilePath(const QString &assKey) const
{
    return baseDirPath() + QStringLiteral("/ass/%1.ass").arg(assKey);
}
