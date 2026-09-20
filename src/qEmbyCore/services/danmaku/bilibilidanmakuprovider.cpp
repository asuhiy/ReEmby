#include "bilibilidanmakuprovider.h"

#include "bilibiliauthservice.h"

#include <QCryptographicHash>
#include <QColor>
#include <QDateTime>
#include <QDebug>
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <zlib.h>

namespace {

constexpr int kRequestTimeoutMs = 20000;
constexpr int kMaxSearchCandidates = 5;

constexpr char kUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";
constexpr char kReferer[] = "https://www.bilibili.com/";

// WBI mixin key index table (stable across Bilibili web revisions).
constexpr int kWbiMixinKeyEncTab[64] = {
    46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35,
    27, 43, 5, 49, 33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
    37, 48, 7, 16, 24, 55, 40, 61, 26, 17, 0, 1, 60, 51, 30, 4,
    22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36, 20, 34, 44, 52};

NetworkRequestOptions requestOptions()
{
    NetworkRequestOptions options;
    options.timeoutMs = kRequestTimeoutMs;
    return options;
}

QMap<QString, QString> requestHeaders(const QString &cookie)
{
    QMap<QString, QString> headers;
    headers.insert(QStringLiteral("User-Agent"), QString::fromLatin1(kUserAgent));
    headers.insert(QStringLiteral("Referer"), QString::fromLatin1(kReferer));
    // 强制 identity：B 站 pgc 接口对 QNAM 的 gzip 自动解压有时返回空 body
    // （1.6MB 响应），明确要求不压缩可稳定拿到完整 JSON。
    headers.insert(QStringLiteral("Accept-Encoding"),
                   QStringLiteral("identity"));
    if (!cookie.trimmed().isEmpty()) {
        headers.insert(QStringLiteral("Cookie"), cookie.trimmed());
    }
    return headers;
}

QString stripHtmlTags(QString text)
{
    text.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    return text.trimmed();
}

QString normalizedTitle(QString value)
{
    value = value.trimmed().toLower();
    value.remove(QRegularExpression(QStringLiteral(R"([^\p{L}\p{N}]+)")));
    return value;
}

double titleScore(const QString &lhs, const QString &rhs)
{
    const QString left = normalizedTitle(lhs);
    const QString right = normalizedTitle(rhs);
    if (left.isEmpty() || right.isEmpty()) {
        return 0.0;
    }
    if (left == right) {
        return 1.0;
    }
    if (left.contains(right) || right.contains(left)) {
        return 0.82;
    }
    if (left.size() < 2 || right.size() < 2) {
        return 0.0;
    }
    QSet<QString> leftPairs;
    QSet<QString> rightPairs;
    for (int i = 0; i + 1 < left.size(); ++i) {
        leftPairs.insert(left.mid(i, 2));
    }
    for (int i = 0; i + 1 < right.size(); ++i) {
        rightPairs.insert(right.mid(i, 2));
    }
    int common = 0;
    for (const QString &pair : std::as_const(leftPairs)) {
        if (rightPairs.contains(pair)) {
            ++common;
        }
    }
    return (2.0 * common) / (leftPairs.size() + rightPairs.size());
}

int extractEpisodeNumber(const QString &text)
{
    static const QList<QRegularExpression> patterns = {
        QRegularExpression(
            QStringLiteral(R"(第\s*0*(\d{1,4})\s*[话話集期回])")),
        QRegularExpression(
            QStringLiteral(R"((?:^|[^A-Za-z0-9])(?:EP?|Episode)\s*[._-]?\s*0*(\d{1,4})(?:[^A-Za-z0-9]|$))"),
            QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(
            QStringLiteral(R"((?:^|[^A-Za-z0-9])0*(\d{1,4})\s*[话話集期回](?:[^A-Za-z0-9]|$))"))};
    for (const QRegularExpression &pattern : patterns) {
        const QRegularExpressionMatch match = pattern.match(text);
        if (match.hasMatch()) {
            return match.captured(1).toInt();
        }
    }
    return 0;
}

QString mixinKey(const QString &imgKey, const QString &subKey)
{
    const QString raw = imgKey + subKey;
    QString key;
    key.reserve(32);
    for (int idx : kWbiMixinKeyEncTab) {
        if (idx < raw.size()) {
            key.append(raw.at(idx));
        }
    }
    return key.left(32);
}

// Builds a w_rid signature for the given params (see Bilibili WBI signing).
QMap<QString, QString> wbiSign(QMap<QString, QString> params,
                               const QString &imgKey, const QString &subKey)
{
    params.insert(QStringLiteral("wts"),
                  QString::number(QDateTime::currentSecsSinceEpoch()));
    QStringList keys = params.keys();
    std::sort(keys.begin(), keys.end());

    QUrlQuery query;
    for (const QString &key : std::as_const(keys)) {
        QString value = params.value(key);
        value.remove(QRegularExpression(QStringLiteral("[!'()*]")));
        query.addQueryItem(key, value);
    }
    QString encoded = query.toString(QUrl::FullyEncoded);
    const QString toHash = encoded + mixinKey(imgKey, subKey);
    const QString wRid = QString::fromLatin1(
        QCryptographicHash::hash(toHash.toUtf8(), QCryptographicHash::Md5).toHex());
    params.insert(QStringLiteral("w_rid"), wRid);
    return params;
}

struct WbiKeys {
    QString imgKey;
    QString subKey;
};

QCoro::Task<WbiKeys> fetchWbiKeys(NetworkManager *networkManager,
                                  const QString &cookie)
{
    WbiKeys keys;
    const QJsonObject response = co_await networkManager->get(
        QStringLiteral("https://api.bilibili.com/x/web-interface/nav"),
        requestHeaders(cookie), requestOptions());
    const QJsonObject wbi = response.value(QStringLiteral("data"))
                                .toObject()
                                .value(QStringLiteral("wbi_img"))
                                .toObject();
    const auto keyFromUrl = [](const QString &url) {
        return url.section('/', -1).section('.', 0, 0);
    };
    keys.imgKey = keyFromUrl(wbi.value(QStringLiteral("img_url")).toString());
    keys.subKey = keyFromUrl(wbi.value(QStringLiteral("sub_url")).toString());
    co_return keys;
}

// Inflates the raw-deflate payload Bilibili serves for comment.xml. The bytes
// are wrapped into a full zlib stream (constant header + adler32) so that
// Bilibili serves the comment payload as raw deflate (no zlib header, no
// trailing adler32). qUncompress can't decode it because its adler32
// verification rejects any stream where the trailer is computed on the
// compressed payload rather than the decompressed one. Use zlib directly
// in -15 (raw deflate) mode and let the caller decide what to do with the
// trailing bytes.
QByteArray rawDeflateInflate(const QByteArray &raw)
{
    if (raw.isEmpty()) {
        return {};
    }
    z_stream stream;
    std::memset(&stream, 0, sizeof(stream));
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(raw.constData()));
    stream.avail_in = static_cast<uInt>(raw.size());
    if (inflateInit2(&stream, -15) != Z_OK) {
        return {};
    }
    QByteArray out;
    out.reserve(raw.size() * 4);
    char buffer[8192];
    int ret = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef *>(buffer);
        stream.avail_out = sizeof(buffer);
        ret = inflate(&stream, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&stream);
            return {};
        }
        out.append(buffer, sizeof(buffer) - stream.avail_out);
    } while (ret == Z_OK);
    inflateEnd(&stream);
    return out;
}

// Parses a JSON payload. The primary path requests Accept-Encoding: identity
// so Bilibili sends plain JSON; this helper only needs to handle the case
// where the server ignored that header and returned raw gzip bytes anyway.
QJsonObject parseJsonResponse(const QByteArray &raw)
{
    QByteArray payload = raw;
    if (payload.size() > 18 && payload.startsWith('\x1f') &&
        static_cast<quint8>(payload.at(1)) == 0x8b) {
        // gzip: 10-byte header + deflate + 8-byte trailer (CRC32+ISIZE).
        // qUncompress needs a zlib stream; wrap the deflate part with a
        // constant zlib header + computed adler32.
        const QByteArray deflated = payload.mid(10, payload.size() - 18);
        QByteArray stream;
        stream.reserve(deflated.size() + 6);
        stream.append('\x78');
        stream.append('\x9c');
        stream.append(deflated);
        quint32 a = 1;
        quint32 b = 0;
        for (const char c : deflated) {
            a = (a + static_cast<quint8>(c)) % 65521;
            b = (b + a) % 65521;
        }
        const quint32 adler = (b << 16) | a;
        stream.append(static_cast<char>((adler >> 24) & 0xff));
        stream.append(static_cast<char>((adler >> 16) & 0xff));
        stream.append(static_cast<char>((adler >> 8) & 0xff));
        stream.append(static_cast<char>(adler & 0xff));
        payload = qUncompress(stream);
        if (payload.isEmpty()) {
            qWarning() << "[BiliBili] gzip payload inflate failed"
                       << "| raw size:" << raw.size();
        }
    }
    QJsonParseError parseError;
    const QJsonDocument doc =
        QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        qWarning() << "[BiliBili] season JSON parse failed"
                   << "| size:" << payload.size()
                   << "| error:" << parseError.errorString()
                   << "| head:" << QString::fromUtf8(payload.left(120));
        return QJsonObject();
    }
    return doc.object();
}

QList<DanmakuComment> parseDanmakuXml(const QByteArray &xml)
{
    QList<DanmakuComment> comments;
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == QLatin1String("d")) {
            const QString p =
                reader.attributes().value(QStringLiteral("p")).toString();
            const QStringList parts = p.split(',', Qt::KeepEmptyParts);
            if (parts.size() < 4) {
                continue;
            }
            const int mode = parts.at(1).toInt();
            // 1=scroll, 4=bottom, 5=top. Drop reverse (6) and advanced (7/8)
            // which the ASS composer cannot render.
            if (mode != 1 && mode != 4 && mode != 5) {
                continue;
            }
            DanmakuComment comment;
            comment.timeMs =
                static_cast<qint64>(parts.at(0).toDouble() * 1000.0);
            comment.mode = mode;
            comment.fontLevel = parts.at(2).toInt();
            comment.color = QColor::fromRgb(parts.at(3).toUInt());
            comment.text = reader.readElementText();
            if (comment.isValid()) {
                comments.append(comment);
            }
        }
    }
    return comments;
}

} // namespace

BiliBiliDanmakuProvider::BiliBiliDanmakuProvider(NetworkManager *networkManager)
    : m_networkManager(networkManager)
{
}

// In the long_title returned by /pgc/view/web/season, the season name is
// followed by the episode number, optionally with a trailing suffix such as
// "重制版". The episode number is the FIRST run of consecutive digits, so a
// linear scan from the left finds it even when a suffix follows. Examples:
//   "凡人风起天南1重制版" -> ("凡人风起天南", 1)
//   "魔道争锋9"          -> ("魔道争锋", 9)
//   "星海飞驰25"         -> ("星海飞驰", 25)
//   "慕兰之战14"         -> ("慕兰之战", 14)
//   "特别篇"            -> ("特别篇", 0)
QPair<QString, int> parseLongTitle(const QString &longTitle)
{
    const QString text = longTitle.trimmed();
    if (text.isEmpty()) {
        return {QString(), 0};
    }
    // Find the first run of digits (the episode number).
    int digitStart = -1;
    int digitEnd = -1;
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i).isDigit()) {
            if (digitStart < 0) {
                digitStart = i;
            }
            digitEnd = i;
        } else if (digitStart >= 0) {
            break;
        }
    }
    if (digitStart < 0) {
        return {text, 0};
    }
    QString seasonName = text.left(digitStart).trimmed();
    int episodeInSeason =
        text.mid(digitStart, digitEnd - digitStart + 1).toInt();
    return {seasonName, episodeInSeason};
}

QCoro::Task<QList<DanmakuMatchCandidate>> BiliBiliDanmakuProvider::searchCandidates(
    DanmakuMediaContext context,
    DanmakuProviderConfig config,
    QString manualKeyword) const
{
    QList<DanmakuMatchCandidate> candidates;
    BiliBiliAuthService *auth = BiliBiliAuthService::instance();
    if (!m_networkManager) {
        qWarning() << "[BiliBili] search aborted: no network manager";
        co_return candidates;
    }
    if (!auth->isLoggedIn()) {
        qWarning() << "[BiliBili] search aborted: not logged in"
                   << "| (SESSDATA missing in ConfigStore)";
        co_return candidates;
    }

    co_await auth->ensureBuvid3();
    const QString cookie = auth->cookieHeader();

    const QString querySubject =
        manualKeyword.trimmed().isEmpty()
            ? (context.isEpisode() ? context.seriesName.trimmed()
                                   : context.title.trimmed())
            : manualKeyword.trimmed();
    qDebug().noquote() << "[BiliBili] search begin"
                       << "| querySubject:" << querySubject
                       << "| isEpisode:" << context.isEpisode()
                       << "| episodeNumber:" << context.episodeNumber
                       << "| seriesName:" << context.seriesName
                       << "| manualKeyword:" << manualKeyword.trimmed()
                       << "| cookie length:" << cookie.size();
    if (querySubject.isEmpty()) {
        qWarning() << "[BiliBili] search aborted: empty querySubject"
                   << "| context.title:" << context.title
                   << "| context.seriesName:" << context.seriesName;
        co_return candidates;
    }

    const WbiKeys wbi = co_await fetchWbiKeys(m_networkManager, cookie);
    if (wbi.imgKey.isEmpty() || wbi.subKey.isEmpty()) {
        qWarning() << "[BiliBili] search aborted: WBI keys unavailable"
                   << "| imgKey empty:" << wbi.imgKey.isEmpty()
                   << "| subKey empty:" << wbi.subKey.isEmpty();
        co_return candidates;
    }

    // 1) media_bangumi search -> { season_id, media_id, title (with <em> tags) }.
    // This surfaces the official bangumi entry instead of UGC uploads that
    // /search/type?search_type=video would return.
    const QMap<QString, QString> searchParams = wbiSign(
        {{QStringLiteral("search_type"), QStringLiteral("media_bangumi")},
         {QStringLiteral("keyword"), querySubject},
         {QStringLiteral("page"), QStringLiteral("1")},
         {QStringLiteral("page_size"), QStringLiteral("5")}},
        wbi.imgKey, wbi.subKey);

    QUrl searchUrl(QStringLiteral("https://api.bilibili.com/x/web-interface/wbi/search/type"));
    QUrlQuery query;
    for (auto it = searchParams.cbegin(); it != searchParams.cend(); ++it) {
        query.addQueryItem(it.key(), it.value());
    }
    searchUrl.setQuery(query);

    QJsonObject searchResponse;
    try {
        searchResponse = co_await m_networkManager->get(
            searchUrl.toString(), requestHeaders(cookie), requestOptions());
    } catch (const std::exception &e) {
        qWarning().noquote() << "[BiliBili] media_bangumi search failed (network)"
                             << "| error:" << e.what()
                             << "| url:" << searchUrl.toString().left(160);
        co_return candidates;
    }
    const int searchCode = searchResponse.value(QStringLiteral("code")).toInt(-999);
    const QJsonArray results =
        searchResponse.value(QStringLiteral("data"))
            .toObject()
            .value(QStringLiteral("result"))
            .toArray();
    qDebug().noquote() << "[BiliBili] media_bangumi search response"
                       << "| top-level code:" << searchCode
                       << "| result count:" << results.size();
    if (results.isEmpty()) {
        qDebug().noquote() << "[BiliBili] media_bangumi search returned no results"
                           << "| keyword:" << querySubject
                           << "| top-level code:" << searchCode;
        co_return candidates;
    }

    int resolved = 0;
    for (const QJsonValue &resultValue : results) {
        if (resolved >= kMaxSearchCandidates) {
            break;
        }
        const QJsonObject result = resultValue.toObject();
        const qint64 seasonId =
            result.value(QStringLiteral("season_id")).toVariant().toLongLong();
        const qint64 mediaId =
            result.value(QStringLiteral("media_id")).toVariant().toLongLong();
        const QString seriesTitle = stripHtmlTags(
            result.value(QStringLiteral("title")).toString());
        if (seasonId <= 0 || seriesTitle.isEmpty()) {
            qDebug().noquote() << "[BiliBili] skip search result: missing fields"
                               << "| seasonId:" << seasonId
                               << "| seriesTitle:" << seriesTitle;
            continue;
        }

        // 2) /pgc/view/web/season?season_id=... -> episodes[] (merged across
        //    all sub-seasons for this series). section_type==0 marks the main
        //    episodes; the rest are previews/PVs/extras and are dropped.
        const QString seasonUrl = QStringLiteral(
            "https://api.bilibili.com/pgc/view/web/season?season_id=%1")
            .arg(seasonId);
        QJsonObject seasonResponse;
        try {
            // B站 pgc/view/web/season 对 QNAM 可能返回空 body（gzip 自动解压
            // 异常），这里直接拿原始字节并手动解压，保证 1.6MB 响应能读到。
            const QByteArray rawSeason = co_await m_networkManager->getBytes(
                seasonUrl, requestHeaders(cookie), requestOptions());
            seasonResponse = parseJsonResponse(rawSeason);
        } catch (const std::exception &e) {
            qWarning().noquote() << "[BiliBili] season detail failed (network)"
                                 << "| seasonId:" << seasonId
                                 << "| error:" << e.what();
            continue;
        }
        // The /pgc/view/web/season endpoint returns the season payload under
        // the top-level "result" key (NOT "data" as for /x/web-interface/*).
        // A parent season_id that maps to a multi-season series (e.g. "凡人
        // 修仙传" which has 风起天南/魔道争锋/... under one series page) will
        // return episodes=[] at this level and instead carry the sub-seasons
        // under result.seasons[]. We accept both shapes; the second one is
        // flattened here into the unified episodes array.
        const QJsonArray directEpisodes =
            seasonResponse.value(QStringLiteral("result"))
                .toObject()
                .value(QStringLiteral("episodes"))
                .toArray();
        QJsonArray episodes = directEpisodes;
        if (episodes.isEmpty()) {
            const QJsonArray seasons =
                seasonResponse.value(QStringLiteral("result"))
                    .toObject()
                    .value(QStringLiteral("seasons"))
                    .toArray();
            qDebug().noquote() << "[BiliBili] season expansion"
                               << "| seasonId:" << seasonId
                               << "| direct episodes:" << directEpisodes.size()
                               << "| sub seasons:" << seasons.size();
            for (const QJsonValue &sVal : seasons) {
                const QJsonObject sub = sVal.toObject();
                for (const QJsonValue &epVal :
                     sub.value(QStringLiteral("episodes")).toArray()) {
                    QJsonObject epObj = epVal.toObject();
                    // Inherit section_type=0 from the sub-season scope when the
                    // merged entry lacks it (each sub-season already filters
                    // by section_type, so missing usually means main).
                    if (!epObj.contains(QStringLiteral("section_type"))) {
                        epObj.insert(QStringLiteral("section_type"), 0);
                    }
                    episodes.append(epObj);
                }
            }
        }
        int mainEpisodes = 0;
        for (const QJsonValue &ep : episodes) {
            if (ep.toObject().value(QStringLiteral("section_type")).toInt(-1) == 0) {
                ++mainEpisodes;
            }
        }
        qDebug().noquote() << "[BiliBili] season detail"
                           << "| seasonId:" << seasonId
                           << "| total episodes:" << episodes.size()
                           << "| main (section_type=0):" << mainEpisodes;
        if (episodes.isEmpty()) {
            continue;
        }

        // 3) Aggregate into ONE series-level candidate. The UI renders a
        //    secondary "pick episode" step from candidate.episodes, so the
        //    search result list stays small (one row per series) and the
        //    season data is fetched once per series instead of per episode.
        DanmakuMatchCandidate seriesCandidate;
        seriesCandidate.provider = QStringLiteral("bilibili");
        seriesCandidate.targetId = QString::number(seasonId);
        seriesCandidate.title = seriesTitle;
        seriesCandidate.subtitle = seriesTitle;
        seriesCandidate.matchReason = QStringLiteral("media_bangumi");
        seriesCandidate.commentCount = mainEpisodes;
        // Score: series-title match (seriesTitle already includes the season
        // name for most shows, e.g. "凡人修仙传"), plus a bonus when the
        // queried series name matches the search subject directly.
        seriesCandidate.score = titleScore(querySubject, seriesTitle) * 60.0;
        if (!seriesTitle.trimmed().isEmpty() &&
            !querySubject.trimmed().isEmpty() &&
            normalizedTitle(seriesTitle).contains(normalizedTitle(querySubject))) {
            seriesCandidate.score += 12.0;
        }

        for (const QJsonValue &episodeValue : episodes) {
            const QJsonObject episode = episodeValue.toObject();
            // Drop PV/trailers (B站 section_type=1 are 30~50s previews; they
            // have no usable danmaku and would only confuse the picker). Only
            // section_type=0 entries (the 189 main episodes) are kept.
            if (episode.value(QStringLiteral("section_type")).toInt(-1) != 0) {
                continue;
            }
            const QString cid =
                episode.value(QStringLiteral("cid")).toVariant().toString().trimmed();
            if (cid.isEmpty()) {
                continue;
            }
            const QString longTitle =
                episode.value(QStringLiteral("long_title")).toString().trimmed();
            // Use the post-filter index (1..N) as the candidate's episode
            // number, so the picker renders "1 凡人风起天南1重制版", "2 凡人
            // 风起天南2重制版", ... and resolveMatch aligns Emby's global
            // episode number to the same flat sequence. parseLongTitle's
            // per-season number would reset to 1 at every season boundary.
            const int flatIndex = seriesCandidate.episodes.size() + 1;

            DanmakuEpisode ep;
            ep.episodeNumber = flatIndex;
            ep.cid = cid;
            ep.title = episode.value(QStringLiteral("title")).toString().trimmed();
            ep.longTitle = longTitle.isEmpty() ? ep.title : longTitle;
            ep.seasonName = QString();  // 季名信息已在 longTitle 文本里
            ep.durationMs = static_cast<qint64>(
                episode.value(QStringLiteral("duration")).toVariant().toDouble() * 1000.0);
            seriesCandidate.episodes.append(ep);
        }
        if (!seriesCandidate.episodes.isEmpty()) {
            candidates.append(seriesCandidate);
        }
        ++resolved;
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const DanmakuMatchCandidate &left,
                 const DanmakuMatchCandidate &right) {
                  return left.score > right.score;
              });
    if (candidates.size() > kMaxSearchCandidates) {
        candidates.resize(kMaxSearchCandidates);
    }
    qDebug().noquote() << "[BiliBili] search done"
                       << "| querySubject:" << querySubject
                       << "| series candidates:" << candidates.size();
    co_return candidates;
}

QCoro::Task<QList<DanmakuComment>> BiliBiliDanmakuProvider::fetchComments(
    DanmakuMatchCandidate candidate,
    DanmakuProviderConfig config) const
{
    QList<DanmakuComment> comments;
    BiliBiliAuthService *auth = BiliBiliAuthService::instance();
    if (!m_networkManager || !candidate.isValid() || !auth->isLoggedIn()) {
        co_return comments;
    }

    // Resolve the concrete cid. Two shapes are accepted:
    //   1) episode-level:  targetId == cid  (normal path, saved manual match)
    //   2) series-level:   targetId == season_id, episodeNumber set — pick the
    //      matching episode from the embedded list (auto-match path).
    QString cid = candidate.targetId.trimmed();
    if (candidate.isSeries()) {
        cid.clear();
        for (const DanmakuEpisode &ep : candidate.episodes) {
            if (ep.episodeNumber == candidate.episodeNumber) {
                cid = ep.cid;
                break;
            }
        }
        if (cid.isEmpty() && !candidate.episodes.isEmpty()) {
            cid = candidate.episodes.constFirst().cid;
        }
    }
    if (cid.isEmpty()) {
        qWarning() << "[BiliBili] fetchComments aborted: no cid resolved"
                   << "| targetId:" << candidate.targetId
                   << "| episodeNumber:" << candidate.episodeNumber
                   << "| episodes:" << candidate.episodes.size();
        co_return comments;
    }

    co_await auth->ensureBuvid3();
    const QString cookie = auth->cookieHeader();

    const QByteArray raw = co_await m_networkManager->getBytes(
        QStringLiteral("https://comment.bilibili.com/%1.xml").arg(cid),
        requestHeaders(cookie), requestOptions());

    QByteArray xml = raw;
    // Bilibili serves raw-deflate; inflate it unless QNAM already did.
    if (!raw.trimmed().startsWith('<')) {
        xml = rawDeflateInflate(raw);
    }
    if (xml.isEmpty()) {
        co_return comments;
    }

    comments = parseDanmakuXml(xml);
    qDebug().noquote() << "[BiliBili] Comments fetched"
                       << "| cid:" << cid
                       << "| episodeNumber:" << candidate.episodeNumber
                       << "| count:" << comments.size();
    co_return comments;
}
