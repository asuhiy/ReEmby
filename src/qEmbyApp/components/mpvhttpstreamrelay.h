#ifndef MPVHTTPSTREAMRELAY_H
#define MPVHTTPSTREAMRELAY_H

#include <QByteArray>
#include <QHash>
#include <QNetworkProxy>
#include <QObject>
#include <QUrl>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class QTcpServer;
class QTcpSocket;

// Local HTTP proxy sitting between mpv and the remote stream.
//
// This used to be a pure pass-through: every Range request from mpv was
// forwarded upstream verbatim, and the moment mpv closed its connection the
// upstream reply was aborted and whatever had already been downloaded was
// thrown away.
//
// That breaks sources whose audio track is stored apart from the video
// (non-interleaved layout, common for WEB-DL MP4). libavformat then asks for
// the audio in tiny chunks that alternate with the video, so every chunk costs
// a full round trip; when a round trip takes ~1.75s and one chunk only carries
// ~0.1s of audio, playback starves no matter how large the mpv buffers are.
//
// The relay now keeps a byte-range cache between mpv and upstream:
//   * upstream reads are shared by every mpv connection and keyed by absolute
//     file offset, so a read starting at X can serve any later request that
//     falls inside [X, X + readahead);
//   * when mpv disconnects mid-stream the upstream read is NOT aborted -- it
//     keeps going up to its limit and feeds the cache ("finish the read").
//     The next request for a nearby offset is answered from memory in
//     microseconds instead of paying another round trip.
class MpvHttpStreamRelay : public QObject {
    Q_OBJECT
public:
    // Operating parameters, all overridable from the config file so a slow
    // machine can trade throughput for CPU without a rebuild. Defaults match
    // the values that were tuned on the reference machine.
    struct Tuning {
        // How far past its start offset one upstream read may go (MiB-sized).
        qint64 readaheadBytes = 64 * 1024 * 1024;
        // How many bytes may sit in the socket's write queue before the relay
        // stops feeding that connection. Larger = mpv consumes more per
        // connection (fewer connections), but more data to drop on close.
        qint64 socketHighWaterBytes = 2 * 1024 * 1024;
        // Largest single write per event-loop turn. Keeping this close to what
        // the kernel socket buffer accepts avoids piling megabytes into Qt's
        // write buffer, where every drain shifts the remainder.
        qint64 pumpChunkBytes = 1024 * 1024;
    };

    explicit MpvHttpStreamRelay(QObject *parent = nullptr);
    ~MpvHttpStreamRelay() override;

    QUrl prepare(const QUrl &targetUrl, const QString &serverId,
                 const QNetworkProxy &proxy,
                 const QString &userAgent = QString(),
                 const Tuning &tuning = Tuning());
    void stop();

Q_SIGNALS:
    void upstreamSpeedChanged(qint64 bytesPerSecond);

private:
    // One contiguous run of cached bytes, keyed by absolute file offset. The
    // list is kept sorted by begin and adjacent/overlapping runs are merged.
    struct CacheBlock {
        qint64 begin = 0;
        QByteArray data;
        // Access heat: how many times mpv actually read bytes out of this
        // block, and when that last happened. It decides who gets evicted once
        // the cache is over budget. This replaces the old "farthest from the
        // upstream read position" rule, which assumed a single, monotonically
        // forward-moving hot spot -- a wrong assumption for non-interleaved
        // files, where the audio track sits at the end of the file and is
        // requested hundreds of times per second while being the "farthest"
        // block every single time. See tools/relay-eviction-design.md.
        quint32 hits = 0;
        qint64 lastHitNs = 0;
    };

    struct ConnectionState {
        QByteArray buffer;
        bool headOnly = false;
        bool headersSent = false;
        bool finished = false; // response fully written, socket is closing

        // Pass-through mode: HEAD requests, or an upstream that ignores Range.
        bool cacheMode = false;
        QNetworkReply *reply = nullptr;
        bool upstreamFinished = false;

        // Cached mode.
        qint64 reqBegin = 0;       // absolute offset mpv asked for
        qint64 reqEnd = -1;        // inclusive; -1 = open ended (through EOF)
        qint64 needPos = 0;        // next absolute offset still to be sent
        qint64 servedFromCache = 0;

        // Per-connection diagnostics: where the time goes, and how much of what
        // was written mpv actually read (the rest is dropped when it closes).
        qint64 acceptedNs = 0;
        qint64 requestNs = 0;
        qint64 headersNs = 0;
        qint64 writtenBytes = 0;
        int pumpWrites = 0;           // write iterations inside one pump pass
        int pumpCalls = 0;            // how many times pumpCacheToSocket ran
        qint64 lastPumpReturnNs = 0;  // when the last pump pass returned
    };

    void onNewConnection();
    void onSocketReadyRead(QTcpSocket *socket);
    void onSocketDisconnected(QTcpSocket *socket);
    void processRequest(QTcpSocket *socket, const QByteArray &requestData);

    // --- pass-through path (fallback, unchanged behaviour) ---
    void startPassThrough(QTcpSocket *socket, const QByteArray &rangeHeader,
                          const QByteArray &acceptHeader, bool headOnly);
    void sendReplyHeaders(QTcpSocket *socket);
    void pumpReplyToSocket(QTcpSocket *socket);

    // --- cached path ---
    void resetCache();
    void appendToCache(const QByteArray &data);
    void normalizeCache();
    void recountCachedBytes();
    void evictCacheIfNeeded();
    int cacheBlockContaining(qint64 pos) const;
    void pumpCacheToSocket(QTcpSocket *socket);
    void pumpCacheToSocketImpl(QTcpSocket *socket);
    void sendCacheHeaders(QTcpSocket *socket);
    void finishCacheConnection(QTcpSocket *socket);
    void broadcastCachedData();

    // --- shared upstream read ---
    bool startFetch(qint64 pos);
    void ensureFetchFrom(qint64 pos);
    void scheduleFetch(qint64 pos);
    void runScheduledFetch();
    void resumeStalledConnections();
    bool followRedirect(QNetworkReply *reply);
    void warmUpstreamRedirects();
    void resolveRedirectStep(const QUrl &url, int depth);
    QUrl effectiveUpstreamUrl() const;
    void parseFetchHeaders();
    void onFetchReadyRead();
    void onFetchFinished();
    void releaseFetch();
    void redirectClientsUpstream();
    void writeRedirect(QTcpSocket *socket, const QUrl &target);

    void writeError(QTcpSocket *socket, int statusCode, const QByteArray &message);
    void closeConnection(QTcpSocket *socket);
    void recordRelayedBytes(qint64 bytes);
    void logActivitySummary();

    bool parseRangeHeader(const QByteArray &value, qint64 *begin, qint64 *end) const;
    static QByteArray reasonPhrase(int statusCode);
    static bool isHopByHopHeader(QByteArray name);

    QTcpServer *m_server = nullptr;
    QNetworkAccessManager *m_network = nullptr;
    QTimer *m_speedTimer = nullptr;
    QHash<QTcpSocket *, ConnectionState> m_connections;
    QUrl m_targetUrl;
    QString m_serverId;
    QString m_streamToken;
    QString m_upstreamUserAgent;
    // Resolved redirect target. QNetworkAccessManager's default redirect policy
    // (NoLessSafeRedirectPolicy) refuses the https -> http hop this server chain
    // needs -- Emby redirects to an OpenList direct link, which redirects again
    // to a pre-signed object-storage URL. Redirects are therefore followed by
    // hand, and the resolved target is reused for the later reads of the same
    // media (it is dropped again when it starts being rejected, since the
    // pre-signed URL expires).
    QUrl m_redirectTarget;
    int m_redirectDepth = 0;
    // Background probe that resolves the redirect chain before mpv asks for
    // anything, so the first request can go straight to the final URL.
    QNetworkReply *m_redirectProbe = nullptr;
    // Set only when the probe itself reached the final URL. The read path also
    // fills m_redirectTarget, one hop at a time, so that member cannot be used
    // to tell whether the chain has been walked to the end -- using it as the
    // guard silently cancelled the warm-up on every single playback.
    bool m_redirectResolved = false;
    // Time origin for the start-up diagnostics: how long the relay took to put
    // the first upstream byte in front of mpv. -1 means "not armed"; 0 is a
    // legitimate value, because QElapsedTimer returns 0 on its first call.
    qint64 m_preparedNs = -1;
    bool m_firstByteLogged = false;
    // Set when a redirect has been detected but its retry has not started yet:
    // the redirect's own body must never be read into the cache (that would
    // advance m_fetchPos past the offset mpv actually asked for), and the same
    // reply must not be parsed twice.
    bool m_redirectPending = false;
    qint64 m_bytesRelayedSinceLastTick = 0;

    // Byte-range cache (memory only for now; see tools/relay-design.md).
    QVector<CacheBlock> m_cache;
    qint64 m_cachedBytes = 0;
    qint64 m_totalSize = -1;          // from upstream Content-Range; -1 = unknown
    QByteArray m_contentType;
    bool m_rangeUnsupported = false;  // upstream answered 200 to a Range request

    // Shared upstream read: one at a time for the whole media.
    QNetworkReply *m_fetch = nullptr;
    qint64 m_fetchPos = 0;            // next absolute offset to be appended
    qint64 m_fetchRequestPos = 0;     // offset the in-flight read was issued for
    qint64 m_fetchLimit = 0;          // read no further than this
    qint64 m_readaheadBytes = 0;
    // Effective write tuning for the current media, taken from Tuning.
    qint64 m_socketHighWaterBytes = 2 * 1024 * 1024;
    qint64 m_pumpChunkBytes = 1024 * 1024;
    qint64 m_scheduledFetchPos = -1;
    bool m_schedulePending = false;

    // Diagnostics, logged in aggregate -- never per chunk.
    qint64 m_statFetches = 0;
    qint64 m_statCacheRequests = 0;
    qint64 m_statConnections = 0;
    qint64 m_statBytesFromCache = 0;
    qint64 m_statBytesFromUpstream = 0;
    // Per-connection timing (nanoseconds, summed) and volume breakdown.
    qint64 m_statTimedConnections = 0;
    qint64 m_statAcceptToRequestNs = 0;
    qint64 m_statRequestToHeadersNs = 0;
    qint64 m_statHeadersToDoneNs = 0;
    qint64 m_statPumpWrites = 0;
    qint64 m_statDiscardedBytes = 0; // written into the socket, dropped at close
    // CPU attribution: time inside the two calls that dominate the
    // per-connection cost, summed over every invocation (nanoseconds).
    qint64 m_statPumpNs = 0;  // pumpCacheToSocket, including its turnarounds
    qint64 m_statWriteNs = 0; // QAbstractSocket::write() only
    // Per-connection fixed cost: everything that is paid once per accepted
    // connection regardless of how many bytes it transfers. With ~500
    // connections/second this is what dominates once the byte shuffling is
    // under control.
    qint64 m_statAcceptInitNs = 0; // nextPendingConnection() + signal wiring
    qint64 m_statAcceptInitCount = 0;
    qint64 m_statCloseNs = 0;      // closeConnection cleanup, first call per socket
    qint64 m_statCloseCount = 0;
    qint64 m_statTurnaroundNs = 0; // pump pass return -> next bytesWritten
    qint64 m_statTurnarounds = 0;
    qint64 m_statPumpCalls = 0;
    // The "request -> response headers" path, split into its three nested
    // stages. They nest: readyRead contains processRequest, which contains
    // sendCacheHeaders, so the log prints all three and the reader subtracts.
    qint64 m_statReadyReadNs = 0;
    qint64 m_statReadyReadCalls = 0;
    qint64 m_statProcessNs = 0;
    qint64 m_statSendHeadersNs = 0;
    qint64 m_statHeaderPathCount = 0;
    qint64 m_statRedirects = 0;      // client connections handed back to upstream
};

#endif
