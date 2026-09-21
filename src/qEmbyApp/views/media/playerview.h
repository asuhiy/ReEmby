#ifndef PLAYERVIEW_H
#define PLAYERVIEW_H

#include "../baseview.h"
#include "../../components/mpvwidget.h"
#include "../../components/modernslider.h"
#include "../../components/loadingoverlay.h" 
#include "../../components/playerdanmakucontroller.h"
#include <models/media/mediaitem.h>
#include <models/media/playbackinfo.h>
#include <services/introdb/introdbservice.h>
#include <services/trakt/traktservice.h>

#include <QVariant>
#include <qcorotask.h>

#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QPropertyAnimation>
#include <QParallelAnimationGroup>
#include <QTimer>
#include <QGraphicsOpacityEffect>
#include <QRect>
#include <QPoint>
#include <QMenu>
#include <QKeyEvent>
#include <QProgressBar>
#include <QHash>
#include <QPointer>
#include <QSet>

class QEmbyCore;
class PlayerOverlayDialog;
class PlayerMediaSwitcherPanel;
class PlayerOsdLayer;
class PlayerLongPressHandler;
class PlayerStatisticsOverlay;
class NativeDanmakuOverlay;

class PlayerView : public BaseView {
    Q_OBJECT
public:
    explicit PlayerView(QEmbyCore *core, QWidget *parent = nullptr,
                        bool standalone = false);
    ~PlayerView() override;
    void prepareForStackLeave() override;

    
    void playMedia(const QString &mediaId, const QString &title, const QString &streamUrl, long long startPositionTicks = 0, const QVariant& sourceInfoVar = QVariant(), bool allowSourceFetch = true);

    
    bool isMediaPlaying() const;
    void pausePlayback();
    void resumePlayback();
    void restoreAfterWindowShow(bool shouldResumePlaying);
    void applyDecodeDecision(const MediaSourceInfo &source);
    void stopAndReport(); 

signals:
    void playerChromeVisibilityChanged(bool visible);
    void playbackTitleChanged(const QString &title);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

private slots:
    void onPositionChanged(double position);
    void onDurationChanged(double duration);
    void onPlaybackStateChanged(bool isPaused);
    void onMpvPropertyChanged(const QString &property, const QVariant &value);
    
    void togglePlayPause();
    void onSliderMoved(int value);
    
    
    void seekRelative(double delta, bool silent = false);
    
    
    void toggleMute();
    void changeVolume(int delta, bool silent = false);
    void onVolumeSliderMoved(int value);

    void showSpeedMenu();
    void showAudioMenu();
    void showSubtitleMenu();
    // 二级字幕列表：一级菜单（主字幕/副字幕）悬停或点击后在其右侧弹出轨道
    // 列表（anchorY 为悬停项在面板内的 y，用于对齐；点击入口传 0）。
    void showSubtitleTrackSubmenu(bool secondary, int anchorY);
    void positionSubtitleSubmenu(int anchorY);
    void closeSubtitleSubmenu();
    void showDanmakuMenu();
    void showDanmakuIdentifyDialog();
    void loadLocalDanmakuFile();
    void loadExternalSubtitleFile();
    void openSubtitleSettingsDialog();
    void openDanmakuSettingsDialog();
    void openSkipSettingsDialog();
    void showSettingsMenu(); 
    
    void cycleVideoScale();
    void toggleFullscreenWindow();

    void showControls();
    void hideControls();
    void reportProgressToServer();
    void onBackClicked();

    // Trakt scrobble / resume-check helpers. All fire-and-forget and silent:
    // failures must never disturb playback.
    bool traktScrobbleActive() const;
    QCoro::Task<TraktMediaIds> traktEnsureIdsResolved();
    QCoro::Task<void> traktScrobbleAt(QString action);
    void traktOnProgressTick();
    void traktOnPauseStateChanged(bool isPaused);
    void traktOnPlaybackStopped();
    QCoro::Task<void> traktCheckResumeProgress();


    
    
    void updateLoadingState();
    void updateDanmakuButtonState();

private:
    void setupUi();
    // 独立播放窗口模式（B3）：把全部覆盖层（HUD/侧边栏/统计/加载/OSD/弹幕）
    // 迁进透明 HUD 顶层窗口 —— 原生子窗口（WS_CHILD）拿不到 per-pixel alpha，
    // 顶层窗口可以。z-order 靠 owned-window 关系（HUD 窗口永远在播放窗口之上）。
    void applyStandaloneOverlay();
    // 创建透明 HUD 顶层窗口（owned by 播放窗口；所有覆盖层的宿主，常显）。
    void ensureStandaloneHudWindow();
    // 把 HUD 透明窗口的几何同步到 PlayerView 客户区（全局坐标；origin 与客户区
    // 重合，因此覆盖层的相对坐标无需换算）。
    void syncStandaloneHudWindow();
    // 惰性创建统计面板的宿主顶层窗口，并把面板 reparent 进去。首次打开统计面板
    // 时调用。见 m_statisticsWindow 的注释（弹幕层的 Source 清屏会擦掉同 backing
    // store 的兄弟控件）。
    void ensureStatisticsWindow();
    // 把统计窗口的几何同步到统计面板在 HUD/客户区里的目标位置（全局坐标）。
    // 跟随主窗口移动/缩放/全屏以及 updateOverlayLayout()。
    void syncStatisticsWindow();
    // 诊断：确认 Qt 是否真的为 HUD 顶层窗口启用 WS_EX_LAYERED（per-pixel alpha
    // 路径）。只对顶层窗口调用 winId()——对普通子控件调用会强制创建原生窗口。
    void logStandaloneLayerDiagnostics();
    // 独立播放窗口模式：HUD 窗口常显（背景全透明），这里只切换"跟随鼠标活动"
    // 的元素（top/bottom HUD、台标、网速）；侧边栏/统计/加载/OSD 等各自独立显隐。
    void setStandaloneHudVisible(bool visible);
    // 独立播放窗口模式：把动态创建的弹层（菜单/设置对话框）迁进透明 HUD 窗口
    // （用全局坐标换算，兼容旧 parent 非本视图的情况）。
    void promoteStandaloneLayer(QWidget *layer);
    void updateTitleElision();
    void updateOverlayLayout();
    void clearMediaSwitcherCache();
    void applyMediaSwitcherMode();
    void updateMediaSwitcherButton();
    bool shouldShowDanmakuHudControls() const;
    bool useHudMediaSwitcher() const;
    QString formatMediaSwitcherPlaybackTitle(const MediaItem &item) const;
    bool findNextResumeMediaFromCache(QString &mediaId,
                                      QString &title,
                                      long long &startPositionTicks,
                                      bool skipCurrentSeries) const;
    void populateRightSidebarFromCache();
    void showHudMediaSwitcher();
    void hideHudMediaSwitcher();
    void syncHudMediaSwitcherContent();
    bool findAdjacentMediaFromCache(int direction, QString &mediaId,
                                    QString &title,
                                    long long &startPositionTicks) const;
    
    
    void setupRightSidebar();
    
    
    QCoro::Task<void> showRightSidebar();
    QCoro::Task<void> ensureMediaSwitcherDataLoaded();
    QCoro::Task<void> autoPlayNextMediaIfEnabled();
    QCoro::Task<void> switchFromMediaSwitcher(QString mediaId,
                                              QString title,
                                              long long startPositionTicks);
    QCoro::Task<void> ensureMediaSourcesThenPlay(QString mediaId,
                                                 QString title,
                                                 QString streamUrl,
                                                 long long startPositionTicks,
                                                 MediaSourceInfo currentSource,
                                                 QString serverId = QString());
    // Continuous-play optimization: negotiate the next episode's playback
    // source in the background while the current one plays, so the
    // auto-advance starts without waiting for another PlaybackInfo round trip.
    QCoro::Task<void> prefetchNextEpisodeSource();

    // 预热下一集的流连接：只请求首字节（getBytesLimited 内部会带
    // Range: bytes=0-0），把「切集时才付」的 DNS / TCP / TLS / CDN 首包
    // 开销提前付掉，让自动连播少等这几百毫秒到几秒。
    // 纯尽力而为：任何失败都静默忽略，正常播放路径完全不受影响。
    QCoro::Task<void> prewarmStreamConnection(MediaSourceInfo source);

    void hideRightSidebar(bool immediate = false);
    void setEffectivePlaybackSpeed(double speed);
    void handlePointerActivity(const QPoint &globalPos);
    // 字幕拖动（B 部分）：按下时命中字幕带进入待定（返回 true）；移动超过
    // 阈值后由 eventFilter 升级为拖动并实时写位置，松手落配置。
    bool beginSubtitleDragIfHit(const QPoint &globalPos);
    void updateSubtitleDrag(const QPoint &globalPos);
    void finishSubtitleDrag();
    void setCursorHidden(bool hidden);
    bool areControlsFullyVisible() const;
    void setPlayerChromeVisible(bool visible);

    void stopTransientUiAnimations(bool immediate = false);
    void beginViewTeardown();

    QPushButton* createHudButton(const QString& iconPath, const QSize& size = QSize(24, 24));
    QString formatTime(double seconds, double totalSeconds) const;
    void applySubtitleStyleSettings();
    void resumePlaybackAfterFinishedSeek();

    
    QString externalSubtitleConfigKey() const;
    QString readPersistedExternalSubtitle() const;
    void persistExternalSubtitle(const QString &absPath);
    void clearPersistedExternalSubtitle();
    void applyPersistedExternalSubtitleIfAny();
    QString openPlayerFileDialog(const QString &title, const QString &startDir,
                                 const QString &filter);
    
    int findSubtitleTrackIdByPath(const QString &absPath) const;
    
    void showToast(const QString& msg);
    void updateStatisticsDisplay();
    void setScaleIcon(); 
    QString formatDanmakuProviderLabel(QString provider) const;
    QString formatDanmakuSourceServiceLabel(QString provider,
                                            QString serverName) const;
    QString buildDanmakuSummaryText() const;
    QString buildDanmakuTooltipText() const;
    void closeActivePlayerDialog();
    void trackPlayerDialog(PlayerOverlayDialog *dialog);
    void updatePowerInhibition();
    QCoro::Task<void> requestIntroDBSegments();
    void checkAndSkipSegment(double position);
    // 解析手动跳过设置（按剧/按条目覆盖 > 全局默认），结果写入
    // m_manualIntroSec / m_manualOutroSec；播放开始与设置保存后调用。
    void refreshManualSkipSettings();

    void showCenteredPopup(QWidget* popup, QPushButton* btn); 
    QWidget* m_activePopup = nullptr; 
    // 字幕选择二级面板（主/副字幕轨道列表）：随一级弹层一同关闭；QPointer 防止
    // 底层 widget 被先行销毁时残留悬空指针。IsSecondary 记录当前面板内容归属
    // （悬停来回划动时可原地复用、只重新对齐位置）。
    QPointer<QWidget> m_subtitleSubmenu;
    bool m_subtitleSubmenuIsSecondary = false;
    QPointer<PlayerOverlayDialog> m_activePlayerDialog;

    
    static QCoro::Task<void> executeFetchLogo(QPointer<PlayerView> safeThis, QEmbyCore* core, QString mediaId, QString serverId = QString());
    static QCoro::Task<void> resolveDanmakuPlaybackContext(
        QPointer<PlayerView> safeThis,
        QPointer<QEmbyCore> core,
        QString mediaId,
        QString fallbackTitle,
        MediaSourceInfo sourceInfo,
        QString serverId = QString());

    MpvWidget *m_mpvWidget;
    // 独立播放（wid）模式下的透明 HUD 顶层窗口：所有覆盖层的宿主（top/bottom
    // HUD、台标、网速、侧边栏、统计、加载、OSD、弹幕层、动态弹层）。
    // 顶层窗口支持 per-pixel alpha（区别于 WS_CHILD 子窗口），QSS 的 rgba
    // 背景能真正透出 mpv 渲染的视频；窗口本身常显（背景全透明）。
    QWidget *m_hudWindow = nullptr;
    NativeDanmakuOverlay *m_nativeDanmakuOverlay = nullptr;
    // true = 独立播放窗口（mpv 走 gpu-next + wid 自建渲染，支持杜比视界 P5）。
    bool m_standalone = false;

    
    QWidget *m_topHUD;
    QWidget *m_bottomHUD;
    PlayerStatisticsOverlay *m_statisticsOverlay;
    // 统计面板的宿主顶层窗口（owned window）。
    //
    // 为什么不能只做 PlayerView/HUD 窗口的子控件：弹幕层（NativeDanmakuOverlay）
    // 是全窗口大小的兄弟控件，它每帧用 CompositionMode_Source + fillRect(透明)
    // 清掉自己整块区域来消除残影。Source 是「覆写」而非「混合」，而兄弟控件共用
    // 同一块 backing store ⇒ 它那次清屏会把统计面板刚画好的像素一起抹掉（表现为
    // 「打开弹幕就看不到统计信息」）。raise() 治不了：它只改「谁先画谁后画」，
    // 弹幕层后画就永远落在统计面板像素之上；关掉弹幕时统计会自己回来，也只是
    // 因为脏区重绘把它重新画了一遍。
    //
    // 让统计面板成为独立顶层窗口后它就有自己的 backing store，弹幕层的清屏再也
    // 波及不到它；再用 owned-window（transientParent）保证它恒在 HUD 窗口之上。
    // 这是顶层窗口（**不能**对它有 QWidget::setParent —— 那会降级成子控件，
    // 修复随即失效）；生命周期只用 QObject::setParent(this) 挂到本视图的对象树上，
    // 析构时与 HUD 窗口一起延迟删除。几何由 syncStatisticsWindow() 跟随主窗口。
    QWidget *m_statisticsWindow = nullptr;
    // 统计面板在"覆盖层坐标系"（= 本视图客户区）里的目标位置，由
    // updateOverlayLayout() 算出；syncStatisticsWindow() 据此换算成全局坐标。
    int m_statisticsOverlayX = 12;
    int m_statisticsOverlayY = 0;
    LoadingOverlay *m_loadingOverlay; 
    // 缓冲结束时不立刻隐藏转圈，先等一小会儿（见 updateLoadingState）。
    // mpv 的 paused-for-cache 在慢链路上会 true/false 反复跳，立刻隐藏会让
    // 转圈和画面交替出现 —— 看起来就是"一闪一闪"。
    QTimer *m_loadingHideTimer = nullptr;
    // 转圈当前是否处于显示状态（避免对已隐藏的 overlay 反复 stop）。
    bool m_loadingShown = false;

    
    QLabel *m_logoLabel;
    QGraphicsOpacityEffect *m_logoOpacity;

    
    QGraphicsOpacityEffect *m_speedOpacity;

    
    PlayerOsdLayer *m_osdLayer = nullptr;
    
    PlayerLongPressHandler *m_longPressHandler = nullptr;

    
    QWidget *m_rightSidebar;
    QWidget *m_rightTrigger;
    QPropertyAnimation *m_rightSidebarAnim;
    
    
    QLabel *m_sidebarTitleLabel;
    class QListWidget *m_resumeList;

    QPushButton *m_backBtn;
    QLabel *m_titleLabel;
    QPushButton *m_minBtn;
    QPushButton *m_maxBtn;
    QPushButton *m_closeBtn;
    QLabel *m_networkSpeedLabel;

    
    QLabel *m_currentTimeLabel;
    ModernSlider *m_progressSlider;
    QLabel *m_totalTimeLabel;

    
    QPushButton *m_prevMediaBtn;
    QPushButton *m_playPauseBtn;
    QPushButton *m_rewindBtn;
    QPushButton *m_forwardBtn;
    QPushButton *m_nextMediaBtn;
    
    
    QPushButton *m_volumeBtn;
    ModernSlider *m_volumeSlider;

    QLabel *m_toastLabel;
    QTimer *m_toastTimer;

    QPushButton *m_speedBtn = nullptr;
    QPushButton *m_mediaSwitchBtn = nullptr;
    QPushButton *m_audioBtn = nullptr;
    QPushButton *m_subtitleBtn = nullptr;
    QPushButton *m_danmakuBtn = nullptr;
    QPushButton *m_settingsBtn = nullptr;
    QPushButton *m_scaleBtn = nullptr;      
    QPushButton *m_fullscreenBtn = nullptr; 
    PlayerMediaSwitcherPanel *m_mediaSwitchDrawer = nullptr;
    int m_bottomHudBaseHeight = 110;

    PlayerDanmakuController *m_danmakuController = nullptr;

    QGraphicsOpacityEffect *m_topOpacity;
    QGraphicsOpacityEffect *m_bottomOpacity;
    QParallelAnimationGroup *m_fadeGroup;
    QTimer *m_hideTimer; 
    QTimer *m_reportTimer;
    QTimer *m_mousePollTimer;
    
    
    QTimer *m_bufferTimer;

    QString m_currentMediaId;
    QString m_currentMediaSourceId; 
    QString m_currentPlaySessionId; 
    MediaItem m_currentMediaItem;
    MediaSourceInfo m_currentMediaSourceInfo;
    
    
    QString m_originalStreamUrl; 
    QVariant m_currentSourceInfoVar; 

    bool m_isPlaying;
    bool m_isBuffering = false; 
    bool m_isSeeking = false;   
    bool m_playerChromeVisible = true;
    
    double m_currentPosition;
    double m_totalDuration = 0.0; 
    double m_pendingSeekSeconds = 0.0;
    bool m_windowRestorePending = false;
    bool m_windowRestoreShouldPlay = false;
    double m_osdSeekPreviewPosition = -1.0;
    
    double m_currentSpeed = 1.0;
    // 画幅模式：0=适应屏幕（保持比例，多余部分留黑边）/ 1=铺满裁剪 / 2=强制拉伸 /
    // 3=原始比例。默认取 0：与主流播放器（PotPlayer 等）一致——播放区随窗口尺寸
    // 自适应，比例不符的部分留黑边，而不是裁剪掉画面内容。
    int m_videoScaleMode = 0;
    
    
    double m_currentVolume = 100.0;
    bool m_isMuted = false;

    bool m_showNetworkSpeed = true; 
    bool m_useRelayNetworkSpeed = false;
    bool m_showStatisticsOverlay = false; 
    qint64 m_effectiveNetworkSpeed = 0;
    bool m_hasSetVideoSize = false; 
    bool m_hasReportedStop = false; 

    // Trakt scrobble state (per playback session; reset in playMedia)
    TraktMediaIds m_traktIds;
    QString m_traktResolvedMediaId;
    bool m_traktResolveInFlight = false;
    bool m_traktResumeChecked = false;
    bool m_traktStopped = false;
    qint64 m_traktLastScrobbleMs = 0;
    bool m_isPlaybackFinished = false;
    bool m_autoPlayAdvanceInProgress = false;
    bool m_isViewTearingDown = false;
    bool m_powerInhibitionHeld = false;
    bool m_isRightSidebarVisible = false; 
    
    
    QString m_switcherPendingItemId;
    QString m_switcherPendingTitle;
    long long m_switcherPendingTicks = 0;
    
    
    bool m_isSeriesMode = false;
    QString m_seriesId;
    QString m_seriesName;

    
    IntroDBService::EpisodeSegments m_episodeSegments;
    bool m_introSkipped = false;
    bool m_outroSkipped = false;
    bool m_segmentsRequested = false;

    // 手动跳过时长（秒，0 = 不跳过该项）；手动设置优先于 IntroDB 数据。
    int m_manualIntroSec = 0;
    int m_manualOutroSec = 0;

    
    QString m_switcherCacheMediaId;
    bool m_switcherCacheReady = false;
    QList<MediaItem> m_switcherResumeItems;
    QList<MediaItem> m_switcherSeriesSeasons;
    QHash<QString, QList<MediaItem>> m_switcherSeasonEpisodes;

    // Playback sources negotiated ahead of time for the upcoming episode
    // (continuous play); keyed by itemId. Bounded: cleared when it grows
    // beyond a few entries.
    QHash<QString, MediaSourceInfo> m_prefetchedSources;

    // Guards the progress-threshold prefetch trigger (one shot per media).
    bool m_prefetchTriggered = false;
    int m_prefetchThreshold = 90;

    // Pure-DV recheck after playback starts: list-style items arrive without
    // MediaStreams, so the initial hwdec decision has no data. Rechecked once
    // per media per playMedia invocation via item detail (reset on each
    // playMedia so switching back retries; reload path does not re-enter
    // playMedia, preventing loops).
    QSet<QString> m_dvRecheckPending;
    // Sticky software-decode decision for the current media. Sources without
    // MediaStreams (list/resume paths) can't be re-judged on later reloads
    // (window restore); without this sticky flag they would reset hwdec back
    // to hardware decoding and DV profile 5 would render green again.
    bool m_swDecodeForCurrentMedia = false;

    QString m_fullTitle;
    
    QRect m_originalGeometry;
    bool m_wasMaximized = false;
    QPoint m_dragPos;
    QPoint m_lastMousePos; 
    bool m_didDrag = false; 
    QTimer *m_singleClickTimer = nullptr; 

    // 字幕拖动（拖动开关开启时可用）：pending = 已按在字幕带内、等待移动
    // 阈值；active = 正在拖动。targetSecondary 区分拖的是副字幕还是内容主字幕
    // （决定松手写哪个配置键）；writesSecondaryPos 记录实际写入的 mpv 属性
    // ——ass-track 弹幕模式下内容主字幕物理上位于 secondary 通道，两者不同。
    bool m_subtitleDragPending = false;
    bool m_subtitleDragActive = false;
    bool m_subtitleDragTargetSecondary = false;
    bool m_subtitleDragWritesSecondaryPos = false;
    double m_subtitleDragStartPos = 0.0;
    double m_subtitleDragLastPos = 0.0;
    QPoint m_subtitleDragStartGlobalPos;
    int m_subtitleDragViewHeight = 0;

    
    int m_targetAudioStreamIndex = -2;
    int m_targetSubStreamIndex = -2;
};

#endif 
