#include "mpvhttpstreamrelay.h"

#include "../utils/logredactionutils.h"

#include <QAbstractSocket>
#include <QDebug>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>

#include <algorithm>

namespace
{

constexpr qint64 kReplyReadBufferBytes = 4 * 1024 * 1024;
// Pass-through: the data has to reach mpv anyway, so keep its socket busy.
constexpr qint64 kSocketQueuedBytesHighWater = 4 * 1024 * 1024;
// Cached path: mpv seeks to an offset, reads a few hundred kilobytes and closes
// the connection, tens of times per second for a non-interleaved audio track.
// The high-water mark must be at least that large: otherwise one response needs
// several event-loop round trips (measured: 5), and each round trip costs about
// 1.8 ms because the relay shares ReEmby's main thread with rendering, danmaku
// and UI work. One round trip per connection is the goal, so this is sized to
// swallow a typical request whole.
constexpr qint64 kCacheSocketHighWaterBytes = 2 * 1024 * 1024;
constexpr qint64 kRelayPumpChunkBytes = 1024 * 1024;
// One aggregate log line per this many client connections, so a playing media
// does not produce tens of thousands of lines.
constexpr int kConnectionLogInterval = 200;
constexpr qint64 kFetchChunkBytes = 1024 * 1024;
constexpr qint64 kDefaultReadaheadBytes = 64 * 1024 * 1024;
// Memory-only for now; the design in tools/relay-design.md replaces this with
// a sparse file plus a configurable disk quota.
constexpr qint64 kCacheMemoryLimitBytes = 256 * 1024 * 1024;
// How long a cache block keeps its full access heat before that heat starts
// halving. Only affects eviction priority, never correctness, so the exact
// value is not critical: 30 s is long enough for the audio track of a
// non-interleaved file to stay hot across a video seek, and short enough that
// an early burst of hits cannot pin a block in the cache forever.
constexpr qint64 kHitHalfLifeNs = 30LL * 1000 * 1000 * 1000;
// While a client is consuming data, keep the upstream read this far ahead of
// it instead of having to re-issue a request at every limit boundary.
constexpr qint64 kFetchLimitLowWaterBytes = 8 * 1024 * 1024;
// The Emby -> OpenList -> object-storage chain uses two hops; allow a few more
// before giving up on a redirect loop.
constexpr int kMaxRedirects = 5;
// A stalled hop would otherwise hang the warm-up probe for the whole session,
// leaving it to die silently when the media is closed.
constexpr int kRedirectProbeTimeoutMs = 8000;

bool canWriteToSocket(QTcpSocket *socket)
{
    return socket && socket->isOpen() && socket->isWritable() && socket->state() != QAbstractSocket::UnconnectedState;
}

// Monotonic clock shared by the per-connection diagnostics.
qint64 monotonicNs()
{
    static QElapsedTimer clock;
    if (!clock.isValid())
    {
        clock.start();
    }
    return clock.nsecsElapsed();
}

// Adds the wall time spent inside a scope to a counter. Used to attribute the
// per-connection cost to the calls that actually consume it, without having to
// instrument every return path by hand. Declared after monotonicNs() on
// purpose -- it calls into it.
class ScopeTimer
{
public:
    explicit ScopeTimer(qint64 *accumulator)
        : m_accumulator(accumulator), m_startNs(monotonicNs())
    {
    }
    ~ScopeTimer() { *m_accumulator += monotonicNs() - m_startNs; }
    ScopeTimer(const ScopeTimer &) = delete;
    ScopeTimer &operator=(const ScopeTimer &) = delete;

private:
    qint64 *m_accumulator = nullptr;
    qint64 m_startNs = 0;
};

} // namespace

// The relay is moved onto its own thread right after construction, so whatever
// is created here ends up owned by that thread. QTcpServer and QTimer are fine
// with that; QNetworkAccessManager is the one object Qt insists on creating in
// the thread that will use it, so it is created lazily from prepare() instead.
MpvHttpStreamRelay::MpvHttpStreamRelay(QObject *parent)
    : QObject(parent), m_server(new QTcpServer(this)), m_speedTimer(new QTimer(this))
{
    connect(m_server, &QTcpServer::newConnection, this, [this]() { onNewConnection(); });
    m_speedTimer->setInterval(1000);
    connect(m_speedTimer, &QTimer::timeout, this,
            [this]()
            {
                const qint64 speed = m_bytesRelayedSinceLastTick;
                m_bytesRelayedSinceLastTick = 0;
                Q_EMIT upstreamSpeedChanged(speed);
            });
}

MpvHttpStreamRelay::~MpvHttpStreamRelay()
{
    stop();
}

QUrl MpvHttpStreamRelay::prepare(const QUrl &targetUrl, const QString &serverId,
                                 const QNetworkProxy &proxy, const QString &userAgent,
                                 const Tuning &tuning)
{
    if (!targetUrl.isValid() || targetUrl.scheme().isEmpty())
    {
        return {};
    }

    stop();
    if (!m_server->isListening() && !m_server->listen(QHostAddress::LocalHost, 0))
    {
        qWarning() << "[MpvHttpStreamRelay] failed to listen"
                   << "| error:" << m_server->errorString();
        return {};
    }

    m_targetUrl = targetUrl;
    m_serverId = serverId;
    m_streamToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_upstreamUserAgent = userAgent.trimmed();
    // Created here rather than in the constructor: QNetworkAccessManager has to
    // live in the thread that uses it, and prepare() now always runs on the
    // relay's own thread.
    if (!m_network)
    {
        m_network = new QNetworkAccessManager(this);
    }
    m_network->setProxy(proxy);
    m_bytesRelayedSinceLastTick = 0;
    m_readaheadBytes = tuning.readaheadBytes > 0 ? tuning.readaheadBytes : kDefaultReadaheadBytes;
    m_socketHighWaterBytes = tuning.socketHighWaterBytes > 0 ? tuning.socketHighWaterBytes
                                                            : kCacheSocketHighWaterBytes;
    m_pumpChunkBytes = tuning.pumpChunkBytes > 0 ? tuning.pumpChunkBytes : kRelayPumpChunkBytes;
    m_preparedNs = monotonicNs();
    m_firstByteLogged = false;
    m_redirectResolved = false;
    resetCache();
    m_speedTimer->start();

    QUrl localUrl;
    localUrl.setScheme(QStringLiteral("http"));
    localUrl.setHost(QStringLiteral("127.0.0.1"));
    localUrl.setPort(m_server->serverPort());
    localUrl.setPath(QStringLiteral("/%1/stream").arg(m_streamToken));

    qInfo() << "[MpvHttpStreamRelay] prepared"
            << "| target:" << LogRedactionUtils::url(m_targetUrl) << "| local:" << localUrl.toString(QUrl::FullyEncoded)
            << "| serverId:" << (m_serverId.isEmpty() ? QStringLiteral("<none>") : m_serverId)
            << "| proxyType:" << proxy.type()
            << "| readaheadMiB:" << (m_readaheadBytes / (1024 * 1024))
            << "| highWaterKiB:" << (m_socketHighWaterBytes / 1024)
            << "| pumpChunkKiB:" << (m_pumpChunkBytes / 1024);

    warmUpstreamRedirects();

    return localUrl;
}

void MpvHttpStreamRelay::stop()
{
    if (m_statConnections > 0)
    {
        logActivitySummary();
    }
    const auto sockets = m_connections.keys();
    for (QTcpSocket *socket : sockets)
    {
        closeConnection(socket);
    }
    if (m_server->isListening())
    {
        m_server->close();
    }
    if (m_speedTimer->isActive())
    {
        m_speedTimer->stop();
        m_bytesRelayedSinceLastTick = 0;
        Q_EMIT upstreamSpeedChanged(0);
    }
    m_connections.clear();
    m_targetUrl.clear();
    m_serverId.clear();
    m_streamToken.clear();
    releaseFetch();
    if (m_redirectProbe)
    {
        QNetworkReply *probe = m_redirectProbe;
        m_redirectProbe = nullptr;
        disconnect(probe, nullptr, this, nullptr);
        probe->abort();
        probe->deleteLater();
    }
    m_scheduledFetchPos = -1;
    resetCache();
}

void MpvHttpStreamRelay::onNewConnection()
{
    while (QTcpSocket *socket = m_server->nextPendingConnection())
    {
        const qint64 acceptInitStartNs = monotonicNs();
        auto inserted = m_connections.insert(socket, ConnectionState{});
        QPointer<QTcpSocket> safeSocket(socket);
        connect(socket, &QTcpSocket::readyRead, this,
                [this, safeSocket]()
                {
                    if (safeSocket)
                    {
                        onSocketReadyRead(safeSocket.data());
                    }
                });
        connect(socket, &QTcpSocket::bytesWritten, this,
                [this, safeSocket](qint64)
                {
                    if (!safeSocket)
                    {
                        return;
                    }
                    QTcpSocket *socket = safeSocket.data();
                    auto it = m_connections.find(socket);
                    if (it == m_connections.end())
                    {
                        return;
                    }
                    if (it->lastPumpReturnNs > 0)
                    {
                        // How long the socket needed before it would take more
                        // (the per-pass event-loop turnaround).
                        m_statTurnaroundNs += monotonicNs() - it->lastPumpReturnNs;
                        ++m_statTurnarounds;
                        it->lastPumpReturnNs = 0;
                    }
                    if (it->cacheMode)
                    {
                        pumpCacheToSocket(socket);
                    }
                    else
                    {
                        pumpReplyToSocket(socket);
                    }
                });
        connect(socket, &QTcpSocket::disconnected, this,
                [this, safeSocket]()
                {
                    if (safeSocket)
                    {
                        onSocketDisconnected(safeSocket.data());
                    }
                });
        inserted->acceptedNs = monotonicNs();
        m_statAcceptInitNs += inserted->acceptedNs - acceptInitStartNs;
        ++m_statAcceptInitCount;
    }
}

void MpvHttpStreamRelay::onSocketReadyRead(QTcpSocket *socket)
{
    ScopeTimer stageTimer(&m_statReadyReadNs);
    ++m_statReadyReadCalls;

    if (!socket || !socket->isOpen())
    {
        closeConnection(socket);
        return;
    }

    auto it = m_connections.find(socket);
    if (it == m_connections.end() || it->reply || it->cacheMode)
    {
        return;
    }

    it->buffer += socket->readAll();
    const int headerEnd = it->buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0)
    {
        if (it->buffer.size() > 64 * 1024)
        {
            writeError(socket, 431, "Request header too large");
        }
        return;
    }

    const QByteArray requestData = it->buffer.left(headerEnd + 4);
    ++m_statHeaderPathCount;
    processRequest(socket, requestData);
}

void MpvHttpStreamRelay::onSocketDisconnected(QTcpSocket *socket)
{
    // Closing a client must never stop an in-flight upstream read: that read is
    // what fills the cache for this client's next request (see "finish the
    // read" in the class comment).
    closeConnection(socket);
}

void MpvHttpStreamRelay::processRequest(QTcpSocket *socket, const QByteArray &requestData)
{
    ScopeTimer stageTimer(&m_statProcessNs);

    if (m_targetUrl.isEmpty() || m_streamToken.isEmpty())
    {
        writeError(socket, 503, "Relay target is not ready");
        return;
    }

    const QList<QByteArray> lines = requestData.split('\n');
    if (lines.isEmpty())
    {
        writeError(socket, 400, "Bad request");
        return;
    }

    const QList<QByteArray> requestParts = lines.first().trimmed().split(' ');
    if (requestParts.size() < 2)
    {
        writeError(socket, 400, "Bad request");
        return;
    }

    const QByteArray method = requestParts.at(0).toUpper();
    const QByteArray path = requestParts.at(1);
    const QByteArray expectedPrefix = QByteArray("/") + m_streamToken.toUtf8() + QByteArray("/");
    if (!(method == "GET" || method == "HEAD") || !path.startsWith(expectedPrefix))
    {
        writeError(socket, 404, "Not found");
        return;
    }

    QByteArray rangeHeader;
    QByteArray acceptHeader;
    for (int i = 1; i < lines.size(); ++i)
    {
        const QByteArray line = lines.at(i).trimmed();
        if (line.isEmpty())
        {
            continue;
        }
        const int colon = line.indexOf(':');
        if (colon <= 0)
        {
            continue;
        }
        const QByteArray lowerName = line.left(colon).trimmed().toLower();
        if (lowerName == "range")
        {
            rangeHeader = line.mid(colon + 1).trimmed();
        }
        else if (lowerName == "accept")
        {
            acceptHeader = line.mid(colon + 1).trimmed();
        }
    }

    auto it = m_connections.find(socket);
    if (it == m_connections.end())
    {
        return;
    }
    const bool headOnly = method == "HEAD";
    it->headOnly = headOnly;

    qint64 begin = 0;
    qint64 end = -1;
    const bool hasRange = !rangeHeader.isEmpty() && parseRangeHeader(rangeHeader, &begin, &end);

    if (!headOnly && hasRange && !m_rangeUnsupported)
    {
        it->cacheMode = true;
        it->reqBegin = begin;
        it->reqEnd = end;
        it->needPos = begin;
        it->servedFromCache = 0;
        it->requestNs = monotonicNs();

        ++m_statCacheRequests;
        if (m_statCacheRequests == 1 || m_statCacheRequests % 500 == 0)
        {
            qDebug() << "[MpvHttpStreamRelay] ranged request"
                     << "| count:" << m_statCacheRequests
                     << "| range:" << rangeHeader << "| begin:" << begin << "| end:" << end;
        }

        pumpCacheToSocket(socket);

        it = m_connections.find(socket);
        if (it != m_connections.end() && !it->finished && cacheBlockContaining(it->needPos) < 0)
        {
            ensureFetchFrom(it->needPos);
        }
        return;
    }

    // The relay cannot serve this media itself (the upstream refused Range, or
    // its reads kept failing). Rather than proxy the request -- which could then
    // fail on our own account -- hand mpv the original URL and step aside.
    if (m_rangeUnsupported && !headOnly && !m_targetUrl.isEmpty())
    {
        ++m_statRedirects;
        qInfo() << "[MpvHttpStreamRelay] handing the client back to the upstream url"
                << "| range:" << rangeHeader;
        it = m_connections.find(socket);
        if (it != m_connections.end())
        {
            it->finished = true;
        }
        writeRedirect(socket, m_targetUrl);
        return;
    }

    startPassThrough(socket, rangeHeader, acceptHeader, headOnly);
}

void MpvHttpStreamRelay::startPassThrough(QTcpSocket *socket, const QByteArray &rangeHeader,
                                          const QByteArray &acceptHeader, bool headOnly)
{
    auto it = m_connections.find(socket);
    if (it == m_connections.end() || it->reply)
    {
        return;
    }

    QNetworkRequest request(m_targetUrl);
    // Manual redirect handling: the chain downgrades https to http, which the
    // default policy rejects outright ("Insecure redirect"). mpv understands
    // the 302 we forward, so let it follow the redirect itself.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (!rangeHeader.isEmpty())
    {
        request.setRawHeader("Range", rangeHeader);
    }
    if (!acceptHeader.isEmpty())
    {
        request.setRawHeader("Accept", acceptHeader);
    }
    // Strict-UA servers reject the default UA: the custom UA (when set)
    // overrides whatever mpv sent, otherwise mpv's UA passes through.
    if (!m_upstreamUserAgent.isEmpty())
    {
        request.setHeader(QNetworkRequest::UserAgentHeader, m_upstreamUserAgent);
    }

    it->headOnly = headOnly;
    it->reply = headOnly ? m_network->head(request) : m_network->get(request);
    it->reply->setReadBufferSize(kReplyReadBufferBytes);

    qDebug() << "[MpvHttpStreamRelay] pass-through request"
             << "| method:" << (headOnly ? "HEAD" : "GET")
             << "| target:" << LogRedactionUtils::url(m_targetUrl)
             << "| range:" << request.rawHeader("Range");

    QNetworkReply *reply = it->reply;
    QPointer<QTcpSocket> safeSocket(socket);
    QPointer<QNetworkReply> safeReply(reply);
    connect(reply, &QNetworkReply::readyRead, this,
            [this, safeSocket, safeReply]()
            {
                if (!safeSocket || !safeReply)
                {
                    return;
                }
                QTcpSocket *socket = safeSocket.data();
                QNetworkReply *reply = safeReply.data();
                auto it = m_connections.find(socket);
                if (it == m_connections.end() || it->reply != reply)
                {
                    return;
                }
                if (!canWriteToSocket(socket))
                {
                    closeConnection(socket);
                    return;
                }

                pumpReplyToSocket(socket);
            });
    connect(reply, &QNetworkReply::finished, this,
            [this, safeSocket, safeReply]()
            {
                if (!safeSocket || !safeReply)
                {
                    return;
                }
                QTcpSocket *socket = safeSocket.data();
                QNetworkReply *reply = safeReply.data();
                auto it = m_connections.find(socket);
                if (it == m_connections.end() || it->reply != reply)
                {
                    return;
                }

                it->upstreamFinished = true;

                if (reply->error() != QNetworkReply::NoError &&
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 0)
                {
                    qWarning() << "[MpvHttpStreamRelay] upstream failed"
                               << "| target:" << LogRedactionUtils::url(m_targetUrl)
                               << "| error:" << reply->errorString();
                }

                pumpReplyToSocket(socket);
            });
}

void MpvHttpStreamRelay::sendReplyHeaders(QTcpSocket *socket)
{
    auto it = m_connections.find(socket);
    if (it == m_connections.end() || !it->reply || it->headersSent || !canWriteToSocket(socket))
    {
        return;
    }

    QNetworkReply *reply = it->reply;
    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode == 0)
    {
        statusCode = reply->error() == QNetworkReply::NoError ? 200 : 502;
    }

    QByteArray response = "HTTP/1.1 " + QByteArray::number(statusCode) + " " + reasonPhrase(statusCode) + "\r\n";
    const auto headerPairs = reply->rawHeaderPairs();
    for (const auto &pair : headerPairs)
    {
        QByteArray name = pair.first;
        const QByteArray lowerName = name.toLower();
        if (isHopByHopHeader(lowerName))
        {
            continue;
        }
        response += name + ": " + pair.second + "\r\n";
    }
    response += "Connection: close\r\n";
    response += "\r\n";

    if (socket->write(response) >= 0)
    {
        it->headersSent = true;
    }
}

void MpvHttpStreamRelay::pumpReplyToSocket(QTcpSocket *socket)
{
    auto it = m_connections.find(socket);
    if (it == m_connections.end() || !it->reply)
    {
        return;
    }
    if (!canWriteToSocket(socket))
    {
        closeConnection(socket);
        return;
    }

    QNetworkReply *reply = it->reply;
    sendReplyHeaders(socket);

    if (!it->headOnly && reply->isOpen())
    {
        while (reply->bytesAvailable() > 0 && socket->bytesToWrite() < kSocketQueuedBytesHighWater)
        {
            const qint64 budget = kSocketQueuedBytesHighWater - socket->bytesToWrite();
            const qint64 readSize = qMin(kRelayPumpChunkBytes, qMin(budget, reply->bytesAvailable()));
            if (readSize <= 0)
            {
                break;
            }

            const QByteArray chunk = reply->read(readSize);
            if (chunk.isEmpty())
            {
                break;
            }

            const qint64 written = socket->write(chunk);
            if (written > 0)
            {
                recordRelayedBytes(written);
            }
            if (written < static_cast<qint64>(chunk.size()))
            {
                break;
            }

            it = m_connections.find(socket);
            if (it == m_connections.end() || it->reply != reply || !canWriteToSocket(socket))
            {
                return;
            }
        }
    }

    it = m_connections.find(socket);
    if (it == m_connections.end() || it->reply != reply)
    {
        return;
    }

    const bool allReplyDataDrained = it->headOnly || !reply->isOpen() || reply->bytesAvailable() == 0;
    if (!it->upstreamFinished || !allReplyDataDrained)
    {
        return;
    }

    it->reply = nullptr;
    disconnect(reply, nullptr, this, nullptr);
    reply->deleteLater();

    if (socket->state() != QAbstractSocket::UnconnectedState)
    {
        socket->flush();
        socket->disconnectFromHost();
    }
    else
    {
        closeConnection(socket);
    }
}

// ---------------------------------------------------------------------------
// Byte-range cache
// ---------------------------------------------------------------------------

void MpvHttpStreamRelay::resetCache()
{
    m_cache.clear();
    m_cachedBytes = 0;
    m_totalSize = -1;
    m_contentType.clear();
    m_rangeUnsupported = false;
    m_fetchPos = 0;
    m_fetchLimit = 0;
    m_redirectTarget.clear();
    m_redirectDepth = 0;
    m_redirectPending = false;
    m_statFetches = 0;
    m_statCacheRequests = 0;
    m_statConnections = 0;
    m_statBytesFromCache = 0;
    m_statBytesFromUpstream = 0;
    m_statTimedConnections = 0;
    m_statAcceptToRequestNs = 0;
    m_statRequestToHeadersNs = 0;
    m_statHeadersToDoneNs = 0;
    m_statPumpWrites = 0;
    m_statDiscardedBytes = 0;
    m_statPumpNs = 0;
    m_statWriteNs = 0;
    m_statAcceptInitNs = 0;
    m_statAcceptInitCount = 0;
    m_statCloseNs = 0;
    m_statCloseCount = 0;
    m_statTurnaroundNs = 0;
    m_statTurnarounds = 0;
    m_statPumpCalls = 0;
    m_statReadyReadNs = 0;
    m_statReadyReadCalls = 0;
    m_statProcessNs = 0;
    m_statSendHeadersNs = 0;
    m_statHeaderPathCount = 0;
    m_statRedirects = 0;
}

void MpvHttpStreamRelay::recountCachedBytes()
{
    qint64 total = 0;
    for (const CacheBlock &block : m_cache)
    {
        total += block.data.size();
    }
    m_cachedBytes = total;
}

void MpvHttpStreamRelay::normalizeCache()
{
    if (m_cache.size() < 2)
    {
        return;
    }

    std::sort(m_cache.begin(), m_cache.end(),
              [](const CacheBlock &a, const CacheBlock &b) { return a.begin < b.begin; });

    QVector<CacheBlock> merged;
    merged.reserve(m_cache.size());
    for (const CacheBlock &block : m_cache)
    {
        if (block.data.isEmpty())
        {
            continue;
        }
        if (merged.isEmpty())
        {
            merged.append(block);
            continue;
        }
        CacheBlock &last = merged.last();
        const qint64 lastEnd = last.begin + last.data.size();
        if (block.begin > lastEnd)
        {
            merged.append(block);
            continue;
        }
        // Overlapping or adjacent: keep what we already have and append only
        // the bytes we are still missing. Heat is carried over as the hotter
        // of the two -- merging must never lose it, otherwise a block that was
        // hot before the merge would come out cold and be evicted first.
        last.hits = qMax(last.hits, block.hits);
        last.lastHitNs = qMax(last.lastHitNs, block.lastHitNs);
        const qint64 overlap = lastEnd - block.begin;
        if (overlap < static_cast<qint64>(block.data.size()))
        {
            last.data.append(block.data.constData() + overlap, block.data.size() - overlap);
        }
    }
    m_cache = merged;
}

void MpvHttpStreamRelay::appendToCache(const QByteArray &data)
{
    if (data.isEmpty())
    {
        return;
    }

    CacheBlock block;
    block.begin = m_fetchPos;
    block.data = data;

    bool mergedIntoPrevious = false;
    if (!m_cache.isEmpty())
    {
        CacheBlock &prev = m_cache.last();
        const qint64 prevEnd = prev.begin + prev.data.size();
        if (block.begin >= prev.begin && prevEnd >= block.begin)
        {
            const qint64 overlap = prevEnd - block.begin;
            if (overlap < static_cast<qint64>(block.data.size()))
            {
                const qint64 appended = block.data.size() - overlap;
                prev.data.append(block.data.constData() + overlap, appended);
                // The bytes just appended have not been read by mpv yet, so the
                // block keeps its existing heat (>= the fresh block's zero).
                // This must never reset the counters: the audio region of a
                // non-interleaved file is exactly the block that keeps growing
                // through this branch, so resetting would pin it at hits == 0
                // and make it the prime eviction victim -- i.e. the whole
                // heat-based fix would silently do nothing.
                prev.hits = qMax(prev.hits, block.hits);
                prev.lastHitNs = qMax(prev.lastHitNs, block.lastHitNs);
                m_cachedBytes += appended;
            }
            mergedIntoPrevious = true;
        }
    }
    if (!mergedIntoPrevious)
    {
        m_cache.append(block);
        // A newly appended out-of-order block can overlap existing ones, and
        // normalizeCache() drops those duplicated bytes while merging, so the
        // new total is not a plain addition. This path is the rare one -- a
        // sequential read always merges into the previous block -- so
        // recomputing here keeps m_cachedBytes exact while removing the O(n)
        // recount that used to run on every single 1 MiB chunk.
        normalizeCache();
        recountCachedBytes();
    }

    m_fetchPos += data.size();
    evictCacheIfNeeded();
}

void MpvHttpStreamRelay::evictCacheIfNeeded()
{
    // Hot path: as long as the cache is within budget this is a single integer
    // comparison, which matters because appendToCache() calls it for every
    // 1 MiB chunk arriving from upstream.
    if (m_cachedBytes <= kCacheMemoryLimitBytes)
    {
        return;
    }

    // Read the clock once for the whole pass, never once per block.
    const qint64 nowNs = monotonicNs();
    // Lazy time decay: a block untouched for a while gives up heat in powers
    // of two, so an early burst of hits cannot pin it in the cache forever.
    // Computed on demand rather than by a timer on purpose -- a timer would
    // have to walk the entire cache periodically, while this is O(1) per block
    // and only ever runs when something is actually being evicted.
    const auto effectiveHits = [nowNs](const CacheBlock &block) -> qint64
    {
        if (block.hits == 0)
        {
            return 0;
        }
        const qint64 idleNs = nowNs - block.lastHitNs;
        if (idleNs <= kHitHalfLifeNs)
        {
            return block.hits;
        }
        const qint64 shifts = idleNs / kHitHalfLifeNs;
        if (shifts >= 32) // quint32 >> 32 would be undefined
        {
            return 0;
        }
        return block.hits >> static_cast<int>(shifts);
    };

    while (m_cachedBytes > kCacheMemoryLimitBytes && m_cache.size() > 1)
    {
        int victim = 0;
        qint64 victimHits = 0;
        qint64 victimLastNs = 0;
        bool haveVictim = false;
        for (int i = 0; i < m_cache.size(); ++i)
        {
            const CacheBlock &block = m_cache.at(i);
            const qint64 hits = effectiveHits(block);
            // Coldest first; among equally cold blocks, the one idle longest
            // goes. So bytes that read-ahead pulled in but mpv never consumed
            // (hits == 0) go first, which is exactly what is worth dropping.
            // Deliberately NOT by distance from m_fetchPos: that treated the
            // upstream read cursor as the playback cursor, and for a
            // non-interleaved file the audio at the end of the file is always
            // the farthest block, so it was evicted first on every pass no
            // matter how often mpv asked for it.
            const bool worse = !haveVictim || hits < victimHits ||
                               (hits == victimHits && block.lastHitNs < victimLastNs);
            if (worse)
            {
                haveVictim = true;
                victim = i;
                victimHits = hits;
                victimLastNs = block.lastHitNs;
            }
        }
        m_cachedBytes -= m_cache.at(victim).data.size();
        m_cache.remove(victim);
    }
}

int MpvHttpStreamRelay::cacheBlockContaining(qint64 pos) const
{
    int lo = 0;
    int hi = m_cache.size();
    while (lo < hi)
    {
        const int mid = (lo + hi) / 2;
        if (m_cache.at(mid).begin <= pos)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    if (lo == 0)
    {
        return -1;
    }
    const int index = lo - 1;
    const CacheBlock &block = m_cache.at(index);
    const qint64 offset = pos - block.begin;
    if (offset < 0 || offset >= static_cast<qint64>(block.data.size()))
    {
        return -1;
    }
    return index;
}

void MpvHttpStreamRelay::sendCacheHeaders(QTcpSocket *socket)
{
    auto it = m_connections.find(socket);
    if (it == m_connections.end() || it->headersSent || !canWriteToSocket(socket))
    {
        return;
    }
    if (m_totalSize <= 0)
    {
        return; // upstream headers have not arrived yet
    }

    // Only the pass that actually answers is timed; the no-op calls the pump
    // loop makes on every turn would otherwise dilute this to nothing.
    ScopeTimer stageTimer(&m_statSendHeadersNs);

    const qint64 begin = it->reqBegin;
    const qint64 end = (it->reqEnd >= 0) ? qMin(it->reqEnd, m_totalSize - 1) : (m_totalSize - 1);
    if (end < begin)
    {
        writeError(socket, 416, "Requested range not satisfiable");
        return;
    }

    QByteArray response = "HTTP/1.1 206 Partial Content\r\n";
    response += "Content-Range: bytes " + QByteArray::number(begin) + "-" + QByteArray::number(end) +
                "/" + QByteArray::number(m_totalSize) + "\r\n";
    response += "Content-Length: " + QByteArray::number(end - begin + 1) + "\r\n";
    if (!m_contentType.isEmpty())
    {
        response += "Content-Type: " + m_contentType + "\r\n";
    }
    response += "Accept-Ranges: bytes\r\n";
    response += "Connection: close\r\n";
    response += "\r\n";

    if (socket->write(response) >= 0)
    {
        it->headersSent = true;
        it->headersNs = monotonicNs();
    }
}

void MpvHttpStreamRelay::pumpCacheToSocket(QTcpSocket *socket)
{
    if (!socket)
    {
        return;
    }
    auto entry = m_connections.find(socket);
    if (entry != m_connections.end())
    {
        ++entry->pumpCalls;
        ++m_statPumpCalls;
    }

    pumpCacheToSocketImpl(socket);

    // Stamp the return so the next bytesWritten can tell how long the socket
    // needed before it would accept more.
    auto after = m_connections.find(socket);
    if (after != m_connections.end())
    {
        after->lastPumpReturnNs = monotonicNs();
    }
}

void MpvHttpStreamRelay::pumpCacheToSocketImpl(QTcpSocket *socket)
{
    if (!socket)
    {
        return;
    }
    ScopeTimer pumpTimer(&m_statPumpNs);

    for (;;)
    {
        auto it = m_connections.find(socket);
        if (it == m_connections.end() || !it->cacheMode || it->finished)
        {
            return;
        }
        if (!canWriteToSocket(socket))
        {
            closeConnection(socket);
            return;
        }
        if (m_totalSize <= 0)
        {
            return; // wait for the upstream headers before answering
        }

        sendCacheHeaders(socket);
        it = m_connections.find(socket);
        if (it == m_connections.end() || it->finished)
        {
            return;
        }

        const int blockIndex = cacheBlockContaining(it->needPos);
        if (blockIndex < 0)
        {
            // Nothing cached at needPos yet: make sure an upstream read is on
            // its way (no-op when the in-flight read already covers it).
            ensureFetchFrom(it->needPos);
            return;
        }

        // Heat is counted HERE -- the one place that actually hands bytes to
        // mpv. It must NOT go inside cacheBlockContaining(): two of that
        // function's three call sites only probe ("is anything cached at
        // needPos?", and they act on the negative answer) and would count a
        // hit for a request that never got served, inflating the sequential
        // video region until the heat signal can no longer tell the two hot
        // spots apart.
        CacheBlock &block = m_cache[blockIndex];
        ++block.hits;
        block.lastHitNs = monotonicNs();

        const qint64 offset = it->needPos - block.begin;
        const qint64 wantEnd = (it->reqEnd >= 0) ? qMin(it->reqEnd, m_totalSize - 1) : (m_totalSize - 1);

        qint64 available = static_cast<qint64>(block.data.size()) - offset;
        available = qMin(available, wantEnd - it->needPos + 1);
        if (available <= 0)
        {
            if (it->needPos > wantEnd)
            {
                finishCacheConnection(socket);
            }
            return;
        }

        const qint64 budget = m_socketHighWaterBytes - socket->bytesToWrite();
        if (budget <= 0)
        {
            return;
        }
        const qint64 chunkSize = qMin(available, qMin(m_pumpChunkBytes, budget));
        qint64 written = 0;
        {
            // Timed separately: write() also runs the socket flush, and on a
            // full loopback buffer that means shifting Qt's whole write queue.
            ScopeTimer writeTimer(&m_statWriteNs);
            written = socket->write(block.data.constData() + offset, chunkSize);
        }
        if (written <= 0)
        {
            return;
        }
        recordRelayedBytes(written);
        m_statBytesFromCache += written;

        it = m_connections.find(socket);
        if (it == m_connections.end() || it->finished)
        {
            return;
        }
        it->needPos += written;
        it->servedFromCache += written;
        it->writtenBytes += written;
        ++it->pumpWrites;

        // Keep the upstream read ahead of an actively consuming client so a
        // sequential read never has to restart the transfer.
        if (m_fetch && !m_fetch->isFinished() &&
            it->needPos + kFetchLimitLowWaterBytes > m_fetchLimit)
        {
            m_fetchLimit = it->needPos + m_readaheadBytes;
        }

        if (it->needPos > wantEnd)
        {
            finishCacheConnection(socket);
            return;
        }
        if (written < chunkSize)
        {
            return; // socket back-pressure, resume on bytesWritten
        }
    }
}

void MpvHttpStreamRelay::finishCacheConnection(QTcpSocket *socket)
{
    auto it = m_connections.find(socket);
    if (it == m_connections.end() || it->finished)
    {
        return;
    }
    it->finished = true;

    qDebug() << "[MpvHttpStreamRelay] client done"
             << "| begin:" << it->reqBegin << "| end:" << it->reqEnd
             << "| sent:" << (it->needPos - it->reqBegin)
             << "| fromCache:" << it->servedFromCache;

    if (socket->state() != QAbstractSocket::UnconnectedState)
    {
        socket->disconnectFromHost();
    }
    else
    {
        closeConnection(socket);
    }
}

void MpvHttpStreamRelay::broadcastCachedData()
{
    const auto sockets = m_connections.keys();
    for (QTcpSocket *socket : sockets)
    {
        pumpCacheToSocket(socket);
    }
}

// ---------------------------------------------------------------------------
// Shared upstream read ("finish the read")
// ---------------------------------------------------------------------------

bool MpvHttpStreamRelay::startFetch(qint64 pos)
{
    releaseFetch();
    m_redirectPending = false;
    if (m_targetUrl.isEmpty() || m_rangeUnsupported)
    {
        return false;
    }

    if (m_redirectTarget.isEmpty())
    {
        m_redirectDepth = 0; // a new chain starts from the original URL
    }

    QNetworkRequest request(effectiveUpstreamUrl());
    // Resolve redirects by hand: the default policy refuses the https -> http
    // hop of this server chain, so followRedirect() walks the chain instead.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(pos) + "-");
    if (!m_upstreamUserAgent.isEmpty())
    {
        request.setHeader(QNetworkRequest::UserAgentHeader, m_upstreamUserAgent);
    }

    m_fetch = m_network->get(request);
    m_fetch->setReadBufferSize(kReplyReadBufferBytes);
    m_fetchPos = pos;
    m_fetchRequestPos = pos;
    m_fetchLimit = pos + m_readaheadBytes;
    ++m_statFetches;

    qInfo() << "[MpvHttpStreamRelay] upstream fetch"
            << "| begin:" << pos << "| limit:" << m_fetchLimit
            << "| fetchCount:" << m_statFetches;

    connect(m_fetch, &QNetworkReply::readyRead, this, [this]() { onFetchReadyRead(); });
    connect(m_fetch, &QNetworkReply::finished, this, [this]() { onFetchFinished(); });
    return true;
}

void MpvHttpStreamRelay::scheduleFetch(qint64 pos)
{
    if (m_schedulePending)
    {
        // A queued redirect retry must keep its own offset: a later caller
        // asking for a different position must not overwrite it.
        if (m_redirectPending)
        {
            return;
        }
        m_scheduledFetchPos = pos;
        return;
    }
    m_scheduledFetchPos = pos;
    m_schedulePending = true;
    // Never abort or restart a reply from inside its own readyRead handler:
    // defer the (re)start to the next event loop turn.
    QTimer::singleShot(0, this, [this]() { runScheduledFetch(); });
}

void MpvHttpStreamRelay::runScheduledFetch()
{
    m_schedulePending = false;
    const qint64 pos = m_scheduledFetchPos;
    m_scheduledFetchPos = -1;
    if (pos < 0 || m_rangeUnsupported || m_targetUrl.isEmpty())
    {
        return;
    }
    // A pending redirect retry always wins: the in-flight reply holds no payload
    // even though its offset still looks "covered".
    if (!m_redirectPending && m_fetch && !m_fetch->isFinished() &&
        pos >= m_fetchPos && pos < m_fetchLimit)
    {
        return; // the in-flight read already covers this offset
    }
    startFetch(pos);
}

void MpvHttpStreamRelay::ensureFetchFrom(qint64 pos)
{
    if (m_rangeUnsupported)
    {
        return;
    }
    if (m_redirectPending)
    {
        return; // a retry against the resolved target is already queued
    }
    if (m_fetch && !m_fetch->isFinished() && pos >= m_fetchPos && pos < m_fetchLimit)
    {
        return;
    }
    scheduleFetch(pos);
}

void MpvHttpStreamRelay::resumeStalledConnections()
{
    const auto sockets = m_connections.keys();
    for (QTcpSocket *socket : sockets)
    {
        auto it = m_connections.find(socket);
        if (it == m_connections.end() || !it->cacheMode || it->finished)
        {
            continue;
        }
        if (cacheBlockContaining(it->needPos) < 0)
        {
            ensureFetchFrom(it->needPos);
            return;
        }
    }
}

// Resolving this server's redirect chain costs two extra round trips, measured
// at ~8 seconds together, and mpv cannot start until the first byte arrives.
// Kick the resolution off in the background as soon as the relay is prepared, so
// mpv's first request can go straight to the final URL. Failing here is
// harmless: the normal read path resolves the chain itself.
void MpvHttpStreamRelay::warmUpstreamRedirects()
{
    if (m_targetUrl.isEmpty() || m_redirectResolved)
    {
        return;
    }
    resolveRedirectStep(m_targetUrl, 0);
}

void MpvHttpStreamRelay::resolveRedirectStep(const QUrl &url, int depth)
{
    if (m_targetUrl.isEmpty() || m_redirectResolved || depth > kMaxRedirects)
    {
        return;
    }
    if (m_redirectProbe)
    {
        return; // one probe at a time
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Range", "bytes=0-0"); // one byte is enough to walk the chain
    // A stalled hop would otherwise keep the probe pending until the media is
    // closed, leaving the whole warm-up silent for the session.
    request.setTransferTimeout(kRedirectProbeTimeoutMs);
    if (!m_upstreamUserAgent.isEmpty())
    {
        request.setHeader(QNetworkRequest::UserAgentHeader, m_upstreamUserAgent);
    }

    QNetworkReply *probe = m_network->get(request);
    probe->setReadBufferSize(64 * 1024);
    m_redirectProbe = probe;

    qInfo() << "[MpvHttpStreamRelay] pre-resolve probing"
            << "| depth:" << depth
            << "| url:" << LogRedactionUtils::url(url);

    connect(probe, &QNetworkReply::finished, this,
            [this, probe, depth]()
            {
                if (probe != m_redirectProbe)
                {
                    return; // stop() or a newer probe took over
                }
                const int statusCode = probe->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QByteArray location = probe->rawHeader("Location");
                const QUrl probeUrl = probe->request().url();
                const QString probeErrorText = probe->errorString();
                m_redirectProbe = nullptr;
                probe->deleteLater();

                if (statusCode >= 300 && statusCode < 400)
                {
                    const QUrl next = probeUrl.resolved(QUrl::fromEncoded(location));
                    if (!location.isEmpty() && next.isValid() && !next.scheme().isEmpty())
                    {
                        qInfo() << "[MpvHttpStreamRelay] pre-resolve hop"
                                << "| depth:" << depth << "| status:" << statusCode
                                << "| to:" << LogRedactionUtils::url(next);
                        resolveRedirectStep(next, depth + 1);
                        return;
                    }
                    // A redirect without a usable Location is a dead end; say so
                    // instead of dropping out of the warm-up without a trace.
                    qWarning() << "[MpvHttpStreamRelay] pre-resolve hop has no usable Location"
                               << "| depth:" << depth << "| status:" << statusCode
                               << "| location:" << location;
                    return;
                }

                if (statusCode == 206 || statusCode == 200)
                {
                    m_redirectTarget = probeUrl;
                    m_redirectDepth = depth;
                    m_redirectResolved = true;
                    qInfo() << "[MpvHttpStreamRelay] redirect chain pre-resolved"
                            << "| hops:" << depth
                            << "| to:" << LogRedactionUtils::url(m_redirectTarget);
                    return;
                }

                // Worth knowing about: without this the warm-up can fail silently
                // and the first request pays the whole chain again.
                qWarning() << "[MpvHttpStreamRelay] redirect pre-resolve failed"
                           << "| url:" << LogRedactionUtils::url(probeUrl)
                           << "| status:" << statusCode
                           << "| error:" << probeErrorText;
            });
}

QUrl MpvHttpStreamRelay::effectiveUpstreamUrl() const
{
    return m_redirectTarget.isEmpty() ? m_targetUrl : m_redirectTarget;
}

// Returns true when `reply` carries a redirect that is now being followed, in
// which case the caller must stop using that reply. Redirects are resolved by
// hand because QNetworkAccessManager's default policy refuses to downgrade
// https to http, and this server chain does exactly that
// (Emby --302--> OpenList --302--> pre-signed object storage).
bool MpvHttpStreamRelay::followRedirect(QNetworkReply *reply)
{
    if (!reply)
    {
        return false;
    }

    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode < 300 || statusCode >= 400)
    {
        return false;
    }

    const QByteArray location = reply->rawHeader("Location");
    if (location.isEmpty())
    {
        qWarning() << "[MpvHttpStreamRelay] redirect without a Location header"
                   << "| status:" << statusCode
                   << "| url:" << LogRedactionUtils::url(reply->request().url());
        return false;
    }

    if (m_redirectDepth >= kMaxRedirects)
    {
        qWarning() << "[MpvHttpStreamRelay] too many redirects, giving up"
                   << "| status:" << statusCode << "| depth:" << m_redirectDepth
                   << "| url:" << LogRedactionUtils::url(reply->request().url());
        return false;
    }

    const QUrl next = reply->request().url().resolved(QUrl::fromEncoded(location));
    if (!next.isValid() || next.scheme().isEmpty())
    {
        qWarning() << "[MpvHttpStreamRelay] unusable redirect target"
                   << "| location:" << location;
        return false;
    }

    ++m_redirectDepth;
    m_redirectTarget = next;

    qInfo() << "[MpvHttpStreamRelay] following redirect"
            << "| status:" << statusCode
            << "| from:" << LogRedactionUtils::url(reply->request().url())
            << "| to:" << LogRedactionUtils::url(next)
            << "| depth:" << m_redirectDepth;
    return true;
}

void MpvHttpStreamRelay::parseFetchHeaders()
{
    if (!m_fetch || m_rangeUnsupported || m_redirectPending || m_totalSize > 0)
    {
        return;
    }

    const int statusCode = m_fetch->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode == 0)
    {
        return; // no response yet
    }

    m_contentType = m_fetch->rawHeader("Content-Type");

    if (followRedirect(m_fetch))
    {
        // A redirect carries no payload: flag it so readyRead stops reading this
        // reply (its body would otherwise be appended to the cache and drag
        // m_fetchPos past the offset mpv actually asked for), and re-issue from
        // the offset this read was issued for. The restart is deferred, because
        // aborting a reply from inside its own readyRead handler is not safe.
        m_redirectPending = true;
        const qint64 resumePos = m_fetchRequestPos;
        qInfo() << "[MpvHttpStreamRelay] retrying read after redirect"
                << "| begin:" << resumePos;
        scheduleFetch(resumePos);
        return;
    }

    if (statusCode != 206)
    {
        // Either the upstream ignored our Range request (HTTP 200) or it
        // failed outright. Caching byte ranges is impossible in both cases, so
        // fall back to the plain pass-through behaviour.
        m_rangeUnsupported = true;
        qWarning() << "[MpvHttpStreamRelay] unusable upstream response, falling back to pass-through"
                   << "| status:" << statusCode
                   << "| target:" << LogRedactionUtils::url(m_targetUrl)
                   << "| error:" << m_fetch->errorString();
        QTimer::singleShot(0, this,
                           [this]()
                           {
                               releaseFetch();
                               redirectClientsUpstream();
                           });
        return;
    }

    const QByteArray contentRange = m_fetch->rawHeader("Content-Range");
    const int slash = contentRange.lastIndexOf('/');
    if (slash > 0)
    {
        bool ok = false;
        const qint64 total = contentRange.mid(slash + 1).trimmed().toLongLong(&ok);
        if (ok && total > 0)
        {
            m_totalSize = total;
        }
    }

    // The server may answer from a slightly different offset than we asked
    // for; trust the header, since that is where the payload actually starts.
    const int space = contentRange.indexOf(' ');
    const int dash = contentRange.indexOf('-');
    if (space >= 0 && dash > space)
    {
        bool ok = false;
        const qint64 serverBegin = contentRange.mid(space + 1, dash - space - 1).toLongLong(&ok);
        if (ok && serverBegin >= 0 && serverBegin != m_fetchPos)
        {
            qWarning() << "[MpvHttpStreamRelay] upstream served a different offset"
                       << "| requested:" << m_fetchPos << "| served:" << serverBegin;
            m_fetchPos = serverBegin;
        }
    }

    if (!m_firstByteLogged && m_preparedNs >= 0)
    {
        // Everything before this point is time mpv spends waiting with nothing
        // to decode, so it is the number that explains a slow start-up.
        m_firstByteLogged = true;
        qInfo() << "[MpvHttpStreamRelay] first upstream byte"
                << "| after:" << ((monotonicNs() - m_preparedNs) / 1000000) << "ms";
    }
    qInfo() << "[MpvHttpStreamRelay] upstream ready"
            << "| status:" << statusCode << "| totalSize:" << m_totalSize
            << "| contentRange:" << contentRange
            << "| contentType:" << m_contentType;
}

void MpvHttpStreamRelay::onFetchReadyRead()
{
    if (!m_fetch)
    {
        return;
    }
    if (m_totalSize <= 0)
    {
        parseFetchHeaders();
    }
    if (!m_fetch || m_rangeUnsupported || m_redirectPending)
    {
        return;
    }

    for (;;)
    {
        if (!m_fetch)
        {
            break;
        }
        if (m_fetch->bytesAvailable() <= 0)
        {
            return;
        }
        if (m_fetchPos >= m_fetchLimit)
        {
            // "Finish the read" finished: the read-ahead window is full.
            qDebug() << "[MpvHttpStreamRelay] read-ahead window reached"
                     << "| fetchPos:" << m_fetchPos << "| cachedBytes:" << m_cachedBytes;
            releaseFetch();
            break;
        }

        const qint64 budget = m_fetchLimit - m_fetchPos;
        const qint64 readSize = qMin(kFetchChunkBytes, qMin(budget, m_fetch->bytesAvailable()));
        if (readSize <= 0)
        {
            break;
        }
        const QByteArray chunk = m_fetch->read(readSize);
        if (chunk.isEmpty())
        {
            break;
        }

        appendToCache(chunk);
        m_statBytesFromUpstream += chunk.size();
        broadcastCachedData();
    }

    resumeStalledConnections();
}

void MpvHttpStreamRelay::onFetchFinished()
{
    if (!m_fetch)
    {
        return;
    }

    QNetworkReply *reply = m_fetch;
    if (m_totalSize <= 0)
    {
        parseFetchHeaders();
    }

    const int finishedStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // A redirect carries no payload, and parseFetchHeaders() has already
    // scheduled the retry against the resolved target: never let its body reach
    // the cache, and never report it as a failed read either.
    if (m_fetch == reply && finishedStatus >= 300 && finishedStatus < 400)
    {
        return;
    }

    if (m_fetch == reply && !m_rangeUnsupported)
    {
        // Drain whatever is left before reporting the failure.
        while (reply->bytesAvailable() > 0 && m_fetchPos < m_fetchLimit)
        {
            const qint64 budget = m_fetchLimit - m_fetchPos;
            const qint64 readSize = qMin(kFetchChunkBytes, qMin(budget, reply->bytesAvailable()));
            if (readSize <= 0)
            {
                break;
            }
            const QByteArray chunk = reply->read(readSize);
            if (chunk.isEmpty())
            {
                break;
            }
            appendToCache(chunk);
            m_statBytesFromUpstream += chunk.size();
            broadcastCachedData();
        }
    }

    if (m_fetch == reply)
    {
        // The resolved redirect target is a pre-signed URL (X-Amz-Expires=900),
        // so it expires: drop it and resolve the chain again from the original
        // URL instead of handing the failure to mpv.
        if ((finishedStatus == 401 || finishedStatus == 403) && !m_redirectTarget.isEmpty())
        {
            qInfo() << "[MpvHttpStreamRelay] resolved target rejected, re-resolving"
                    << "| status:" << finishedStatus << "| fetchPos:" << m_fetchPos;
            m_redirectTarget.clear();
            m_redirectDepth = 0;
            m_redirectResolved = false; // let the warm-up probe in again
            releaseFetch();
            scheduleFetch(m_fetchPos);
            return;
        }

        const bool failed = finishedStatus == 0 || (finishedStatus != 206 && finishedStatus != 200);
        if (failed)
        {
            qWarning() << "[MpvHttpStreamRelay] upstream read failed"
                       << "| status:" << finishedStatus << "| error:" << reply->errorString()
                       << "| fetchPos:" << m_fetchPos;
        }
        else
        {
            qDebug() << "[MpvHttpStreamRelay] upstream read finished"
                     << "| status:" << finishedStatus << "| fetchPos:" << m_fetchPos
                     << "| fetchLimit:" << m_fetchLimit;
        }

        if (failed)
        {
            const auto sockets = m_connections.keys();
            for (QTcpSocket *socket : sockets)
            {
                auto it = m_connections.find(socket);
                if (it == m_connections.end() || !it->cacheMode || it->finished)
                {
                    continue;
                }
                if (!it->headersSent)
                {
                    writeError(socket, 502, "Upstream read failed");
                }
                else
                {
                    closeConnection(socket);
                }
            }
        }
        releaseFetch();
    }

    resumeStalledConnections();
}

void MpvHttpStreamRelay::releaseFetch()
{
    if (!m_fetch)
    {
        return;
    }
    QNetworkReply *reply = m_fetch;
    m_fetch = nullptr;
    disconnect(reply, nullptr, this, nullptr);
    if (!reply->isFinished())
    {
        reply->abort();
    }
    reply->deleteLater();
}

// Hand every pending client back to the original upstream URL instead of
// proxying for them. mpv follows redirects on its own, so from this point on it
// talks to the server directly -- exactly what it would have done with no relay
// at all. That is deliberate: the relay must never be the reason a media that
// was playable stops playing, whatever goes wrong on its own side.
void MpvHttpStreamRelay::redirectClientsUpstream()
{
    const auto sockets = m_connections.keys();
    for (QTcpSocket *socket : sockets)
    {
        auto it = m_connections.find(socket);
        if (it == m_connections.end() || !it->cacheMode || it->finished || it->headersSent)
        {
            continue;
        }
        it->cacheMode = false;
        it->finished = true;
        ++m_statRedirects;
        writeRedirect(socket, m_targetUrl);
    }
}

void MpvHttpStreamRelay::writeRedirect(QTcpSocket *socket, const QUrl &target)
{
    if (!socket || target.isEmpty())
    {
        closeConnection(socket);
        return;
    }

    QByteArray response = "HTTP/1.1 302 Found\r\n";
    response += "Location: " + target.toString(QUrl::FullyEncoded).toUtf8() + "\r\n";
    response += "Content-Length: 0\r\n";
    response += "Connection: close\r\n";
    response += "\r\n";

    if (canWriteToSocket(socket))
    {
        socket->write(response);
        socket->disconnectFromHost();
    }
    else
    {
        closeConnection(socket);
    }
}

// ---------------------------------------------------------------------------

void MpvHttpStreamRelay::writeError(QTcpSocket *socket, int statusCode, const QByteArray &message)
{
    if (!socket)
    {
        return;
    }

    const QByteArray body = message + "\n";
    QByteArray response = "HTTP/1.1 " + QByteArray::number(statusCode) + " " + reasonPhrase(statusCode) + "\r\n";
    response += "Content-Type: text/plain; charset=utf-8\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Connection: close\r\n\r\n";
    response += body;
    if (canWriteToSocket(socket))
    {
        socket->write(response);
        socket->disconnectFromHost();
    }
    else
    {
        closeConnection(socket);
    }
}

void MpvHttpStreamRelay::closeConnection(QTcpSocket *socket)
{
    if (!socket)
    {
        return;
    }

    const qint64 closeStartNs = monotonicNs();
    bool firstCall = false;
    auto it = m_connections.find(socket);
    if (it != m_connections.end())
    {
        firstCall = true;
        QNetworkReply *reply = it->reply;
        const bool wasCacheMode = it->cacheMode;
        // Snapshot the diagnostics while the entry is still alive.
        const qint64 acceptedNs = it->acceptedNs;
        const qint64 requestNs = it->requestNs;
        const qint64 headersNs = it->headersNs;
        const qint64 pumpWrites = it->pumpWrites;
        // Anything still queued for the socket at this point is data mpv never
        // read: it was written and counted, then dropped when the socket died.
        const qint64 pendingBytes = socket->bytesToWrite();
        it->reply = nullptr;
        m_connections.erase(it);

        if (reply)
        {
            disconnect(reply, nullptr, this, nullptr);
            if (!reply->isFinished())
            {
                reply->abort();
            }
            reply->deleteLater();
        }

        if (wasCacheMode)
        {
            // One aggregate line every kConnectionLogInterval connections: a
            // non-interleaved audio track opens tens of connections per second,
            // so a line per connection buries the rest of the log.
            // The three stage timings say where a connection's life goes:
            // accept->request (mpv was slow to send), request->headers (this
            // class was slow to answer) and headers->done (mpv consumed, or
            // walked away mid-transfer).
            ++m_statConnections;
            if (requestNs > 0 && acceptedNs > 0)
            {
                ++m_statTimedConnections;
                m_statAcceptToRequestNs += requestNs - acceptedNs;
                if (headersNs > 0)
                {
                    m_statRequestToHeadersNs += headersNs - requestNs;
                    m_statHeadersToDoneNs += monotonicNs() - headersNs;
                }
            }
            m_statPumpWrites += pumpWrites;
            m_statDiscardedBytes += pendingBytes;

            if (m_statConnections % kConnectionLogInterval == 0)
            {
                logActivitySummary();
            }
        }
    }

    disconnect(socket, nullptr, this, nullptr);
    if (socket->state() != QAbstractSocket::UnconnectedState)
    {
        socket->disconnectFromHost();
    }
    socket->deleteLater();
    if (firstCall)
    {
        // The actual destruction happens later via deleteLater(); this measures
        // the bookkeeping we do synchronously on the way out.
        m_statCloseNs += monotonicNs() - closeStartNs;
        ++m_statCloseCount;
    }
}

void MpvHttpStreamRelay::recordRelayedBytes(qint64 bytes)
{
    if (bytes > 0)
    {
        m_bytesRelayedSinceLastTick += bytes;
    }
}

// One-line report of cache activity. Emitted every kConnectionLogInterval
// connections and once more when the media is torn down, so even a short
// playback ends with a complete summary.
void MpvHttpStreamRelay::logActivitySummary()
{
    const qint64 connections = qMax<qint64>(1, m_statConnections);
    const qint64 timed = qMax<qint64>(1, m_statTimedConnections);
    qDebug() << "[MpvHttpStreamRelay] cache activity"
             << "| connections:" << m_statConnections
             << "| avgBytesPerConnection:" << (m_statBytesFromCache / connections)
             << "| discardedBytes:" << m_statDiscardedBytes
             << "| avgPumpWrites:" << (m_statPumpWrites / connections)
             << "| acceptToRequestUs:" << (m_statAcceptToRequestNs / timed / 1000)
             << "| requestToHeadersUs:" << (m_statRequestToHeadersNs / timed / 1000)
             << "| headersToDoneUs:" << (m_statHeadersToDoneNs / timed / 1000)
             << "| pumpCpuUs:" << (m_statPumpNs / connections / 1000)
             << "| writeCpuUs:" << (m_statWriteNs / connections / 1000)
             // Per-connection fixed cost: paid once per accepted connection no
             // matter how many bytes it moves. At ~500 connections/second this
             // is the part that does not shrink when the byte count shrinks.
             << "| accepts:" << m_statAcceptInitCount
             << "| acceptInitUs:" << (m_statAcceptInitNs / qMax<qint64>(1, m_statAcceptInitCount) / 1000)
             << "| closes:" << m_statCloseCount
             << "| closeUs:" << (m_statCloseNs / qMax<qint64>(1, m_statCloseCount) / 1000)
             << "| pumpCallsPerConn:" << (m_statPumpCalls / connections)
             // Request -> response headers, split into three nested stages:
             // readyRead (whole handler) > processUs (parse + dispatch) >
             // sendHeadersUs (build and write the 206 head).
             << "| readyReadUs:" << (m_statReadyReadNs / qMax<qint64>(1, m_statReadyReadCalls) / 1000)
             << "| readyReadCalls:" << m_statReadyReadCalls
             << "| processUs:" << (m_statProcessNs / qMax<qint64>(1, m_statHeaderPathCount) / 1000)
             << "| sendHeadersUs:" << (m_statSendHeadersNs / qMax<qint64>(1, m_statHeaderPathCount) / 1000)
             << "| headerPaths:" << m_statHeaderPathCount
             << "| turnarounds:" << m_statTurnarounds
             << "| turnaroundUs:" << (m_statTurnaroundNs / qMax<qint64>(1, m_statTurnarounds) / 1000)
             << "| cachedBytes:" << m_cachedBytes
             << "| redirects:" << m_statRedirects
             << "| fetches:" << m_statFetches
             << "| bytesFromCache:" << m_statBytesFromCache
             << "| bytesFromUpstream:" << m_statBytesFromUpstream;
}

bool MpvHttpStreamRelay::parseRangeHeader(const QByteArray &value, qint64 *begin, qint64 *end) const
{
    const QByteArray trimmed = value.trimmed();
    if (!trimmed.startsWith("bytes="))
    {
        return false;
    }
    const QByteArray spec = trimmed.mid(6).trimmed();
    const int comma = spec.indexOf(',');
    const QByteArray single = (comma >= 0) ? spec.left(comma) : spec;
    const int dash = single.indexOf('-');
    if (dash < 0)
    {
        return false;
    }

    const QByteArray left = single.left(dash).trimmed();
    const QByteArray right = single.mid(dash + 1).trimmed();
    bool ok = false;

    if (left.isEmpty())
    {
        // Suffix range ("bytes=-N"): needs the total size, which we only know
        // once the upstream headers have been seen.
        if (m_totalSize <= 0)
        {
            return false;
        }
        const qint64 suffix = right.toLongLong(&ok);
        if (!ok || suffix <= 0)
        {
            return false;
        }
        *begin = qMax<qint64>(0, m_totalSize - suffix);
        *end = m_totalSize - 1;
        return true;
    }

    const qint64 parsedBegin = left.toLongLong(&ok);
    if (!ok || parsedBegin < 0)
    {
        return false;
    }
    qint64 parsedEnd = -1;
    if (!right.isEmpty())
    {
        parsedEnd = right.toLongLong(&ok);
        if (!ok || parsedEnd < parsedBegin)
        {
            return false;
        }
    }
    *begin = parsedBegin;
    *end = parsedEnd;
    return true;
}

QByteArray MpvHttpStreamRelay::reasonPhrase(int statusCode)
{
    switch (statusCode)
    {
    case 200:
        return "OK";
    case 206:
        return "Partial Content";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 416:
        return "Range Not Satisfiable";
    case 431:
        return "Request Header Fields Too Large";
    case 502:
        return "Bad Gateway";
    case 503:
        return "Service Unavailable";
    default:
        return "Status";
    }
}

bool MpvHttpStreamRelay::isHopByHopHeader(QByteArray name)
{
    name = name.toLower();
    return name == "connection" || name == "keep-alive" || name == "proxy-authenticate" ||
           name == "proxy-authorization" || name == "te" || name == "trailer" || name == "transfer-encoding" ||
           name == "upgrade";
}
