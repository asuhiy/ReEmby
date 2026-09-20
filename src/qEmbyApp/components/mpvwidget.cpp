#include "mpvwidget.h"
#include "mpvhttpstreamrelay.h"
#include "../utils/logredactionutils.h"
#include <QOpenGLContext>
#include <QMetaObject>
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QSurfaceFormat>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include "api/proxymanager.h"
#include "config/config_keys.h"
#include "config/configstore.h"

#ifdef Q_OS_WIN
// standalone（wid）模式下需要把 mpv 的渲染子窗口尺寸对齐到 widget 客户区。
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>

// mpv 自建渲染窗口的窗口类名（mpv 源码 video/out/w32_common.c：
// `#define MPV_WINDOW_CLASS_NAME L"mpv"`，嵌入模式同样用该类）。
static const wchar_t *const kMpvWindowClass = L"mpv";
#endif

MpvWidget::MpvWidget(QWidget *parent, bool standalone)
    : QOpenGLWidget(parent), m_standalone(standalone), m_mpv_gl(nullptr) {

    // standalone 模式：强制原生 HWND 供 mpv（vo=gpu-next + wid）直绘。不创建
    // OpenGL render context，本 widget 仅充当 mpv 渲染目标容器。
    if (m_standalone) {
        setAttribute(Qt::WA_NativeWindow);
    }

    
    QSurfaceFormat format = this->format(); 
    
    format.setSwapInterval(1); 
    this->setFormat(format);

    m_controller = new MpvController(this);

    // relay 跑在独立线程（原因见 mpvwidget.h 里该成员的注释）。relay 自身不带
    // parent —— moveToThread 要求如此；它的子对象（QTcpServer / QNAM / QTimer）
    // 会一起搬过去，因此 listen 与全部连接处理都发生在那个线程。
    m_relayThread = new QThread(this);
    m_relayThread->setObjectName(QStringLiteral("MpvStreamRelay"));
    m_streamRelay = new MpvHttpStreamRelay();
    m_streamRelay->moveToThread(m_relayThread);
    m_relayThread->start();

    connect(m_streamRelay, &MpvHttpStreamRelay::upstreamSpeedChanged, this,
            &MpvWidget::networkSpeedChanged);

    
    connect(m_controller, &MpvController::positionChanged, this, &MpvWidget::positionChanged);
    connect(m_controller, &MpvController::durationChanged, this, &MpvWidget::durationChanged);
    connect(m_controller, &MpvController::playbackStateChanged, this, &MpvWidget::playbackStateChanged);
    connect(m_controller, &MpvController::errorOccurred, this, &MpvWidget::errorOccurred);

    
    // standalone 时把本 widget 的原生 HWND 作为 wid 传给 mpv，让其自建渲染。
    m_controller->init(m_standalone,
                       m_standalone ? reinterpret_cast<void *>(winId())
                                    : nullptr);

    if (m_standalone) {
        // mpv 的渲染子窗口在 VO 初始化（开始播放）后才创建，此时补一次渲染区同步。
        // 再补一次稍后的同步：某些片源首个视频帧/解码器初始化较晚，
        // 那时 mpv 才把子窗口建好。
        connect(m_controller, &MpvController::fileLoaded, this, [this]() {
            QTimer::singleShot(0, this, [this]() { syncStandaloneRenderArea(); });
            QTimer::singleShot(400, this, [this]() { syncStandaloneRenderArea(); });
        });
    }
}

MpvWidget::~MpvWidget() {
    
    
    
    if (QOpenGLContext *ctx = context()) {
        disconnect(ctx, &QOpenGLContext::aboutToBeDestroyed,
                   this, &MpvWidget::cleanupGL);
    }

    
    
    
    shutdown();

    // shutdown() 已在线程还活着时让它停下；这里收尾线程本身。顺序重要：先
    // quit/wait 再 delete —— 在所属线程之外销毁一个仍绑定该线程的对象是未定义
    // 行为，反过来线程已退出后直接 delete 则是安全的。
    if (m_relayThread) {
        m_relayThread->quit();
        m_relayThread->wait();
    }
    delete m_streamRelay;
    m_streamRelay = nullptr;

    if (m_controller) {
        m_controller->deleteLater();
        m_controller = nullptr;
    }
}


void MpvWidget::shutdown() {
    if (!m_controller) return;

    
    this->blockSignals(true);
    m_controller->blockSignals(true);

    
    
    m_controller->command(QVariantList{"stop"});
    stopRelay();
    if (m_usingStreamRelay) {
        m_usingStreamRelay = false;
        Q_EMIT relayActiveChanged(false);
    }

    cleanupGL();

    m_controller->forceCleanup();
}

void MpvWidget::stopRelay() {
    if (!m_streamRelay) {
        return;
    }
    // Blocking dispatch, so callers keep the old synchronous behaviour. Once the
    // thread is gone (destructor order) calling it directly is safe -- and a
    // blocking call into a thread without an event loop would deadlock.
    if (m_relayThread && m_relayThread->isRunning()) {
        QMetaObject::invokeMethod(m_streamRelay, [this]() { m_streamRelay->stop(); },
                                  Qt::BlockingQueuedConnection);
    } else {
        m_streamRelay->stop();
    }
}

void MpvWidget::cleanupGL() {
    if (m_mpv_gl) {
        mpv_render_context_set_update_callback(m_mpv_gl, nullptr, nullptr);

        
        
        
        if (isValid()) {
            makeCurrent();
            mpv_render_context_free(m_mpv_gl);
            doneCurrent();
        } else {
            qWarning() << "[MpvWidget] OpenGL surface is invalid; freeing MPV render context without makeCurrent";
            mpv_render_context_free(m_mpv_gl);
        }
        m_mpv_gl = nullptr;
        qInfo() << "[MpvWidget] MPV render context released with OpenGL context";
    }
}

void *MpvWidget::getProcAddress(void *ctx, const char *name) {
    Q_UNUSED(ctx);
    QOpenGLContext *glctx = QOpenGLContext::currentContext();
    return glctx ? reinterpret_cast<void *>(glctx->getProcAddress(QByteArray(name))) : nullptr;
}

void MpvWidget::onMpvRenderUpdate(void *ctx) {
    
    auto *self = static_cast<MpvWidget *>(ctx);
    QMetaObject::invokeMethod(self, "maybeUpdate", Qt::QueuedConnection);
}

void MpvWidget::maybeUpdate() {
    update();
}

void MpvWidget::initializeGL() {
    initializeOpenGLFunctions();

    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    if (ctx && ctx->format().majorVersion() < 2) {
        qCritical() << "Fatal Error: OpenGL version is too low.";
        emit errorOccurred(tr("当前环境缺乏必需的 OpenGL 硬件加速支持，视频渲染已禁用。"));
        return;
    }

    
    
    
    if (ctx) {
        connect(ctx, &QOpenGLContext::aboutToBeDestroyed,
                this, &MpvWidget::cleanupGL, Qt::DirectConnection);
    }

    
    
    if (m_mpv_gl) {
        mpv_render_context_set_update_callback(m_mpv_gl, nullptr, nullptr);
        mpv_render_context_free(m_mpv_gl);
        m_mpv_gl = nullptr;
    }

    mpv_opengl_init_params gl_init_params{
        getProcAddress,
        nullptr
    };

    mpv_render_param params[]{
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    const int createError = mpv_render_context_create(&m_mpv_gl, m_controller->mpv(), params);
    if (createError < 0) {
        qCritical() << "[MpvWidget] MPV render context creation failed"
                    << "| error:" << mpv_error_string(createError);
        emit errorOccurred(tr("OpenGL rendering initialization failed."));
        return;
    }

    mpv_render_context_set_update_callback(m_mpv_gl, onMpvRenderUpdate, this);

    qInfo() << "[MpvWidget] MPV render context initialized"
            << "| resumePending:" << m_resumeWhenRenderReady;

    if (!m_pendingUrl.isEmpty()) {
        loadMediaNow(m_pendingUrl, m_pendingServerId, true);
        m_pendingUrl.clear();
        m_pendingServerId.clear();
    }

    if (m_resumeWhenRenderReady) {
        m_resumeWhenRenderReady = false;
        m_controller->setProperty("pause", false);
    }

    update();
}

void MpvWidget::paintGL() {
    if (!m_mpv_gl) return;

    int w = static_cast<int>(width() * devicePixelRatio());
    int h = static_cast<int>(height() * devicePixelRatio());
    if (w == 0 || h == 0) return; 

    int fbo = defaultFramebufferObject();
    int flip_y = 1;

    mpv_opengl_fbo mpfbo{fbo, w, h, 0};

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &mpfbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    mpv_render_context_render(m_mpv_gl, params);
}

void MpvWidget::resizeGL(int w, int h) {
    Q_UNUSED(w);
    Q_UNUSED(h);
}

void MpvWidget::paintEvent(QPaintEvent *event) {
    // standalone 模式下 mpv 用 d3d11 直绘到本 widget 的原生 HWND，这里不走
    // QOpenGLWidget 的 GL 合成（否则会争抢同一 HWND、触发无谓的 GL 上下文）。
    if (m_standalone) {
        Q_UNUSED(event);
        return;
    }
    QOpenGLWidget::paintEvent(event);
}

void MpvWidget::resizeEvent(QResizeEvent *event) {
    QOpenGLWidget::resizeEvent(event);
    if (m_standalone) {
        syncStandaloneRenderArea();
    }
}

void MpvWidget::showEvent(QShowEvent *event) {
    QOpenGLWidget::showEvent(event);
    if (m_standalone) {
        // 首次显示后 widget 才有最终尺寸，且 mpv 的子窗口此时可能还没创建；
        // 排到事件循环之后再同步一次（幂等）。
        QTimer::singleShot(0, this, [this]() { syncStandaloneRenderArea(); });
    }
}

void MpvWidget::syncStandaloneRenderArea() {
#ifdef Q_OS_WIN
    if (!m_standalone) {
        return;
    }
    // 命名避免 parent/child —— 会遮蔽 QObject::parent()，且易与 Qt 成员混淆。
    const HWND parentHwnd = reinterpret_cast<HWND>(winId());
    if (!parentHwnd) {
        return;
    }
    // wid 模式下 mpv 会自建一个 WS_CHILD 渲染窗口，其客户区就是 mpv 的渲染区
    // （mpv 的 window_resize() 直接取该窗口的 client rect 作为 dwidth/dheight）。
    // mpv 只在收到父窗口的 WM_WINDOWPOSCHANGED（同进程 hook）/ WinEvent 时才跟随
    // 缩放，而 Qt 调整原生子窗口尺寸时这条通知并不总能到达 mpv。
    //
    // 实测（2026-09-11 截图像素量化）：拖动独立窗口后 mpv 侧仍停留在旧尺寸——
    // 渲染区比 widget 高约 205px，导致画面被裁掉下部、且不随窗口自适应（用户
    // 表现为"播放区没有自适应比例"）。这里在 widget 尺寸变化/首次显示/开始播放
    // 后直接把 mpv 子窗口设为 widget 客户区大小，等同于补一次漏掉的同步。
    // 幂等：尺寸一致时直接返回，不重复 SetWindowPos。
    // 注意：child 不能是 const —— 下面类名查找失败时还要再赋值一次。
    HWND child = FindWindowExW(parentHwnd, nullptr, kMpvWindowClass, nullptr);
    if (!child) {
        // 类名兜底（mpv 的窗口类名若有变化，就取第一个子窗口）。
        if (!(child = FindWindowExW(parentHwnd, nullptr, nullptr, nullptr))) {
            // mpv 的子窗口尚未创建（VO 未初始化），后续 resize/show/fileLoaded 会再调。
            return;
        }
    }
    RECT target{};
    if (!GetClientRect(parentHwnd, &target)) {
        return;
    }
    RECT current{};
    if (!GetClientRect(child, &current)) {
        return;
    }
    if (current.right == target.right && current.bottom == target.bottom) {
        return;
    }
    SetWindowPos(child, nullptr, 0, 0, target.right, target.bottom,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
#endif
}



void MpvWidget::setCustomUserAgent(const QString &userAgent) {
    m_customUserAgent = userAgent.trimmed();
}

void MpvWidget::setForceSoftwareDecode(bool force) {
    if (!m_controller)
        return;
    // hwdec 是运行时可改 option，下一次 loadfile 生效；每次 loadMedia 前
    // 都会显式调用，状态始终确定。RDP 会话基线是 "no"，恢复时不会破坏。
    const QString hwdec = force ? QStringLiteral("no")
                                : m_controller->effectiveHwdec();
    m_controller->setProperty(QStringLiteral("hwdec"), hwdec);
    qInfo().noquote() << "[MpvWidget] hwdec override for source"
                      << "| hwdec:" << hwdec;
}

void MpvWidget::loadMediaNow(const QString &url, const QString &serverId, bool wasPending) {



    const QUrl loadQUrl(url);
    const QNetworkProxy proxy =
        serverId.isEmpty()
            ? ProxyManager::instance()->resolveForUrl(loadQUrl)
            : ProxyManager::instance()->resolveForServerId(serverId);
    const QString scheme = loadQUrl.scheme().toLower();
    const bool isHttpStream =
        scheme == QStringLiteral("http") || scheme == QStringLiteral("https");
    // The relay is a local HTTP proxy that caches byte ranges; it is what makes
    // sources with a non-interleaved layout (audio stored apart from the video)
    // play smoothly, because it can answer mpv's small follow-up requests from
    // memory instead of paying a network round trip for each one. Those sources
    // are the minority, and for a normally interleaved file the relay is pure
    // overhead (a local hop plus its own thread for an already-sequential read
    // pattern), so it is off unless the user turns it on for such a file.
    ConfigStore *relayCfg = ConfigStore::instance();
    const bool relayEnabled = relayCfg->get<bool>(ConfigKeys::PlayerRelayEnabled, false);
    const bool shouldRelay = isHttpStream && relayEnabled;

    QString playbackUrl = url;
    bool usingRelay = false;
    if (shouldRelay && m_streamRelay) {
        const int readaheadMbCfg =
            relayCfg->get<int>(ConfigKeys::PlayerRelayReadaheadMb, 64);
        const int highWaterKbCfg =
            relayCfg->get<int>(ConfigKeys::PlayerRelayHighWaterKb, 2048);
        const int pumpChunkKbCfg =
            relayCfg->get<int>(ConfigKeys::PlayerRelayPumpChunkKb, 1024);
        MpvHttpStreamRelay::Tuning relayTuning;
        relayTuning.readaheadBytes =
            static_cast<qint64>(readaheadMbCfg > 0 ? readaheadMbCfg : 64) * 1024 * 1024;
        relayTuning.socketHighWaterBytes =
            static_cast<qint64>(highWaterKbCfg > 0 ? highWaterKbCfg : 2048) * 1024;
        relayTuning.pumpChunkBytes =
            static_cast<qint64>(pumpChunkKbCfg > 0 ? pumpChunkKbCfg : 1024) * 1024;
        // prepare() 必须同步返回本地 URL，所以用阻塞式队列调用把它交给 relay
        // 线程执行；主线程只等 listen() + 生成 URL（毫秒级）。参数按值捕获，
        // 避免工作线程去读主线程的成员。
        const QString relayUa = m_customUserAgent;
        QUrl localUrl;
        QMetaObject::invokeMethod(
            m_streamRelay,
            [this, &localUrl, loadQUrl, serverId, proxy, relayTuning, relayUa]() {
                localUrl = m_streamRelay->prepare(loadQUrl, serverId, proxy, relayUa, relayTuning);
            },
            Qt::BlockingQueuedConnection);
        if (localUrl.isValid()) {
            playbackUrl = localUrl.toString(QUrl::FullyEncoded);
            usingRelay = true;
        }
    } else if (m_streamRelay) {
        stopRelay();
    }
    if (m_usingStreamRelay != usingRelay) {
        m_usingStreamRelay = usingRelay;
        Q_EMIT relayActiveChanged(usingRelay);
    }

    // Strict-UA servers reject the default libmpv UA on stream requests too.
    const QString effectiveUa =
        m_customUserAgent.isEmpty() ? QStringLiteral("libmpv") : m_customUserAgent;
    m_controller->setProperty(QStringLiteral("user-agent"), effectiveUa);

    // Fast Start: lower the buffered-data threshold mpv waits for before
    // playback begins (cache-pause-wait). Default 1.0s; fast start uses 0.2s
    // so remote streams begin rendering sooner, at the cost of possible
    // early pauses on slow links. Read via ConfigStore (same store the
    // Settings UI writes to).
    {
        const bool fastStart = ConfigStore::instance()->get<bool>(
            ConfigKeys::PlayerFastStart, false);
        m_controller->setProperty(QStringLiteral("cache-pause-wait"),
                                  fastStart ? 0.2 : 1.0);
    }

    // Disk-backed demuxer cache (Settings -> Player -> "Disk Cache"):
    //  - when enabled, mpv writes the cache's packet data to disk instead of
    //    RAM (--cache-on-disk), which is what makes a very large
    //    demuxer-max-bytes affordable without the same cost in memory; the
    //    target directory comes from --demuxer-cache-dir. Cache files are
    //    deleted when the media is unloaded, so this is NOT a persistent
    //    cache and does not make replaying the same media start faster;
    //  - no manual flush is needed when switching media: the cache lives and
    //    dies with the demuxer, so loading a new file always starts clean.
    //  - the configured directory will look EMPTY in Explorer even while the
    //    cache is in active use: mpv creates "mpv-cache-XXXXXX.dat" and then
    //    immediately unlinks it (--demuxer-cache-unlink-files defaults to
    //    "immediate"). The file keeps occupying space with a live descriptor,
    //    it just has no directory entry. See demux/cache.c:demux_cache_create.
    //    To actually see the files, set demuxer-cache-unlink-files to
    //    "whendone" (delete at unmount) or "no" (keep them).
    {
        const bool diskCache = ConfigStore::instance()->get<bool>(
            ConfigKeys::PlayerDiskCache, false);
        if (diskCache) {
            // mpv only creates the disk cache when its seekable demuxer cache
            // is on: demux.c does `if (in->seekable_cache && opts->disk_cache
            // && !in->cache) demux_cache_create(...)`. We deliberately do NOT
            // force `cache` here: its default "auto" already resolves to "on
            // for network streams" (demux.c: `bool use_cache = is_streaming`),
            // which covers every source this app plays, and pinning it to
            // "yes" would also change read behaviour for local files. Just be
            // aware of the coupling: if `cache` is ever forced to "no", this
            // switch silently stops doing anything.
            m_controller->setProperty(QStringLiteral("cache-on-disk"),
                                      QStringLiteral("yes"));
            const QString cacheDir = ConfigStore::instance()->get<QString>(
                ConfigKeys::PlayerDiskCacheDir, QString());
            if (!cacheDir.isEmpty()) {
                QDir().mkpath(cacheDir);
                m_controller->setProperty(QStringLiteral("demuxer-cache-dir"),
                                          cacheDir);
            }
        } else {
            m_controller->setProperty(QStringLiteral("cache-on-disk"),
                                      QStringLiteral("no"));
        }
    }

    // Advanced mpv tuning (Settings -> Player). All values come from the
    // same ConfigStore the settings UI writes to; missing values fall back
    // to mpv defaults (we simply do not touch the property).
    {
        ConfigStore *cfg = ConfigStore::instance();

        const QString audioChannels =
            cfg->get<QString>(ConfigKeys::PlayerAudioChannels, QString()).trimmed();
        if (!audioChannels.isEmpty()) {
            m_controller->setProperty(QStringLiteral("audio-channels"), audioChannels);
        }

        if (cfg->has(ConfigKeys::PlayerAudioNormalizeDownmix)) {
            m_controller->setProperty(
                QStringLiteral("audio-normalize-downmix"),
                cfg->get<bool>(ConfigKeys::PlayerAudioNormalizeDownmix, true)
                    ? QStringLiteral("yes") : QStringLiteral("no"));
        }
        if (cfg->has(ConfigKeys::PlayerAudioExclusive)) {
            m_controller->setProperty(
                QStringLiteral("audio-exclusive"),
                cfg->get<bool>(ConfigKeys::PlayerAudioExclusive, false)
                    ? QStringLiteral("yes") : QStringLiteral("no"));
        }
        if (cfg->has(ConfigKeys::PlayerAudioStreamSilence)) {
            m_controller->setProperty(
                QStringLiteral("audio-stream-silence"),
                cfg->get<bool>(ConfigKeys::PlayerAudioStreamSilence, false)
                    ? QStringLiteral("yes") : QStringLiteral("no"));
        }
        if (cfg->has(ConfigKeys::PlayerStreamBufferSize)) {
            m_controller->setProperty(
                QStringLiteral("stream-buffer-size"),
                QString::number(cfg->get<int>(ConfigKeys::PlayerStreamBufferSize, 128))
                    + QStringLiteral("KiB"));
        }
        if (cfg->has(ConfigKeys::PlayerDemuxerMaxBytes)) {
            m_controller->setProperty(
                QStringLiteral("demuxer-max-bytes"),
                QString::number(cfg->get<int>(ConfigKeys::PlayerDemuxerMaxBytes, 1536))
                    + QStringLiteral("MiB"));
        }
        if (cfg->has(ConfigKeys::PlayerDemuxerMaxBackBytes)) {
            m_controller->setProperty(
                QStringLiteral("demuxer-max-back-bytes"),
                QString::number(cfg->get<int>(ConfigKeys::PlayerDemuxerMaxBackBytes, 0))
                    + QStringLiteral("MiB"));
        }
        if (cfg->has(ConfigKeys::PlayerDemuxerReadaheadSecs)) {
            m_controller->setProperty(
                QStringLiteral("demuxer-readahead-secs"),
                cfg->get<int>(ConfigKeys::PlayerDemuxerReadaheadSecs, 1));
        }
        // 注意：「网络后端（libcurl）」与「TCP keep-alive」两个开关在本项目使用的
        // mpv 版本里没有对应属性（`--list-options` 里只有 network-timeout），
        // 之前每次播放都会因此打印 setProperty failed 警告。这里不再发无效调用：
        // 这两项当前不生效，UI 上的开关仅为占位。
    }

    const QString mpvProxyValue =
        usingRelay ? QString() : ProxyManager::toMpvHttpProxy(proxy);
    const bool forceSeekable =
        !usingRelay && !mpvProxyValue.isEmpty() && scheme == QStringLiteral("http");

    m_controller->setProperty(QStringLiteral("http-proxy"), mpvProxyValue);
    m_controller->setProperty(QStringLiteral("force-seekable"), forceSeekable);

    QVariantMap loadOptions;
    if (!mpvProxyValue.isEmpty()) {
        loadOptions.insert(QStringLiteral("http-proxy"), mpvProxyValue);
    }
    if (!m_customUserAgent.isEmpty()) {
        loadOptions.insert(QStringLiteral("user-agent"), m_customUserAgent);
    }
    if (forceSeekable) {
        loadOptions.insert(QStringLiteral("force-seekable"), QStringLiteral("yes"));
    }

    if (!usingRelay && !mpvProxyValue.isEmpty() && scheme == QStringLiteral("https")) {
        qWarning() << "[MpvWidget] MPV http-proxy is not applied to HTTPS streams by libmpv"
                   << "| url:" << LogRedactionUtils::url(loadQUrl)
                   << "| serverId:" << (serverId.isEmpty()
                                           ? QStringLiteral("<none>")
                                           : serverId);
    }

    qInfo() << (wasPending
                    ? QStringLiteral("[MpvWidget] http-proxy applied (pending)")
                    : QStringLiteral("[MpvWidget] http-proxy applied"))
            << "| url:" << LogRedactionUtils::url(loadQUrl)
            << "| serverId:" << (serverId.isEmpty()
                                     ? QStringLiteral("<none>")
                                     : serverId)
            << "| mpvProxyValue:" << LogRedactionUtils::proxy(mpvProxyValue)
            << "| relay:" << usingRelay
            << "| playbackUrl:" << (usingRelay
                                       ? playbackUrl
                                       : QStringLiteral("<direct>"))
            << "| forceSeekable:" << forceSeekable;

    QVariantList loadCommand;
    if (loadOptions.isEmpty()) {
        loadCommand = QVariantList{QStringLiteral("loadfile"), playbackUrl};
    } else {
        loadCommand = QVariantList{QStringLiteral("loadfile"), playbackUrl,
                                   QStringLiteral("replace"), -1,
                                   loadOptions};
    }

    const int err = m_controller->command(loadCommand, nullptr);
    if (err < 0 && !loadOptions.isEmpty()) {
        qWarning() << "[MpvWidget] loadfile with indexed per-file network options failed, retrying legacy form"
                   << "| error:" << mpv_error_string(err);
        const int legacyErr = m_controller->command(
            QVariantList{QStringLiteral("loadfile"), playbackUrl,
                         QStringLiteral("replace"), loadOptions},
            nullptr);
        if (legacyErr < 0) {
            qWarning() << "[MpvWidget] legacy loadfile per-file options failed, retrying plain loadfile"
                       << "| error:" << mpv_error_string(legacyErr);
            m_controller->command(QVariantList{QStringLiteral("loadfile"), playbackUrl}, nullptr);
        }
    }
}

void MpvWidget::loadMedia(const QString &url, const QString &serverId) {
    
    if (m_standalone) {
        // standalone 无 render context，mpv 自建渲染，无需等待就绪即可加载。
        loadMediaNow(url, serverId, false);
        return;
    }
    if (!m_mpv_gl) {
        m_pendingUrl = url;
        m_pendingServerId = serverId;
        return;
    }
    loadMediaNow(url, serverId, false);
}

void MpvWidget::play() {
    m_controller->setProperty("pause", false);
}

void MpvWidget::resumeAfterContextRestore() {
    if (m_standalone) {
        m_controller->setProperty("pause", false);
        return;
    }

    if (!m_mpv_gl || !context() || !context()->isValid()) {
        m_resumeWhenRenderReady = true;
        qInfo() << "[MpvWidget] Resume deferred until OpenGL context is ready";
        update();
        return;
    }

    m_resumeWhenRenderReady = false;
    m_controller->setProperty("pause", false);
    update();
}

void MpvWidget::pause() {
    m_controller->setProperty("pause", true);
}

void MpvWidget::stop() {
    m_controller->command(QVariantList{"stop"});
}

void MpvWidget::seek(double positionInSeconds) {
    m_controller->command(QVariantList{"seek", positionInSeconds, "absolute"});
}
