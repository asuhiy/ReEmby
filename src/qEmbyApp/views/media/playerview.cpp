#include "playerview.h"
#include "../../utils/qcoroutil.h"
#include "../../components/modernscrollpanel.h"
#include "../../components/nativedanmakuoverlay.h"
#include "../../components/playerdanmakuidentifydialog.h"
#include "../../components/playerdanmakusettingsdialog.h"
#include "../../components/seriesdanmakumatchdialog.h"
#include "../../components/playerlongpresshandler.h"
#include "../../components/playermediaswitcherpanel.h"
#include "../../components/playerosdlayer.h"
#include "../../components/playerstatisticsoverlay.h"
#include "../../components/playersubtitlesettingsdialog.h"
#include "../../components/skipsettingsdialog.h"
#include "../../utils/mediaitemutils.h"
#include "../../utils/dvdetectionutils.h"
#include "../../utils/mediasourcepreferenceutils.h"
#include "../../utils/playerpreferenceutils.h"
#include "../../utils/powerinhibitutils.h"
#include "../../utils/subtitlestyleutils.h"
#include "../../managers/playbackmanager.h"
#include "qembycore.h"
#include "../../../qEmbyCore/api/networkmanager.h"
#include <QUrl>
#include <QAbstractAnimation>
#include <QApplication>
#include <QCursor>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QTimer>
#include <QFontMetrics>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPalette>
#include <QPointer> 
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QTime>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>
#include <fileutils.h>

#include <QVector>
#include <QDateTime>
#include <QWindow>
#include <cmath>
#include <config/config_keys.h>
#include <config/configstore.h>
#include <services/skip/skipsegmentsstore.h>
#include <initializer_list>
#include <models/media/playbackinfo.h>
#include <models/media/playerlaunchcontext.h>
#include <models/profile/serverprofile.h>
#include <services/danmaku/danmakuservice.h>
#include <services/manager/servermanager.h>
#include <services/media/mediaservice.h>

#ifdef Q_OS_WIN
// 仅用于诊断：检查 Qt 是否为 standalone 覆盖层真正启用了 WS_EX_LAYERED
// （per-pixel alpha 合成的前提）。
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace
{
constexpr int kHudAutoHideDelayMs = 1800;
// 停止缓冲后，转圈再停留多久才淡出。mpv 的 paused-for-cache 在慢链路上会在
// true/false 之间反复跳（短暂拿到数据就解除、缓冲耗尽又进入），如果每次都
// 立刻隐藏转圈，画面就会"一闪一闪"。给一点宽限期让抖动被吸收掉。
constexpr int kLoadingHideDelayMs = 400;
// 独立播放窗口（standalone）下 HUD 是原生窗口、靠 setVisible 显隐，停留时间
// 比内嵌略长一点。
constexpr int kStandaloneHudAutoHideDelayMs = 2000;
// 半透明改走 Qt 属性链路（WA_TranslucentBackground → 内部 per-pixel alpha
// 合成）；不再使用 SetLayeredWindowAttributes。kStandaloneLayerAlpha 作为
// 备查留作 historical note —— 启用 LWA 路径时取消下面这行的注释即可。
// constexpr int kStandaloneLayerAlpha = 200;

// 纯 Dolby Vision（profile 5，无 HDR10/SDR 兼容层）。硬解会把携带 DV
// 元数据的 RPU NAL 丢弃，mpv 无法应用 fallback 色彩映射 → 画面发绿。
// 这类片源必须软解（mpv 0.40+ 的 vo_gpu fallback colorspace 修复生效）。
// Emby 字段语义：VideoRangeType "DOVI" = 纯 DV；"DOVIWithHDR10/WithSDR/
// WithHLG" = 有兼容层的 hybrid，硬解颜色正常，不做覆盖。
// 旧版 Emby 只有 VideoRange（值 "DOVI"/"DolbyVision"，无法区分 profile）时，
// 保守按纯 DV 处理（误伤 hybrid 只多耗 CPU，漏判则发绿不可看）。
bool sourceNeedsForcedSoftwareDecode(const MediaSourceInfo &source)
{
    // 实现在 DvDetectionUtils（供 PlaybackManager 的 DV 自动独立窗口共用）。
    return DvDetectionUtils::isPureDolbyVision(source);
}

// 判定数据是否可用：没有任何 Video 流说明 sourceInfo 来自列表 API
//（不带 MediaStreams），初始 hwdec 决策没有依据，需要拉 detail 复查。
bool hasVideoStreamData(const MediaSourceInfo &source)
{
    // 实现在 DvDetectionUtils（供 PlaybackManager 的 DV 自动独立窗口共用）。
    return DvDetectionUtils::hasVideoStreamData(source);
}


bool isDanmakuEnabledConfigKey(const QString &key)
{
    static const QString kDanmakuEnabledSuffix =
        QStringLiteral("/") + QString::fromLatin1(ConfigKeys::PlayerDanmakuEnabled);
    return key == QLatin1String(ConfigKeys::PlayerDanmakuEnabled) || key.endsWith(kDanmakuEnabledSuffix);
}

QString trimOrDash(QString value)
{
    value = value.trimmed();
    return value.isEmpty() ? QStringLiteral("-") : value;
}

QString firstNonEmpty(const QStringList &values)
{
    for (QString value : values)
    {
        value = value.trimmed();
        if (!value.isEmpty())
        {
            return value;
        }
    }
    return {};
}

QString joinNonEmpty(const QStringList &values, const QString &separator)
{
    QStringList filtered;
    for (QString value : values)
    {
        value = value.trimmed();
        if (!value.isEmpty())
        {
            filtered.append(value);
        }
    }
    return filtered.join(separator);
}

QString stripTrailingZeros(QString text)
{
    if (!text.contains(QLatin1Char('.')))
    {
        return text;
    }

    while (text.endsWith(QLatin1Char('0')))
    {
        text.chop(1);
    }
    if (text.endsWith(QLatin1Char('.')))
    {
        text.chop(1);
    }
    return text;
}

QString formatBitrateValue(qint64 bitsPerSecond)
{
    if (bitsPerSecond <= 0)
    {
        return {};
    }

    if (bitsPerSecond < 1000000)
    {
        return QStringLiteral("%1 kbps").arg(stripTrailingZeros(QString::number(bitsPerSecond / 1000.0, 'f', 1)));
    }

    return QStringLiteral("%1 Mbps").arg(stripTrailingZeros(QString::number(bitsPerSecond / 1000000.0, 'f', 2)));
}

QString formatDataRateValue(qint64 bytesPerSecond)
{
    if (bytesPerSecond <= 0)
    {
        return QStringLiteral("0.0 KB/s");
    }

    if (bytesPerSecond < 1024 * 1024)
    {
        return QStringLiteral("%1 KB/s").arg(stripTrailingZeros(QString::number(bytesPerSecond / 1024.0, 'f', 1)));
    }

    return QStringLiteral("%1 MB/s").arg(stripTrailingZeros(QString::number(bytesPerSecond / 1048576.0, 'f', 1)));
}

QString formatFrameRateValue(double fps)
{
    if (!std::isfinite(fps) || fps <= 0.0)
    {
        return {};
    }

    return QStringLiteral("%1 fps").arg(stripTrailingZeros(QString::number(fps, 'f', fps >= 100.0 ? 1 : 3)));
}

QString formatDurationValue(double seconds)
{
    if (!std::isfinite(seconds) || seconds <= 0.0)
    {
        return {};
    }

    return QStringLiteral("%1 s").arg(stripTrailingZeros(QString::number(seconds, 'f', seconds >= 100.0 ? 0 : 1)));
}

QString formatAvSyncValue(double seconds)
{
    if (!std::isfinite(seconds))
    {
        return {};
    }

    const double milliseconds = seconds * 1000.0;
    return QStringLiteral("%1%2 ms")
        .arg(milliseconds >= 0.0 ? QStringLiteral("+") : QString())
        .arg(stripTrailingZeros(QString::number(milliseconds, 'f', 1)));
}

QString formatLevelValue(int level)
{
    if (level <= 0)
    {
        return {};
    }

    if (level % 10 == 0)
    {
        return QString::number(level / 10);
    }

    return QStringLiteral("%1.%2").arg(level / 10).arg(level % 10);
}

QString formatDimensionValue(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return {};
    }

    return QStringLiteral("%1 x %2").arg(width).arg(height);
}

QString formatAspectValue(double width, double height, QString explicitAspect = {})
{
    explicitAspect = explicitAspect.trimmed();
    if (!explicitAspect.isEmpty())
    {
        return explicitAspect;
    }

    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0 || height <= 0.0)
    {
        return {};
    }

    const double ratio = width / height;
    struct KnownRatio
    {
        double value;
        const char *label;
    };

    static const KnownRatio knownRatios[] = {
        {4.0 / 3.0, "4:3"}, {16.0 / 9.0, "16:9"}, {21.0 / 9.0, "21:9"}, {1.0, "1:1"}};

    for (const KnownRatio &known : knownRatios)
    {
        if (std::fabs(ratio - known.value) < 0.03)
        {
            return QString::fromLatin1(known.label);
        }
    }

    return QStringLiteral("%1:1").arg(stripTrailingZeros(QString::number(ratio, 'f', 2)));
}

qint64 variantToLongLong(const QVariant &value)
{
    bool ok = false;
    const qint64 result = value.toLongLong(&ok);
    return ok ? result : 0;
}

double variantToDouble(const QVariant &value)
{
    bool ok = false;
    const double result = value.toDouble(&ok);
    return ok ? result : 0.0;
}

const MediaStreamInfo *findFirstStreamByType(const MediaSourceInfo &source, const QString &type)
{
    for (const MediaStreamInfo &stream : source.mediaStreams)
    {
        if (stream.type.compare(type, Qt::CaseInsensitive) == 0)
        {
            return &stream;
        }
    }
    return nullptr;
}

QVariantMap findSelectedTrackByType(const QVariantList &tracks, const QString &type)
{
    for (const QVariant &trackValue : tracks)
    {
        const QVariantMap track = trackValue.toMap();
        if (track.value(QStringLiteral("type")).toString() == type && track.value(QStringLiteral("selected")).toBool())
        {
            return track;
        }
    }
    return {};
}

bool hasValidAnimationTarget(const QPropertyAnimation *animation)
{
    return animation && animation->targetObject();
}

bool fadeGroupHasValidTargets(const QParallelAnimationGroup *group)
{
    if (!group)
    {
        return false;
    }

    for (int i = 0; i < group->animationCount(); ++i)
    {
        const auto *animation = qobject_cast<QPropertyAnimation *>(group->animationAt(i));
        if (!hasValidAnimationTarget(animation))
        {
            return false;
        }
    }

    return true;
}

void stopPropertyAnimationSafely(QPropertyAnimation *animation)
{
    if (!hasValidAnimationTarget(animation) || animation->state() == QAbstractAnimation::Stopped)
    {
        return;
    }

    animation->stop();
}

void stopFadeGroupSafely(QParallelAnimationGroup *group)
{
    if (!group || group->state() == QAbstractAnimation::Stopped || !fadeGroupHasValidTargets(group))
    {
        return;
    }

    group->stop();
}

void detachPropertyAnimationTarget(QPropertyAnimation *animation)
{
    if (!animation)
    {
        return;
    }

    if (animation->state() != QAbstractAnimation::Stopped)
    {
        animation->stop();
    }

    animation->setTargetObject(nullptr);
}

void detachFadeGroupTargets(QParallelAnimationGroup *group)
{
    if (!group)
    {
        return;
    }

    if (group->state() != QAbstractAnimation::Stopped)
    {
        group->stop();
    }

    for (int i = 0; i < group->animationCount(); ++i)
    {
        auto *animation = qobject_cast<QPropertyAnimation *>(group->animationAt(i));
        detachPropertyAnimationTarget(animation);
    }
}
} 

PlayerView::PlayerView(QEmbyCore *core, QWidget *parent, bool standalone)
    : BaseView(core, parent), m_isPlaying(false), m_currentPosition(0.0), m_totalDuration(0.0), m_activePopup(nullptr), m_standalone(standalone)
{

    setProperty("isImmersive", true);
    setMouseTracking(true);
    setAutoFillBackground(true);
    setFocusPolicy(Qt::StrongFocus); 

    m_isRightSidebarVisible = false; 

    m_hideTimer = new QTimer(this);
    m_hideTimer->setSingleShot(true);
    connect(m_hideTimer, &QTimer::timeout, this, &PlayerView::hideControls);

    m_reportTimer = new QTimer(this);
    m_reportTimer->setInterval(10000); 
    connect(m_reportTimer, &QTimer::timeout, this, &PlayerView::reportProgressToServer);

    
    m_longPressHandler = new PlayerLongPressHandler(this);
    connect(m_longPressHandler, &PlayerLongPressHandler::seekRequested, this, &PlayerView::seekRelative);
    connect(m_longPressHandler, &PlayerLongPressHandler::toastRequested, this, &PlayerView::showToast);

    
    m_bufferTimer = new QTimer(this);
    m_bufferTimer->setInterval(500);
    connect(m_bufferTimer, &QTimer::timeout, this,
            [this]()
            {
                if (m_totalDuration > 0 && m_mpvWidget && m_mpvWidget->controller())
                {
                    double bufferedPos = m_currentPosition;

                    
                    QVariant stateVar = m_mpvWidget->controller()->getProperty("demuxer-cache-state");
                    if (stateVar.isValid() && stateVar.metaType().id() == QMetaType::QVariantMap)
                    {
                        auto ranges = stateVar.toMap()["seekable-ranges"].toList();
                        if (!ranges.isEmpty())
                        {
                            
                            bufferedPos = ranges.last().toMap()["end"].toDouble();
                        }
                    }
                    else
                    {
                        
                        double cacheDuration =
                            m_mpvWidget->controller()->getProperty("demuxer-cache-duration").toDouble();
                        if (cacheDuration > 0)
                        {
                            bufferedPos = m_currentPosition + cacheDuration;
                        }
                    }

                    if (bufferedPos > m_totalDuration)
                    {
                        bufferedPos = m_totalDuration;
                    }

                    
                    m_progressSlider->setBufferValue(static_cast<int>(bufferedPos));
                }

                if (m_showStatisticsOverlay && m_statisticsOverlay)
                {
                    updateStatisticsDisplay();
                }
            });

    
    m_mousePollTimer = new QTimer(this);
    m_mousePollTimer->setInterval(100);
    connect(m_mousePollTimer, &QTimer::timeout, this,
            [this]()
            {
                if (!m_isPlaying)
                {
                    if (m_topOpacity->opacity() < 1.0)
                    {
                        showControls();
                    }
                    return;
                }

                if (areControlsFullyVisible() && this->cursor().shape() != Qt::BlankCursor &&
                    (!m_mpvWidget || m_mpvWidget->cursor().shape() != Qt::BlankCursor))
                {
                    return;
                }

                QPoint currentPos = QCursor::pos();
                if (currentPos == m_lastMousePos)
                {
                    return; 
                }

                m_lastMousePos = currentPos;

                
                if (this->window()->isMinimized() || !this->isVisible())
                {
                    return;
                }

                QPoint localPos = this->mapFromGlobal(currentPos);
                bool isMouseInside = this->rect().contains(localPos);

                
                if (isMouseInside)
                {
                    handlePointerActivity(currentPos);
                }
            });

    m_toastTimer = new QTimer(this);
    m_toastTimer->setSingleShot(true);
    connect(m_toastTimer, &QTimer::timeout, this, [this]() { m_toastLabel->hide(); });

    
    m_singleClickTimer = new QTimer(this);
    m_singleClickTimer->setSingleShot(true);
    m_singleClickTimer->setInterval(200);
    connect(m_singleClickTimer, &QTimer::timeout, this, &PlayerView::togglePlayPause);

    setupUi();
    connect(ConfigStore::instance(), &ConfigStore::valueChanged, this,
            [this](const QString &key, const QVariant &value)
            {
                Q_UNUSED(value);
                if (isDanmakuEnabledConfigKey(key))
                {
                    updateDanmakuButtonState();
                }
                if (key == ConfigKeys::PlayerMediaSwitcherMode)
                {
                    applyMediaSwitcherMode();
                }
                if (SubtitleStyleUtils::isSubtitleStyleKey(key))
                {
                    applySubtitleStyleSettings();
                }
            });
    applyMediaSwitcherMode();
    applySubtitleStyleSettings();
    this->installEventFilter(this);
}


PlayerView::~PlayerView()
{
    disconnect(this, &PlayerView::playerChromeVisibilityChanged, nullptr, nullptr);
    beginViewTeardown();
    // 销毁透明 HUD 顶层窗口：所有覆盖层（top/bottom HUD、侧边栏、统计、加载、
    // OSD、弹幕层）都 reparent 进它，随它一起删除（必须先于 QWidget 基类析构
    // 删，避免悬空父指针）。
    if (m_hudWindow) {
        m_hudWindow->deleteLater();
        m_hudWindow = nullptr;
    }
    // 统计面板宿主同为独立顶层窗口（不是本视图的子控件，不会被基类析构自动删除），
    // 必须在这里显式销毁。面板本体是它的子控件，随之一起删。
    // deleteLater() 与上面的 HUD 窗口保持一致（本项目事件循环此时仍在跑）。
    if (m_statisticsWindow) {
        m_statisticsWindow->hide();
        m_statisticsWindow->deleteLater();
        m_statisticsOverlay = nullptr;
        m_statisticsWindow = nullptr;
    }
    // 兜底：统计面板若在宿主窗口创建前（从未打开过统计）就走到这里，它此时是
    // 无 parent 的裸 widget，基类析构不会回收它 —— 必须显式删除，否则泄漏。
    if (m_statisticsOverlay) {
        delete m_statisticsOverlay;
        m_statisticsOverlay = nullptr;
    }
    stopAndReport();
}

void PlayerView::prepareForStackLeave()
{
    if (m_isViewTearingDown && m_hasReportedStop)
    {
        return;
    }

    qDebug() << "[PlayerView] Prepare for stack leave"
             << "| mediaId=" << m_currentMediaId << "| isTearingDown=" << m_isViewTearingDown
             << "| hasReportedStop=" << m_hasReportedStop;

    beginViewTeardown();
    stopAndReport();
}

void PlayerView::clearMediaSwitcherCache()
{
    m_switcherCacheMediaId.clear();
    m_switcherCacheReady = false;
    m_switcherResumeItems.clear();
    m_switcherSeriesSeasons.clear();
    m_switcherSeasonEpisodes.clear();
}

bool PlayerView::useHudMediaSwitcher() const
{
    return ConfigStore::instance()->get<QString>(ConfigKeys::PlayerMediaSwitcherMode, "sidebar") ==
           QStringLiteral("hud");
}

void PlayerView::applyMediaSwitcherMode()
{
    const bool hudMode = useHudMediaSwitcher();

    if (m_mediaSwitchBtn)
    {
        m_mediaSwitchBtn->setVisible(hudMode);
    }
    if (m_rightTrigger)
    {
        m_rightTrigger->setVisible(!hudMode);
    }
    if (hudMode)
    {
        hideRightSidebar();
    }
    if (!hudMode)
    {
        hideHudMediaSwitcher();
    }

    updateMediaSwitcherButton();
}

void PlayerView::updateMediaSwitcherButton()
{
    if (!m_mediaSwitchBtn)
    {
        return;
    }

    m_mediaSwitchBtn->setProperty("drawerActive",
                                  m_mediaSwitchDrawer && m_mediaSwitchDrawer->isVisible() && useHudMediaSwitcher());
    m_mediaSwitchBtn->style()->unpolish(m_mediaSwitchBtn);
    m_mediaSwitchBtn->style()->polish(m_mediaSwitchBtn);
    m_mediaSwitchBtn->update();

    const QString tooltip = m_isSeriesMode ? tr("Switch season and episode") : tr("Switch media");
    m_mediaSwitchBtn->setToolTip(tooltip);
}

QString PlayerView::formatMediaSwitcherPlaybackTitle(const MediaItem &item) const
{
    return MediaItemUtils::playbackTitle(item, m_seriesName);
}

void PlayerView::showHudMediaSwitcher()
{
    if (m_isViewTearingDown || !useHudMediaSwitcher() || !m_mediaSwitchDrawer)
    {
        return;
    }

    if (m_activePopup)
    {
        m_activePopup->deleteLater();
        m_activePopup = nullptr;
    }
    closeSubtitleSubmenu();
    closeActivePlayerDialog();

    if (m_switcherCacheReady && m_switcherCacheMediaId == m_currentMediaId)
    {
        m_mediaSwitchDrawer->show();
        syncHudMediaSwitcherContent();
        updateMediaSwitcherButton();
        updateOverlayLayout();
        showControls();
        return;
    }

    m_mediaSwitchDrawer->setLoadingState(m_isSeriesMode ? (m_seriesName.isEmpty() ? tr("Episodes") : m_seriesName)
                                                        : tr("Continue Watching"),
                                         tr("Loading..."), m_isSeriesMode);
    m_mediaSwitchDrawer->show();
    updateMediaSwitcherButton();
    updateOverlayLayout();
    showControls();
    ensureMediaSwitcherDataLoaded();
}

void PlayerView::hideHudMediaSwitcher()
{
    if (m_isViewTearingDown)
    {
        if (m_mediaSwitchDrawer)
        {
            m_mediaSwitchDrawer->hide();
        }
        return;
    }

    if (!m_mediaSwitchDrawer || !m_mediaSwitchDrawer->isVisible())
    {
        return;
    }

    m_mediaSwitchDrawer->hide();
    updateMediaSwitcherButton();
    updateOverlayLayout();
}

void PlayerView::syncHudMediaSwitcherContent()
{
    if (!m_mediaSwitchDrawer)
    {
        return;
    }

    if (m_isSeriesMode && !m_seriesId.isEmpty())
    {
        m_mediaSwitchDrawer->setSeriesData(m_seriesName, m_switcherSeriesSeasons, m_switcherSeasonEpisodes,
                                           m_currentMediaId);
    }
    else
    {
        m_mediaSwitchDrawer->setMovieItems(m_switcherResumeItems, m_currentMediaId);
    }

    updateMediaSwitcherButton();
    updateOverlayLayout();
}

bool PlayerView::findAdjacentMediaFromCache(int direction, QString &mediaId, QString &title,
                                            long long &startPositionTicks) const
{
    mediaId.clear();
    title.clear();
    startPositionTicks = 0;

    if (!m_switcherCacheReady || m_switcherCacheMediaId != m_currentMediaId)
    {
        return false;
    }

    if (m_isSeriesMode && !m_seriesId.isEmpty())
    {
        const QString currentId = m_currentMediaId;
        MediaItem previousItem;
        MediaItem nextItem;
        bool foundCurrent = false;

        for (const MediaItem &season : m_switcherSeriesSeasons)
        {
            const QList<MediaItem> episodes = m_switcherSeasonEpisodes.value(season.id);
            for (const MediaItem &episode : episodes)
            {
                if (episode.id == currentId)
                {
                    foundCurrent = true;
                }
                else if (!foundCurrent)
                {
                    previousItem = episode;
                }
                else if (foundCurrent && nextItem.id.isEmpty())
                {
                    nextItem = episode;
                    break;
                }
            }
            if (foundCurrent && !nextItem.id.isEmpty())
            {
                break;
            }
        }

        const MediaItem targetItem = direction < 0 ? previousItem : nextItem;
        if (targetItem.id.isEmpty())
        {
            return false;
        }

        mediaId = targetItem.id;
        title = formatMediaSwitcherPlaybackTitle(targetItem);
        startPositionTicks = targetItem.userData.playbackPositionTicks;
        return true;
    }

    if (m_switcherResumeItems.isEmpty())
    {
        return false;
    }

    QVector<int> filteredIndexes;
    filteredIndexes.reserve(m_switcherResumeItems.size());
    int currentFilteredIndex = -1;
    const QString currentType = m_currentMediaItem.type;

    for (int i = 0; i < m_switcherResumeItems.size(); ++i)
    {
        const MediaItem &item = m_switcherResumeItems.at(i);
        if (!currentType.isEmpty() && item.type != currentType)
        {
            continue;
        }

        const int filteredIndex = filteredIndexes.size();
        filteredIndexes.append(i);
        if (item.id == m_currentMediaId)
        {
            currentFilteredIndex = filteredIndex;
        }
    }

    if (currentFilteredIndex < 0)
    {
        return false;
    }

    const int step = direction < 0 ? -1 : 1;
    const int targetFilteredIndex = currentFilteredIndex + step;
    if (targetFilteredIndex < 0 || targetFilteredIndex >= filteredIndexes.size())
    {
        return false;
    }

    const MediaItem &targetItem = m_switcherResumeItems.at(filteredIndexes.at(targetFilteredIndex));
    if (targetItem.id.isEmpty())
    {
        return false;
    }

    mediaId = targetItem.id;
    title = formatMediaSwitcherPlaybackTitle(targetItem);
    startPositionTicks = targetItem.userData.playbackPositionTicks;
    return true;
}

bool PlayerView::findNextResumeMediaFromCache(QString &mediaId,
                                              QString &title,
                                              long long &startPositionTicks,
                                              bool skipCurrentSeries) const
{
    mediaId.clear();
    title.clear();
    startPositionTicks = 0;

    if (!m_switcherCacheReady || m_switcherCacheMediaId != m_currentMediaId || m_switcherResumeItems.isEmpty())
    {
        return false;
    }

    const auto isEligible = [this, skipCurrentSeries](const MediaItem &item)
    {
        if (item.id.isEmpty() || item.id == m_currentMediaId)
        {
            return false;
        }

        if (skipCurrentSeries && !m_seriesId.isEmpty() &&
            (item.seriesId == m_seriesId || item.id == m_seriesId))
        {
            return false;
        }

        return true;
    };

    int currentIndex = -1;
    for (int i = 0; i < m_switcherResumeItems.size(); ++i)
    {
        if (m_switcherResumeItems.at(i).id == m_currentMediaId)
        {
            currentIndex = i;
            break;
        }
    }

    const int startIndex = currentIndex >= 0 ? currentIndex + 1 : 0;
    for (int i = startIndex; i < m_switcherResumeItems.size(); ++i)
    {
        const MediaItem &item = m_switcherResumeItems.at(i);
        if (!isEligible(item))
        {
            continue;
        }

        mediaId = item.id;
        title = MediaItemUtils::playbackTitle(item);
        if (title.isEmpty())
        {
            title = item.name;
        }
        startPositionTicks = item.userData.playbackPositionTicks;
        return true;
    }

    return false;
}


bool PlayerView::isMediaPlaying() const
{
    return m_isPlaying;
}

void PlayerView::updatePowerInhibition()
{
    const bool shouldHold = !m_isViewTearingDown && !m_hasReportedStop && m_isPlaying && !m_currentMediaId.isEmpty();

    if (shouldHold == m_powerInhibitionHeld)
    {
        return;
    }

    if (shouldHold)
    {
        m_powerInhibitionHeld = PowerInhibitUtils::acquirePlaybackInhibition(QStringLiteral("ReEmby video playback"));
    }
    else
    {
        PowerInhibitUtils::releasePlaybackInhibition();
        m_powerInhibitionHeld = false;
    }
}

QCoro::Task<void> PlayerView::autoPlayNextMediaIfEnabled()
{
    if (m_isViewTearingDown || m_hasReportedStop || m_currentMediaId.isEmpty())
    {
        co_return;
    }

    if (!ConfigStore::instance()->get<bool>(ConfigKeys::PlayerContinuousPlay, true))
    {
        qInfo().noquote() << "[PlayerView] Continuous playback disabled"
                          << "| mediaId:" << m_currentMediaId;
        co_return;
    }

    if (m_autoPlayAdvanceInProgress)
    {
        qDebug().noquote() << "[PlayerView] Continuous playback already advancing"
                           << "| mediaId:" << m_currentMediaId;
        co_return;
    }

    m_autoPlayAdvanceInProgress = true;

    QPointer<PlayerView> guard(this);
    const QString finishedMediaId = m_currentMediaId;
    const bool finishedSeriesMode = m_isSeriesMode && !m_seriesId.isEmpty();

    qInfo().noquote() << "[PlayerView] Continuous playback resolving next item"
                      << "| mediaId:" << finishedMediaId
                      << "| seriesMode:" << finishedSeriesMode
                      << "| seriesId:" << m_seriesId;

    if (!m_switcherCacheReady || m_switcherCacheMediaId != finishedMediaId)
    {
        co_await ensureMediaSwitcherDataLoaded();
        if (!guard || guard->m_currentMediaId != finishedMediaId)
        {
            co_return;
        }
    }

    QString targetId;
    QString targetTitle;
    long long startTicks = 0;
    bool hasTarget = guard->findAdjacentMediaFromCache(1, targetId, targetTitle, startTicks);
    if (!hasTarget)
    {
        hasTarget = guard->findNextResumeMediaFromCache(targetId, targetTitle, startTicks, finishedSeriesMode);
    }

    if (!hasTarget)
    {
        qInfo().noquote() << "[PlayerView] Continuous playback has no next item; finishing playback"
                          << "| mediaId:" << finishedMediaId
                          << "| seriesMode:" << finishedSeriesMode;
        guard->m_autoPlayAdvanceInProgress = false;
        // 没有下一集（最后一集 / 单集 / 电影）：停止播放并离开播放器——
        // 内嵌形态回到详情界面，独立窗口形态由 navigateBack 接收者关闭窗口。
        // 用 0ms 延迟脱离当前 EOF 信号链，避免在 mpv 属性回调里同步 teardown。
        if (guard)
        {
            QPointer<PlayerView> leaveGuard(guard);
            QTimer::singleShot(0, guard.data(), [leaveGuard]()
                               {
                                   if (leaveGuard && !leaveGuard->m_isViewTearingDown)
                                   {
                                       leaveGuard->onBackClicked();
                                   }
                               });
        }
        co_return;
    }

    qInfo().noquote() << "[PlayerView] Continuous playback switching item"
                      << "| from:" << finishedMediaId
                      << "| to:" << targetId
                      << "| title:" << targetTitle
                      << "| startTicks:" << startTicks;

    co_await guard->switchFromMediaSwitcher(targetId, targetTitle, startTicks);
    if (guard && !guard->m_hasReportedStop && guard->m_currentMediaId == finishedMediaId)
    {
        guard->m_autoPlayAdvanceInProgress = false;
    }
}

QCoro::Task<void> PlayerView::requestIntroDBSegments()
{
    QPointer<IntroDBService> introDB = m_core ? m_core->introDBService() : nullptr;
    QPointer<MediaService> mediaService = m_core ? m_core->mediaService() : nullptr;
    if (m_segmentsRequested || !introDB)
        co_return;

    const bool skipIntro = ConfigStore::instance()->get<bool>(ConfigKeys::PlayerSkipIntro, false);
    const bool skipOutro = ConfigStore::instance()->get<bool>(ConfigKeys::PlayerSkipOutro, false);
    if (!skipIntro && !skipOutro)
    {
        qInfo("IntroDB: both skip intro and skip outro disabled, skipping");
        co_return;
    }

    const QString currentMediaId = m_currentMediaId;
    const MediaItem currentItem = m_currentMediaItem;
    const int season = currentItem.parentIndexNumber;
    const int episode = currentItem.indexNumber;
    if (season <= 0 || episode <= 0)
    {
        qInfo("IntroDB: invalid season/episode: S%02dE%02d", season, episode);
        co_return;
    }

    m_segmentsRequested = true;
    QPointer<PlayerView> safeThis(this);

    const auto providerIdValue = [](const QVariantMap &providerIds, std::initializer_list<const char *> keys)
    {
        for (const char *key : keys)
        {
            const QString value = providerIds.value(QString::fromLatin1(key)).toString().trimmed();
            if (!value.isEmpty())
            {
                return value;
            }
        }
        return QString();
    };

    const QString episodeImdbId = providerIdValue(currentItem.providerIds, {"Imdb", "IMDb", "imdb", "imdbid"});
    QString imdbId;
    if (mediaService && !currentItem.seriesId.trimmed().isEmpty())
    {
        try
        {
            const MediaItem seriesDetail = co_await mediaService->getItemDetail(
                currentItem.seriesId, m_currentMediaItem.serverId);
            if (!safeThis || safeThis->m_currentMediaId != currentMediaId)
                co_return;
            imdbId = providerIdValue(seriesDetail.providerIds, {"Imdb", "IMDb", "imdb", "imdbid"});
            if (!imdbId.isEmpty())
            {
                qInfo("IntroDB: using series IMDb ID imdb=%s seriesId=%s", qPrintable(imdbId),
                      qPrintable(currentItem.seriesId));
            }
        }
        catch (const std::exception &e)
        {
            if (!safeThis || safeThis->m_currentMediaId != currentMediaId)
                co_return;
            qWarning("IntroDB: failed to fetch series detail for IMDb ID: %s", e.what());
        }
    }

    if (imdbId.isEmpty())
    {
        imdbId = episodeImdbId;
        if (!imdbId.isEmpty())
        {
            qInfo("IntroDB: using episode IMDb ID fallback imdb=%s", qPrintable(imdbId));
        }
    }

    if (imdbId.isEmpty())
    {
        qInfo("IntroDB: no IMDb ID in providerIds, episodeKeys=%s",
              qPrintable(currentItem.providerIds.keys().join(", ")));
        co_return;
    }

    qInfo("IntroDB: requesting segments imdb=%s S%02dE%02d", qPrintable(imdbId), season, episode);

    if (!introDB)
        co_return;
    const IntroDBService::EpisodeSegments segments = co_await introDB->fetchSegments(imdbId, season, episode);
    if (!safeThis || safeThis->m_currentMediaId != currentMediaId)
        co_return;
    safeThis->m_episodeSegments = segments;
    qInfo("IntroDB: segments fetched=%d notFound=%d intro=[%.1f-%.1f] outro=[%.1f-%.1f]",
          safeThis->m_episodeSegments.fetched, safeThis->m_episodeSegments.notFound,
          safeThis->m_episodeSegments.intro.startSec, safeThis->m_episodeSegments.intro.endSec,
          safeThis->m_episodeSegments.outro.startSec, safeThis->m_episodeSegments.outro.endSec);
}

void PlayerView::checkAndSkipSegment(double position)
{
    const bool skipIntro = ConfigStore::instance()->get<bool>(ConfigKeys::PlayerSkipIntro, false);
    const bool skipOutro = ConfigStore::instance()->get<bool>(ConfigKeys::PlayerSkipOutro, false);

    // ① 手动设置优先（用户在播放器里为该剧/该片手动设定的时长）。
    if (skipIntro && !m_introSkipped && m_mpvWidget && m_manualIntroSec > 0 &&
        std::isfinite(position) && position >= 0.0 && position < m_manualIntroSec)
    {
        qInfo("SkipSegments: skipping manual intro [0-%.1f] at pos=%.1f",
              static_cast<double>(m_manualIntroSec), position);
        m_mpvWidget->seek(static_cast<double>(m_manualIntroSec));
        m_introSkipped = true;
    }
    if (skipOutro && !m_outroSkipped && m_mpvWidget && m_manualOutroSec > 0 &&
        m_totalDuration > 0.0 && std::isfinite(position) &&
        position >= m_totalDuration - static_cast<double>(m_manualOutroSec))
    {
        qInfo("SkipSegments: skipping manual outro at pos=%.1f (duration=%.1f)",
              position, m_totalDuration);
        // 跳到结尾触发 EOF：连播接下一集；最后一集则停止播放返回详情。
        m_mpvWidget->seek(m_totalDuration);
        m_outroSkipped = true;
    }

    // ② IntroDB 社区数据（手动未设置或未命中时兜底）。
    if (!m_episodeSegments.fetched || m_episodeSegments.notFound)
        return;

    const auto skipIfInRange =
        [this, position](const IntroDBService::SegmentInfo &seg, const char *label, bool enabled, bool &flag)
    {
        if (!enabled || flag || !m_mpvWidget || !std::isfinite(seg.startSec) || !std::isfinite(seg.endSec) ||
            seg.startSec < 0 || seg.endSec <= seg.startSec)
            return;
        if (position >= seg.startSec && position < seg.endSec)
        {
            qInfo("IntroDB: skipping %s [%.1f-%.1f] at pos=%.1f", label, seg.startSec, seg.endSec, position);
            m_mpvWidget->seek(seg.endSec);
            flag = true;
        }
    };

    skipIfInRange(m_episodeSegments.intro, "intro", skipIntro, m_introSkipped);
    skipIfInRange(m_episodeSegments.outro, "outro", skipOutro, m_outroSkipped);
}

void PlayerView::refreshManualSkipSettings()
{
    const SkipSegmentsStore::Lengths lengths =
        SkipSegmentsStore::instance()->resolve(m_seriesId, m_currentMediaId);
    m_manualIntroSec = qMax(0, lengths.introSec);
    m_manualOutroSec = qMax(0, lengths.outroSec);
    qInfo("SkipSegments: manual settings resolved series=%s item=%s intro=%d outro=%d",
          qPrintable(m_seriesId), qPrintable(m_currentMediaId),
          m_manualIntroSec, m_manualOutroSec);
}


void PlayerView::pausePlayback()
{
    if (m_mpvWidget)
        m_mpvWidget->pause();
}

// 统一的软解决策入口：有 MediaStreams 数据时更新 sticky 状态，无数据时
// 沿用当前媒体的既有决策（否则列表路径的窗口恢复会把 DV 软解重置回硬解）。
void PlayerView::applyDecodeDecision(const MediaSourceInfo &source)
{
    const bool hasData = hasVideoStreamData(source);
    const bool need = hasData ? sourceNeedsForcedSoftwareDecode(source)
                              : m_swDecodeForCurrentMedia;
    m_swDecodeForCurrentMedia = need;
    // standalone（独立窗口，vo=gpu-next + d3d11）不需要为纯 DV 强制软解：DV 的
    // RPU 由 libplacebo 正确应用，实测 d3d11va 硬解同样不发绿；而 4K 10bit 纯
    // 软解本身就是明显的卡顿来源（用户实测独立窗口比内嵌卡）。内嵌仍走 render
    // API 的旧 gpu renderer，保持原有软解策略不变。
    m_mpvWidget->setForceSoftwareDecode(need && !m_standalone);
}




void PlayerView::resumePlayback()
{
    if (!m_mpvWidget || m_currentMediaId.isEmpty() || m_isViewTearingDown || m_hasReportedStop)
        return;

    qInfo().noquote() << "[PlayerView] Resume after window restore"
                      << "| mediaId:" << m_currentMediaId
                      << "| position:" << m_currentPosition;

    updateOverlayLayout();
    m_mpvWidget->update();
    showControls();
    m_mpvWidget->resumeAfterContextRestore();
}

void PlayerView::restoreAfterWindowShow(bool shouldResumePlaying)
{
    if (!m_mpvWidget || m_currentMediaId.isEmpty() || m_isViewTearingDown || m_hasReportedStop)
        return;

    const double restorePosition = qMax(0.0, m_currentPosition);
    QString streamUrl = m_originalStreamUrl;
    if (m_currentSourceInfoVar.isValid())
    {
        MediaSourceInfo sourceInfo;
        if (m_currentSourceInfoVar.canConvert<PlayerLaunchContext>())
            sourceInfo = m_currentSourceInfoVar.value<PlayerLaunchContext>().selectedSource;
        else if (m_currentSourceInfoVar.canConvert<MediaSourceInfo>())
            sourceInfo = m_currentSourceInfoVar.value<MediaSourceInfo>();

        // 纯 DV (profile 5) 片源覆盖 hwdec，防止硬解发绿（同主播放入口）。
        applyDecodeDecision(sourceInfo);

        // 跨服路由：恢复播放按当前 item 所属服务器重算 URL。
        const QString refreshedUrl = m_core->mediaService()->getStreamUrl(
            m_currentMediaId, sourceInfo, m_currentMediaItem.serverId);
        if (!refreshedUrl.isEmpty())
            streamUrl = refreshedUrl;
    }

    if (streamUrl.isEmpty())
    {
        qWarning().noquote() << "[PlayerView] Cannot restore playback: stream URL is empty"
                             << "| mediaId:" << m_currentMediaId;
        return;
    }

    qInfo().noquote() << "[PlayerView] Restore playback after window show"
                      << "| mediaId:" << m_currentMediaId
                      << "| position:" << restorePosition
                      << "| shouldPlay:" << shouldResumePlaying;

    m_pendingSeekSeconds = restorePosition;
    m_windowRestorePending = true;
    m_windowRestoreShouldPlay = shouldResumePlaying;
    m_isBuffering = true;
    updateLoadingState();
    updateOverlayLayout();
    showControls();

    if (m_danmakuController) {
        m_danmakuController->prepareForMediaReload();
    }

    const ServerProfile activeProfile = m_core->serverManager()->activeProfile();
    const QString activeServerId = activeProfile.id;
    m_mpvWidget->setCustomUserAgent(activeProfile.effectiveUserAgent());
    m_mpvWidget->loadMedia(streamUrl, activeServerId);
}


QPushButton *PlayerView::createHudButton(const QString &iconPath, const QSize &size)
{
    auto *btn = new QPushButton(this);
    btn->setProperty("class", "player-hud-btn");
    btn->setIcon(QIcon(iconPath));
    btn->setIconSize(size);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFocusPolicy(Qt::NoFocus); 
    btn->setFixedSize(size.width() + 16, size.height() + 16);
    return btn;
}

void PlayerView::setupUi()
{
    m_mpvWidget = new MpvWidget(this, m_standalone);
    m_mpvWidget->setAutoFillBackground(true);
    m_mpvWidget->installEventFilter(this); 
    m_mpvWidget->setMouseTracking(true);
    m_nativeDanmakuOverlay = new NativeDanmakuOverlay(m_mpvWidget->controller(), this);
    m_danmakuController = new PlayerDanmakuController(m_core, m_mpvWidget, m_nativeDanmakuOverlay, this);
    connect(m_danmakuController, &PlayerDanmakuController::toastRequested, this,
            [this](const QString &message) { showToast(message); });
    connect(m_danmakuController, &PlayerDanmakuController::stateChanged, this, &PlayerView::updateDanmakuButtonState);

    
    m_loadingOverlay = new LoadingOverlay(this);
    m_loadingOverlay->setObjectName("playerLoadingOverlay");
    
    m_loadingOverlay->setImmersive(true);
    
    m_loadingOverlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    // 缓冲结束时不立刻隐藏转圈，先等 kLoadingHideDelayMs。期间若又回到缓冲
    // 状态（updateLoadingState 会停掉这个定时器），转圈就一直保持显示。
    m_loadingHideTimer = new QTimer(this);
    m_loadingHideTimer->setSingleShot(true);
    m_loadingHideTimer->setInterval(kLoadingHideDelayMs);
    connect(m_loadingHideTimer, &QTimer::timeout, this, [this]()
            {
                if (m_isBuffering || m_isSeeking)
                {
                    return;
                }
                if (m_loadingOverlay)
                {
                    m_loadingOverlay->stop();
                }
                m_loadingShown = false;
            });

    connect(m_mpvWidget->controller(), &MpvController::fileLoaded, this,
            [this]()
            {
                if (m_windowRestorePending)
                {
                    const double restorePosition = m_pendingSeekSeconds;
                    m_windowRestorePending = false;
                    m_pendingSeekSeconds = 0.0;
                    if (restorePosition > 0.0)
                        m_mpvWidget->seek(restorePosition);
                    if (m_windowRestoreShouldPlay)
                        m_mpvWidget->play();
                    else
                        m_mpvWidget->pause();

                    qInfo().noquote() << "[PlayerView] Window restore state applied"
                                      << "| position:" << restorePosition
                                      << "| playing:" << m_windowRestoreShouldPlay;
                }
                m_isBuffering = false;
                updateLoadingState();
                applySubtitleStyleSettings();

                // 诊断日志：播放 5 秒后检查字幕选流与解码状态，
                // 用于区分「解码层无数据（sub-text 为空）」和
                // 「渲染层吞字幕（sub-text 有内容但不显示）」
                if (m_mpvWidget && m_mpvWidget->controller())
                {
                    auto *ctrl = m_mpvWidget->controller();
                    QTimer::singleShot(5000, this, [ctrl]()
                    {
                        if (!ctrl->mpv())
                            return;
                        const QVariant sid = ctrl->getProperty("sid");
                        const QVariant subText = ctrl->getProperty("sub-text");
                        const QVariant subVis = ctrl->getProperty("sub-visibility");
                        const QVariant hwdec = ctrl->getProperty("hwdec-current");
                        const QVariant pixFmt = ctrl->getProperty("video-params/pixelformat");
                        qInfo().noquote()
                            << "[PlayerView] Subtitle diagnostic (5s after load)"
                            << "| sid:" << sid.toString()
                            << "| sub-visibility:" << subVis.toString()
                            << "| sub-text-len:" << subText.toString().size()
                            << "| sub-text:" << subText.toString().left(40)
                            << "| hwdec:" << hwdec.toString()
                            << "| pixfmt:" << pixFmt.toString();
                    });
                }
            });
    

    
    m_osdLayer = new PlayerOsdLayer(this);

    

    

    setupRightSidebar();

    
    m_rightTrigger = new QWidget(this);
    m_rightTrigger->setFixedWidth(15);
    m_rightTrigger->setCursor(Qt::PointingHandCursor);
    m_rightTrigger->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    m_rightTrigger->installEventFilter(this);

    
    m_logoLabel = new QLabel(this);
    m_logoLabel->setObjectName("playerLogoLabel");
    m_logoLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_logoLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_logoLabel->hide();

    
    m_networkSpeedLabel = new QLabel("0.0 KB/s", this); 
    m_networkSpeedLabel->setObjectName("playerNetworkSpeed");
    m_networkSpeedLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true); 
    m_networkSpeedLabel->setAlignment(Qt::AlignRight | Qt::AlignTop);

    
    m_topHUD = new QWidget(this);
    m_topHUD->setObjectName("playerTopHUD");
    
    m_topHUD->setFixedHeight(32);
    m_topHUD->installEventFilter(this); 

    auto *topLayout = new QHBoxLayout(m_topHUD);
    topLayout->setContentsMargins(0, 0, 0, 0); 
    topLayout->setSpacing(0);

    
    m_backBtn = new QPushButton(m_topHUD);
    m_backBtn->setObjectName("hud-back-btn");
    m_backBtn->setIcon(QIcon(":/svg/player/back.svg"));
    m_backBtn->setIconSize(QSize(16, 16)); 
    m_backBtn->setFixedSize(38, 28);       
    m_backBtn->setFocusPolicy(Qt::NoFocus);
    m_backBtn->setToolTip(tr("Back"));
    connect(m_backBtn, &QPushButton::clicked, this, &PlayerView::onBackClicked);

    m_titleLabel = new QLabel(m_topHUD);
    m_titleLabel->setObjectName("playerTitleLabel");
    m_titleLabel->setMinimumWidth(0);
    m_titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_titleLabel->setAlignment(Qt::AlignCenter);
    m_titleLabel->installEventFilter(this);

    
    auto *sysLayout = new QHBoxLayout();
    sysLayout->setSpacing(0);
    sysLayout->setContentsMargins(0, 0, 0, 0);

    
    m_minBtn = new QPushButton(m_topHUD);
    m_minBtn->setObjectName("hud-min-btn");
    m_minBtn->setIcon(QIcon(":/svg/player/min.svg"));
    m_minBtn->setIconSize(QSize(12, 12)); 
    m_minBtn->setFixedSize(38, 28);
    m_minBtn->setFocusPolicy(Qt::NoFocus);
    m_minBtn->setToolTip(tr("Minimize"));

    m_maxBtn = new QPushButton(m_topHUD);
    m_maxBtn->setObjectName("hud-max-btn");
    m_maxBtn->setIcon(QIcon(":/svg/player/max.svg"));
    m_maxBtn->setIconSize(QSize(12, 12));
    m_maxBtn->setFixedSize(38, 28);
    m_maxBtn->setFocusPolicy(Qt::NoFocus);
    m_maxBtn->setToolTip(tr("Maximize"));

    m_closeBtn = new QPushButton(m_topHUD);
    m_closeBtn->setObjectName("hud-close-btn");
    m_closeBtn->setIcon(QIcon(":/svg/player/close.svg"));
    m_closeBtn->setIconSize(QSize(12, 12));
    m_closeBtn->setFixedSize(38, 28);
    m_closeBtn->setFocusPolicy(Qt::NoFocus);
    m_closeBtn->setToolTip(tr("Close"));

#if defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    m_minBtn->hide();
    m_maxBtn->hide();
    m_closeBtn->hide();
#else
    connect(m_minBtn, &QPushButton::clicked, this, [this]() { window()->showMinimized(); });

    connect(m_maxBtn, &QPushButton::clicked, this,
            [this]()
            {
                
                if (window()->isMaximized())
                {
                    window()->showNormal();
                }
                else
                {
                    window()->showMaximized();
                }
            });

    
    
    
    connect(m_closeBtn, &QPushButton::clicked, this,
            [this]()
            {
                window()->close();
            });
#endif

#if defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    delete sysLayout;
#else
    sysLayout->addWidget(m_minBtn, 0, Qt::AlignTop);
    sysLayout->addWidget(m_maxBtn, 0, Qt::AlignTop);
    sysLayout->addWidget(m_closeBtn, 0, Qt::AlignTop);
#endif

    
#if defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    auto *macLeftTitlebarArea = new QWidget(m_topHUD);
    macLeftTitlebarArea->setObjectName("playerMacTitlebarLeftArea");
    macLeftTitlebarArea->setFixedWidth(118);

    auto *macRightTitlebarArea = new QWidget(m_topHUD);
    macRightTitlebarArea->setObjectName("playerMacTitlebarRightArea");
    macRightTitlebarArea->setFixedWidth(118);
    auto *macRightLayout = new QHBoxLayout(macRightTitlebarArea);
    macRightLayout->setContentsMargins(0, 0, 0, 0);
    macRightLayout->setSpacing(0);
    macRightLayout->addStretch();
    macRightLayout->addWidget(m_backBtn, 0, Qt::AlignTop);

    topLayout->addWidget(macLeftTitlebarArea, 0, Qt::AlignTop);
    topLayout->addWidget(m_titleLabel, 1, Qt::AlignTop);
    topLayout->addWidget(macRightTitlebarArea, 0, Qt::AlignTop);
#else
    topLayout->addWidget(m_backBtn, 0, Qt::AlignTop);
    topLayout->addWidget(m_titleLabel, 1, Qt::AlignTop);
    topLayout->addLayout(sysLayout, 0);
#endif

    
    m_bottomHUD = new QWidget(this);
    m_bottomHUD->setObjectName("playerBottomHUD");
    m_bottomHUD->setFixedHeight(m_bottomHudBaseHeight);

    
    m_bottomHUD->installEventFilter(this);

    auto *bottomLayout = new QVBoxLayout(m_bottomHUD);
    bottomLayout->setContentsMargins(32, 10, 32, 16);
    bottomLayout->setSpacing(8);

    m_mediaSwitchDrawer = new PlayerMediaSwitcherPanel(m_core, m_bottomHUD);
    m_mediaSwitchDrawer->hide();
    connect(m_mediaSwitchDrawer, &PlayerMediaSwitcherPanel::playRequested, this,
            [this](QString mediaId, QString title, long long startPositionTicks) -> QCoro::Task<void>
            { co_await switchFromMediaSwitcher(mediaId, title, startPositionTicks); });
    bottomLayout->addWidget(m_mediaSwitchDrawer);

    
    auto *progressLayout = new QHBoxLayout();
    progressLayout->setSpacing(16);

    m_currentTimeLabel = new QLabel("00:00", m_bottomHUD);
    m_currentTimeLabel->setObjectName("playerTimeLabel");

    m_progressSlider = new ModernSlider(Qt::Horizontal, m_bottomHUD);
    m_progressSlider->setObjectName("playerSlider");
    m_progressSlider->setFocusPolicy(Qt::NoFocus); 
    m_progressSlider->setCursor(Qt::PointingHandCursor);
    m_progressSlider->setSingleStep(1); 
    m_progressSlider->setPageStep(10);
    m_progressSlider->setRange(0, 1000);

    m_totalTimeLabel = new QLabel("00:00", m_bottomHUD);
    m_totalTimeLabel->setObjectName("playerTimeLabel");

    progressLayout->addWidget(m_currentTimeLabel);
    progressLayout->addWidget(m_progressSlider, 1);
    progressLayout->addWidget(m_totalTimeLabel);

    
    auto *controlLayout = new QHBoxLayout();
    controlLayout->setSpacing(12);

    m_prevMediaBtn = createHudButton(":/svg/player/prev.svg");
    m_prevMediaBtn->setToolTip(tr("Previous"));
    m_rewindBtn = createHudButton(":/svg/player/fast-rewind.svg");
    m_rewindBtn->setToolTip(tr("Rewind"));
    m_playPauseBtn = createHudButton(":/svg/player/pause.svg", QSize(36, 36));
    m_playPauseBtn->setToolTip(tr("Play/Pause"));
    m_forwardBtn = createHudButton(":/svg/player/fast-forward.svg");
    m_forwardBtn->setToolTip(tr("Forward"));
    m_nextMediaBtn = createHudButton(":/svg/player/next.svg");
    m_nextMediaBtn->setToolTip(tr("Next"));

    
    m_volumeBtn = createHudButton(":/svg/player/volume.svg");
    m_volumeBtn->setToolTip(tr("Mute/Unmute"));
    m_volumeBtn->installEventFilter(this); 

    m_volumeSlider = new ModernSlider(Qt::Horizontal, m_bottomHUD);
    m_volumeSlider->setObjectName("volumeSlider"); 
    m_volumeSlider->setFocusPolicy(Qt::NoFocus);
    m_volumeSlider->setCursor(Qt::PointingHandCursor);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(100);
    m_volumeSlider->setFixedWidth(80);
    m_volumeSlider->installEventFilter(this);

    m_speedBtn = new QPushButton("1.0X", m_bottomHUD);
    m_speedBtn->setObjectName("playerSpeedBtn");
    m_speedBtn->setCursor(Qt::PointingHandCursor);
    m_speedBtn->setFocusPolicy(Qt::NoFocus);
    m_speedBtn->setFixedHeight(40);
    m_speedBtn->setToolTip(tr("Playback Speed"));

    m_mediaSwitchBtn = createHudButton(":/svg/player/media-switcher.svg");
    m_mediaSwitchBtn->setObjectName("playerMediaSwitchBtn");
    m_mediaSwitchBtn->setToolTip(tr("Media Switcher"));

    m_audioBtn = createHudButton(":/svg/player/audio.svg");
    m_audioBtn->setToolTip(tr("Audio Track"));
    m_subtitleBtn = createHudButton(":/svg/player/subtitle.svg");
    m_subtitleBtn->setToolTip(tr("Subtitles"));
    m_danmakuBtn = createHudButton(":/svg/player/danmaku.svg");
    m_danmakuBtn->setToolTip(tr("Danmaku"));
    m_settingsBtn = createHudButton(":/svg/player/settings.svg");
    m_settingsBtn->setToolTip(tr("Settings"));

    
    m_scaleBtn = createHudButton(":/svg/player/scale-fit.svg");
    m_scaleBtn->setToolTip(tr("Aspect Ratio"));
    m_fullscreenBtn = createHudButton(":/svg/player/fullscreen.svg");
    m_fullscreenBtn->setToolTip(tr("Fullscreen"));

    m_toastLabel = new QLabel("", m_bottomHUD);
    m_toastLabel->setObjectName("playerToastLabel");
    m_toastLabel->setAlignment(Qt::AlignCenter);
    m_toastLabel->hide();

    
    controlLayout->addWidget(m_prevMediaBtn);
    controlLayout->addWidget(m_rewindBtn);
    controlLayout->addWidget(m_playPauseBtn);
    controlLayout->addWidget(m_forwardBtn);
    controlLayout->addWidget(m_nextMediaBtn);

    
    controlLayout->addSpacing(20);

    controlLayout->addWidget(m_volumeBtn);
    controlLayout->addWidget(m_volumeSlider);

    controlLayout->addStretch();
    controlLayout->addWidget(m_toastLabel);
    controlLayout->addStretch();

    controlLayout->addWidget(m_mediaSwitchBtn);
    controlLayout->addWidget(m_speedBtn);
    controlLayout->addWidget(m_audioBtn);
    controlLayout->addWidget(m_subtitleBtn);
    controlLayout->addWidget(m_danmakuBtn);
    controlLayout->addWidget(m_settingsBtn);
    controlLayout->addWidget(m_scaleBtn);
    controlLayout->addWidget(m_fullscreenBtn);

    bottomLayout->addLayout(progressLayout);
    bottomLayout->addLayout(controlLayout);

    
    // 统计面板创建为**无 parent 的顶层控件**（由 m_statisticsWindow 托管、按需
    // 惰性创建，见 ensureStatisticsWindow）。原因：弹幕层（NativeDanmakuOverlay）
    // 是与它同在 HUD 顶层窗口里的兄弟控件，每帧用 CompositionMode_Source +
    // fillRect(透明) 清屏；Source 是「覆写」不是「混合」，共用同一块 backing store
    // 时会连统计面板的像素一起抹掉（表现为「打开弹幕就看不到统计信息」）。让它拥有
    // 独立的 backing store（独立顶层窗口）才能根治，raise() 无效。
    m_statisticsOverlay = new PlayerStatisticsOverlay(nullptr);
    m_statisticsOverlay->setObjectName("playerStatisticsOverlay");
    m_statisticsOverlay->hide();

    
    m_topOpacity = new QGraphicsOpacityEffect(m_topHUD);
    m_bottomOpacity = new QGraphicsOpacityEffect(m_bottomHUD);
    m_logoOpacity = new QGraphicsOpacityEffect(m_logoLabel);
    m_speedOpacity = new QGraphicsOpacityEffect(m_networkSpeedLabel); 

    m_topHUD->setGraphicsEffect(m_topOpacity);
    m_bottomHUD->setGraphicsEffect(m_bottomOpacity);
    m_logoLabel->setGraphicsEffect(m_logoOpacity);
    m_networkSpeedLabel->setGraphicsEffect(m_speedOpacity); 

    m_fadeGroup = new QParallelAnimationGroup(this);

    auto *topAnim = new QPropertyAnimation(m_topOpacity, "opacity", this);
    topAnim->setDuration(250);

    auto *bottomAnim = new QPropertyAnimation(m_bottomOpacity, "opacity", this);
    bottomAnim->setDuration(250);

    auto *logoAnim = new QPropertyAnimation(m_logoOpacity, "opacity", this);
    logoAnim->setDuration(250);

    auto *speedAnim = new QPropertyAnimation(m_speedOpacity, "opacity", this);
    speedAnim->setDuration(250);

    m_fadeGroup->addAnimation(topAnim);
    m_fadeGroup->addAnimation(bottomAnim);
    m_fadeGroup->addAnimation(logoAnim);
    m_fadeGroup->addAnimation(speedAnim); 

    
    connect(m_playPauseBtn, &QPushButton::clicked, this, &PlayerView::togglePlayPause);
    connect(m_progressSlider, &QSlider::sliderMoved, this, &PlayerView::onSliderMoved);

    
    connect(m_rewindBtn, &QPushButton::pressed, this,
            [this]()
            {
                if (!m_longPressHandler)
                {
                    double step = ConfigStore::instance()->get<QString>(ConfigKeys::PlayerSeekStep, "10").toDouble();
                    seekRelative(-step);
                    return;
                }

                bool isHudVisible = (m_topOpacity && m_topOpacity->opacity() > 0.0);
                m_longPressHandler->startKeyLongPress(-1, !isHudVisible);
            });

    connect(m_forwardBtn, &QPushButton::pressed, this,
            [this]()
            {
                if (!m_longPressHandler)
                {
                    double step = ConfigStore::instance()->get<QString>(ConfigKeys::PlayerSeekStep, "10").toDouble();
                    seekRelative(step);
                    return;
                }

                bool isHudVisible = (m_topOpacity && m_topOpacity->opacity() > 0.0);
                m_longPressHandler->startKeyLongPress(1, !isHudVisible);
            });

    connect(m_rewindBtn, &QPushButton::released, this,
            [this]()
            {
                if (m_longPressHandler)
                {
                    m_longPressHandler->stopKeyLongPress();
                }
            });

    connect(m_forwardBtn, &QPushButton::released, this,
            [this]()
            {
                if (m_longPressHandler)
                {
                    m_longPressHandler->stopKeyLongPress();
                }
            });

    connect(m_prevMediaBtn, &QPushButton::clicked, this,
            [this]() -> QCoro::Task<void>
            {
                QString targetId;
                QString targetTitle;
                long long startTicks = 0;
                if (!findAdjacentMediaFromCache(-1, targetId, targetTitle, startTicks))
                {
                    showToast(m_isSeriesMode ? tr("No previous episode") : tr("No previous media"));
                    ensureMediaSwitcherDataLoaded();
                    co_return;
                }

                co_await switchFromMediaSwitcher(targetId, targetTitle, startTicks);
            });

    connect(m_nextMediaBtn, &QPushButton::clicked, this,
            [this]() -> QCoro::Task<void>
            {
                QString targetId;
                QString targetTitle;
                long long startTicks = 0;
                if (!findAdjacentMediaFromCache(1, targetId, targetTitle, startTicks))
                {
                    showToast(m_isSeriesMode ? tr("No next episode") : tr("No next media"));
                    ensureMediaSwitcherDataLoaded();
                    co_return;
                }

                co_await switchFromMediaSwitcher(targetId, targetTitle, startTicks);
            });

    
    connect(m_volumeBtn, &QPushButton::clicked, this, &PlayerView::toggleMute);
    connect(m_volumeSlider, &QSlider::valueChanged, this, &PlayerView::onVolumeSliderMoved);

    connect(m_speedBtn, &QPushButton::clicked, this, &PlayerView::showSpeedMenu);
    connect(m_mediaSwitchBtn, &QPushButton::clicked, this,
            [this]()
            {
                if (m_mediaSwitchDrawer && m_mediaSwitchDrawer->isVisible())
                {
                    hideHudMediaSwitcher();
                    return;
                }
                showHudMediaSwitcher();
            });
    connect(m_audioBtn, &QPushButton::clicked, this, &PlayerView::showAudioMenu);
    connect(m_subtitleBtn, &QPushButton::clicked, this, &PlayerView::showSubtitleMenu);
    connect(m_danmakuBtn, &QPushButton::clicked, this, &PlayerView::showDanmakuMenu);
    connect(m_settingsBtn, &QPushButton::clicked, this, &PlayerView::showSettingsMenu);
    connect(m_scaleBtn, &QPushButton::clicked, this, &PlayerView::cycleVideoScale);             
    connect(m_fullscreenBtn, &QPushButton::clicked, this, &PlayerView::toggleFullscreenWindow); 

    
    connect(m_mpvWidget, &MpvWidget::positionChanged, this, &PlayerView::onPositionChanged);
    connect(m_mpvWidget, &MpvWidget::durationChanged, this, &PlayerView::onDurationChanged);
    connect(m_mpvWidget, &MpvWidget::playbackStateChanged, this, &PlayerView::onPlaybackStateChanged);
    connect(m_mpvWidget, &MpvWidget::relayActiveChanged, this,
            [this](bool active)
            {
                m_useRelayNetworkSpeed = active;
                if (!active && m_networkSpeedLabel)
                {
                    m_effectiveNetworkSpeed = 0;
                    m_networkSpeedLabel->setText(formatDataRateValue(0));
                }
            });
    connect(m_mpvWidget, &MpvWidget::networkSpeedChanged, this,
            [this](qint64 bytesPerSecond)
            {
                if (m_useRelayNetworkSpeed && m_networkSpeedLabel)
                {
                    m_effectiveNetworkSpeed = bytesPerSecond;
                    m_networkSpeedLabel->setText(formatDataRateValue(bytesPerSecond));
                }
            });
    
    connect(m_mpvWidget->controller(), &MpvController::propertyChanged, this, &PlayerView::onMpvPropertyChanged);
    connect(m_mpvWidget->controller(), &MpvController::endOfFile, this,
            [this](const QString &reason)
            {
                qDebug() << "[PlayerView] MPV end of file"
                         << "| reason=" << reason;
                m_isPlaybackFinished = (reason == QLatin1String("eof"));
                m_isPlaying = false;
                m_isBuffering = false;
                m_isSeeking = false;
                updatePowerInhibition();
                updateLoadingState();
                if (m_playPauseBtn)
                {
                    m_playPauseBtn->setIcon(QIcon(":/svg/player/play.svg"));
                }
                if (m_isPlaybackFinished)
                {
                    traktOnPlaybackStopped();
                    autoPlayNextMediaIfEnabled();
                }
            });

    setScaleIcon();
    updateDanmakuButtonState();
    updateMediaSwitcherButton();
    m_rightSidebar->raise();

    // 独立播放窗口模式：HUD 提升为原生子窗口并抬到 mpv 直绘的视频层之上。
    if (m_standalone) {
        applyStandaloneOverlay();
    }
}



void PlayerView::applyStandaloneOverlay()
{
    // standalone 模式下 MpvWidget 是 WA_NativeWindow 原生子窗口，mpv 用 d3d11
    // 直绘到它的 HWND。普通 QWidget 画在父窗口 backing store 上会被它盖住，
    // 而原生子窗口（WS_CHILD）又拿不到 per-pixel alpha（UpdateLayeredWindow
    // 仅顶层窗口可用）——所以覆盖层统一迁进一个独立的透明顶层窗口（HUD 窗口，
    // 见 ensureStandaloneHudWindow）。z-order 由 owned-window 关系保证（HUD
    // 窗口永远在播放窗口之上，自然也在视频之上），半透明由顶层窗口的
    // per-pixel alpha + QSS rgba 保证。
    if (!m_standalone || !m_mpvWidget) {
        return;
    }

    // 本窗口内的视频层降到最底（覆盖层已不在本窗口，此调用仅为兜底）
    m_mpvWidget->lower();

    ensureStandaloneHudWindow();
    if (!m_hudWindow) {
        return;
    }

    // 迁进 HUD 窗口的覆盖层清单（全部是本视图的直接子控件）。
    // 所有几何都以本视图客户区为坐标系（见 updateOverlayLayout），而 HUD 窗口
    // 的 origin/size 与本视图完全重合（syncStandaloneHudWindow），因此 reparent
    // 后无需任何坐标换算。
    // m_mediaSwitchDrawer 是 m_bottomHUD 的子控件，随其一起迁移。
    // PlayerOsdLayer 是 QObject（非 QWidget），取它的 container() 容器。
    //
    // ⚠️ 迁移后这些控件（及其子控件）**不再是本视图的子孙**：任何"相对本视图"
    // 的坐标换算必须走全局坐标中转（mapToGlobal → mapFromGlobal），**不要**用
    // mapTo(this, …)/mapFrom(this, …) —— 祖先关系不成立时 Qt 只打一条
    // "parent must be in parent hierarchy" 警告并返回 (0,0)（曾导致底部 HUD 的
    // 弹层被定位到窗口左上角，用户看到的是"点按钮没反应"）。本视图自身的
    // mapFromGlobal/mapToGlobal 不受影响。
    // 注意：m_rightTrigger（右侧 15px 鼠标热区）刻意不迁移——它没有背景，
    // 迁进透明窗口后会挡住视频区右侧的鼠标；其触发由 handlePointerActivity
    // 按坐标命中（见该函数）。
    const QList<QWidget *> overlayWidgets = {
        m_topHUD, m_bottomHUD,
        m_loadingOverlay,
        m_logoLabel, m_networkSpeedLabel, m_nativeDanmakuOverlay,
        m_rightSidebar,
        m_osdLayer ? m_osdLayer->container() : nullptr,
    };
    for (QWidget *w : overlayWidgets) {
        if (!w || w == m_hudWindow || w->parentWidget() == m_hudWindow) {
            continue; // 空 / 自身 / 已迁移（幂等）
        }
        if (w->parentWidget() != this) {
            continue; // 只处理本视图的直接子控件（兜底）
        }
        // setParent 会把 widget 移到 (0,0) 并隐藏，先存几何与显隐状态再恢复。
        const QRect geo = w->geometry();
        const bool wasHidden = w->isHidden();
        w->setParent(m_hudWindow);
        w->setGeometry(geo);
        if (!wasHidden) {
            w->show();
        }
    }

    // HUD 窗口是 layered 顶层窗口：Qt 用 UpdateLayeredWindowIndirect 提交整帧，
    // 而该 API 对脏区零容忍——任何超出窗口边界的重绘请求都会让**整帧更新被
    // 拒绝**（qemby.log: "UpdateLayeredWindowIndirect failed ... 参数错误"），
    // 表现为界面停止刷新、Windows hit-test 卡在旧位图（点击无响应、侧边栏
    // 弹出不灵敏/位置错乱）。
    // QGraphicsDropShadowEffect 会把 update 区域向外扩展一个 blur 半径：
    // 侧边栏 y = 32（topHUD 固定高度）− blur 35 = −3，溢出窗口顶部；贴右边缘
    // 时又向右溢出 35px —— 实测日志脏区恰为 (…, -3)。故 layered 模式下移除
    // 覆盖层自身的投影 effect（半透明面板本身已有层次感，视觉损失很小）。
    // 注意：opacity effect（top/bottom HUD、台标、网速）不扩展绘制区域、且被
    // 成员指针持有（删了后续 setOpacity 会悬空），不能在此删除。
    if (m_rightSidebar) {
        m_rightSidebar->setGraphicsEffect(nullptr);
    }

    syncStandaloneHudWindow();
}

void PlayerView::ensureStandaloneHudWindow()
{
    if (!m_standalone || m_hudWindow) {
        return;
    }
    // 透明顶层窗口：Qt 对顶层窗口的 per-pixel alpha 是完整支持的
    //（UpdateLayeredWindow / DWM 合成），区别于 WS_CHILD 子窗口那条死路。
    // Tool + ShowWithoutActivating：不进任务栏、不抢播放窗口焦点。
    m_hudWindow = new QWidget(nullptr, Qt::FramelessWindowHint | Qt::Tool);
    m_hudWindow->setAttribute(Qt::WA_TranslucentBackground, true);
    m_hudWindow->setAttribute(Qt::WA_ShowWithoutActivating, true);
    // HUD 窗口自身的鼠标活动也要喂给 PlayerView 的显隐逻辑（鼠标在视频区
    // 挪动时 HUD 要保持显示）。
    m_hudWindow->installEventFilter(this);
    // 此刻不 show()：PlayerView 多半还没显示，mapToGlobal 坐标无效，show 会在
    // 屏幕 (0,0) 闪现。首次显示交给 setStandaloneHudVisible(true)（那时
    // showControls 已在真实几何下运行）。
    // 强制创建原生窗口，让 transientParent（owned 关系）可以设置。
    m_hudWindow->winId();
    // 播放窗口移动/缩放/显示时同步 HUD 窗口位置
    if (QWidget *top = window()) {
        top->installEventFilter(this);
    }
}

void PlayerView::syncStandaloneHudWindow()
{
    // HUD 窗口铺满 PlayerView 的客户区（全局坐标）。origin 与 PlayerView
    // 重合，因此 topHUD/bottomHUD reparent 后的相对坐标无需换算。
    if (!m_standalone || !m_hudWindow) {
        return;
    }
    // owned-window 关系（transientParent）：播放窗口首次 show 后 windowHandle
    // 才有效，所以在这里幂等补设（未设且能设就设）。
    if (window() && window()->windowHandle() && m_hudWindow->windowHandle() &&
        m_hudWindow->windowHandle()->transientParent() != window()->windowHandle()) {
        m_hudWindow->windowHandle()->setTransientParent(window()->windowHandle());
    }
    const QPoint globalOrigin = mapToGlobal(QPoint(0, 0));
    m_hudWindow->setGeometry(globalOrigin.x(), globalOrigin.y(), width(), height());
}

void PlayerView::ensureStatisticsWindow()
{
    // 统计面板必须住在一个**独立顶层窗口**里：它若只是 HUD 窗口（或 PlayerView）
    // 的普通子控件，就会与全窗口大小的弹幕层共用同一块 backing store，而弹幕层
    // 每帧用 CompositionMode_Source + fillRect(透明) 清屏——Source 是覆写不是混合，
    // 把统计面板刚画好的像素一并抹掉，表现为「打开弹幕就看不到统计信息」。顶层
    // 窗口有自己的 backing store，弹幕层的清屏波及不到它。raise() 解决不了这个问题
    //（只改绘制先后），关弹幕时统计自己回来也只是脏区重绘把它重画了一遍。
    if (m_statisticsWindow) {
        return;
    }
    if (!m_statisticsOverlay) {
        return;
    }

    // 与 HUD 窗口同样的做法：FramelessWindowHint + Tool（不进任务栏、不抢焦点），
    // 顶层窗口才能拿到 per-pixel alpha（统计面板要透出视频画面）。
    m_statisticsWindow = new QWidget(nullptr, Qt::FramelessWindowHint | Qt::Tool);
    m_statisticsWindow->setObjectName("playerStatisticsWindow");
    m_statisticsWindow->setAttribute(Qt::WA_TranslucentBackground, true);
    m_statisticsWindow->setAttribute(Qt::WA_ShowWithoutActivating, true);
    m_statisticsWindow->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    // 强制创建原生窗口，让 transientParent（owned 关系）可以设置。
    m_statisticsWindow->winId();
    // 注意：**不要**给它设 QWidget parent —— 那会把它从顶层窗口降级成子控件，
    // 弹幕层的 Source 清屏就又能擦到它，整个修复随即失效。它是顶层窗口，只由
    // PlayerView 的析构函数负责销毁（见 ~PlayerView）。
    // 同理不用 QObject::setParent(this)：QWidget 的 parent 会同时影响窗口层级语义，
    // 这里要保持"纯粹的顶层窗口 + 手动生命周期"。

    // 监听顶层窗口的 Move/Resize/Show 以跟随几何。standalone 模式下
    // ensureStandaloneHudWindow() 已经装过（installEventFilter 幂等，重复装无害），
    // 内嵌模式下则是这里首次安装。
    if (QWidget *top = window()) {
        top->installEventFilter(this);
    }

    // 面板铺满统计窗口（窗口几何即面板的目标矩形，见 syncStatisticsWindow）。
    m_statisticsOverlay->setParent(m_statisticsWindow);
    m_statisticsOverlay->move(0, 0);
    m_statisticsOverlay->show();
}

void PlayerView::syncStatisticsWindow()
{
    if (!m_statisticsWindow || !m_statisticsOverlay) {
        return;
    }
    // 面板尺寸未知（还没走过 updateOverlayLayout）时不同步，避免把窗口
    // 缩成 0×0 后连面板一起看不见。
    const int overlayWidth = m_statisticsOverlay->width();
    const int overlayHeight = m_statisticsOverlay->height();
    if (overlayWidth <= 0 || overlayHeight <= 0) {
        return;
    }

    // owned-window 关系：让统计窗口恒在播放窗口（及 HUD 窗口）之上，跟随最小化。
    // 首次显示后 windowHandle 才有效，所以在这里幂等补设。
    if (window() && window()->windowHandle() && m_statisticsWindow->windowHandle() &&
        m_statisticsWindow->windowHandle()->transientParent() != window()->windowHandle()) {
        m_statisticsWindow->windowHandle()->setTransientParent(window()->windowHandle());
    }

    // 把面板在"覆盖层坐标系"里的目标位置换算成全局坐标：该坐标系的 origin 就是
    // 本视图客户区左上角（standalone 下 HUD 窗口与客户区完全重合，见
    // syncStandaloneHudWindow），所以直接用 mapToGlobal。
    const QPoint globalTopLeft = mapToGlobal(QPoint(m_statisticsOverlayX, m_statisticsOverlayY));
    m_statisticsWindow->setGeometry(globalTopLeft.x(), globalTopLeft.y(),
                                    overlayWidth, overlayHeight);
}

void PlayerView::logStandaloneLayerDiagnostics()
{
    // 只在首次显示时打一次，避免 HUD 频繁显隐刷屏。验证对象是透明 HUD 顶层
    // 窗口（所有覆盖层的载体）：确认 Qt 是否真的为它启用了 WS_EX_LAYERED ——
    // 若为 false，说明 Qt 没有走 per-pixel alpha 路径，半透明不会生效。
    if (!m_standalone) {
        return;
    }
    static bool s_logged = false;
    if (s_logged) {
        return;
    }
    s_logged = true;

    // 只检查 HUD 窗口本身：它必须是 native 顶层窗口（winId() 只是取句柄）。
    // 不要对普通子控件调用 winId()——那会强制把它们提升为原生子窗口，破坏
    // 它们在 layered 父窗口中的 per-pixel alpha 合成。
    const QList<QWidget *> layers = {m_hudWindow};
    for (QWidget *w : layers) {
        if (!w) {
            continue;
        }
        const WId id = w->winId();
        bool layered = false;
#ifdef Q_OS_WIN
        if (const HWND hwnd = reinterpret_cast<HWND>(id)) {
            layered = (GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
        }
#else
        Q_UNUSED(id);
#endif
        qInfo().noquote() << "[PlayerView] Standalone layer diag"
                          << "| object:" << (w == m_hudWindow ? QStringLiteral("hudWindow") : w->objectName())
                          << "| isTopLevel:" << w->isWindow()
                          << "| wsExLayered:" << layered
                          << "| translucentAttr:"
                          << w->testAttribute(Qt::WA_TranslucentBackground)
                          << "| visible:" << w->isVisible();
    }
}

void PlayerView::setStandaloneHudVisible(bool visible)
{
    // B3：覆盖层全部在透明 HUD 顶层窗口里（applyStandaloneOverlay 迁移），
    // 而该窗口里有两种显隐语义的元素：
    //   ① 跟随鼠标活动显隐：top/bottom HUD、台标、网速标签；
    //   ② 独立控制显隐：侧边栏、统计、加载、OSD、弹幕层、动态弹层。
    // 所以 HUD 窗口本身常显（背景全透明，视觉上不可见），这里只切换 ① 类元素。
    if (!m_standalone) {
        return;
    }
    if (m_hudWindow && !m_hudWindow->isVisible() && width() > 0 && height() > 0) {
        // 首次显示：几何有效后才 show，避免在 (0,0)/零尺寸下闪现。
        syncStandaloneHudWindow();
        m_hudWindow->show();
    }
    if (m_topHUD) {
        m_topHUD->setVisible(visible);
    }
    if (m_bottomHUD) {
        m_bottomHUD->setVisible(visible);
    }
    if (m_logoLabel) {
        m_logoLabel->setVisible(visible);
    }
    // 网速标签：仅在配置启用时跟随 HUD 显隐（关闭时由既有逻辑保持隐藏）
    if (m_networkSpeedLabel && m_showNetworkSpeed) {
        m_networkSpeedLabel->setVisible(visible);
    }
    if (visible) {
        // 首次显示后确认 Qt 是否真的把 HUD 窗口设成 per-pixel translucent
        // （hasAlpha + hasNoNativeFrame 满足 → 内部加 WS_EX_LAYERED 并走
        // UpdateLayeredWindow 合成路径）。只打一次。
        logStandaloneLayerDiagnostics();
    }

    // 注意：字幕刻意不做 HUD 避让——与内嵌播放器保持一致，字幕始终停留在
    // 实际视频画面内（由 SubtitleStyleUtils 的 sub-pos 决定），不随 HUD 显隐移动。
}

void PlayerView::promoteStandaloneLayer(QWidget *layer)
{
    // 独立窗口模式下，动态创建的弹层（倍速/音轨/字幕菜单、播放器设置对话框等）
    // 若留在播放窗口里会被 mpv 直绘的视频层盖住，且它们自身也需要半透明
    // （QSS rgba）。因此与静态覆盖层一样迁进透明 HUD 顶层窗口。
    // 用全局坐标换算目标位置：兼容 layer 的旧 parent 不是本视图的情况
    // （HUD 窗口与本视图客户区完全重合，换算结果与直接用原 geometry 等价）。
    if (!m_standalone || !layer || !m_hudWindow) {
        return;
    }
    if (layer->parentWidget() == m_hudWindow) {
        return; // 已迁移（幂等）
    }
    // 同 applyStandaloneOverlay：layered 窗口下弹层自身的投影 effect 会让脏区
    // 溢出窗口边界（ModernScrollPanel 的 blur 20 + offset(0,4)，在底部按钮上方
    // 弹出时可能超出窗口顶部/底部），导致整帧更新被拒绝——表现为点击后弹层
    // "看不到 / 没反应"。layered 模式下移除（弹层本身无 effect 时是 no-op）。
    layer->setGraphicsEffect(nullptr);
    const QPoint globalTopLeft = layer->mapToGlobal(QPoint(0, 0));
    const QSize layerSize = layer->size();
    const bool wasHidden = layer->isHidden();
    layer->setParent(m_hudWindow);
    const QPoint localTopLeft = m_hudWindow->mapFromGlobal(globalTopLeft);
    layer->setGeometry(localTopLeft.x(), localTopLeft.y(), layerSize.width(), layerSize.height());
    if (!wasHidden) {
        layer->show();
    }
    layer->raise();
}


void PlayerView::updateLoadingState()
{
    if (m_isViewTearingDown)
    {
        if (m_loadingHideTimer)
        {
            m_loadingHideTimer->stop();
        }
        if (m_loadingOverlay)
        {
            m_loadingOverlay->forceStop();
        }
        m_loadingShown = false;
        return;
    }

    if (m_isBuffering || m_isSeeking)
    {
        // 缓冲 / 跳转中：取消待执行的隐藏，保证转圈一直在。
        if (m_loadingHideTimer)
        {
            m_loadingHideTimer->stop();
        }
        m_loadingOverlay->start();
        m_loadingShown = true;

        
        
        m_topHUD->raise();
        m_bottomHUD->raise();
        m_logoLabel->raise();         
        m_networkSpeedLabel->raise(); 
        m_rightSidebar->raise();
        m_rightTrigger->raise();
        if (m_activePopup)
            m_activePopup->raise();
        if (m_subtitleSubmenu)
            m_subtitleSubmenu->raise();
    }
    else if (m_loadingShown)
    {
        // 缓冲结束：不立刻隐藏，先等 kLoadingHideDelayMs 让抖动被吸收掉。
        if (m_loadingHideTimer)
        {
            m_loadingHideTimer->start();
        }
        else
        {
            m_loadingOverlay->stop();
            m_loadingShown = false;
        }
    }
}

QString PlayerView::formatDanmakuProviderLabel(QString provider) const
{
    provider = provider.trimmed();
    if (provider == QLatin1String("local-file"))
    {
        return tr("Local File");
    }
    if (provider == QLatin1String("dandanplay"))
    {
        return tr("DandanPlay");
    }
    if (provider == QLatin1String("danmu_api"))
    {
        return tr("LogVar / danmu_api");
    }
    return provider.isEmpty() ? tr("Unknown Source") : provider;
}

QString PlayerView::formatDanmakuSourceServiceLabel(QString provider, QString serverName) const
{
    provider = provider.trimmed();
    serverName = serverName.trimmed();
    if (!serverName.isEmpty())
    {
        return tr("Server: %1").arg(serverName);
    }
    return formatDanmakuProviderLabel(provider);
}

QString PlayerView::buildDanmakuSummaryText() const
{
    if (!m_danmakuController)
    {
        return tr("No danmaku loaded");
    }

    if (m_danmakuController->isLoading())
    {
        return tr("Loading danmaku...");
    }

    if (!m_danmakuController->hasPreparedDanmaku())
    {
        return tr("No danmaku loaded");
    }

    const QString sourceTitle = m_danmakuController->sourceTitle().trimmed().isEmpty()
                                    ? tr("Unknown Danmaku")
                                    : m_danmakuController->sourceTitle().trimmed();
    const QString countText = m_danmakuController->commentCount() > 0
                                  ? tr("%1 comments").arg(m_danmakuController->commentCount())
                                  : tr("Comment count unavailable");
    const QString sourceService =
        formatDanmakuSourceServiceLabel(m_danmakuController->sourceProvider(), m_danmakuController->sourceServerName());
    return tr("Current: %1 | %2 | %3").arg(sourceTitle, countText, sourceService);
}

QString PlayerView::buildDanmakuTooltipText() const
{
    if (!m_danmakuController)
    {
        return tr("Danmaku");
    }

    if (m_danmakuController->isLoading())
    {
        return tr("Danmaku (Loading...)");
    }

    if (!m_danmakuController->isDanmakuVisible())
    {
        return tr("Danmaku (Hidden)");
    }

    if (!m_danmakuController->hasPreparedDanmaku())
    {
        return tr("Danmaku");
    }

    const QString sourceTitle = m_danmakuController->sourceTitle().trimmed().isEmpty()
                                    ? tr("Unknown Danmaku")
                                    : m_danmakuController->sourceTitle().trimmed();
    const QString countText = m_danmakuController->commentCount() > 0
                                  ? tr("%1 comments").arg(m_danmakuController->commentCount())
                                  : tr("Comment count unavailable");
    const QString serverName = m_danmakuController->sourceServerName().trimmed();
    if (!serverName.isEmpty())
    {
        return tr("Danmaku\nSource: %1\nServer: %2\nProvider: %3\nComments: %4")
            .arg(sourceTitle, serverName, formatDanmakuProviderLabel(m_danmakuController->sourceProvider()), countText);
    }
    return tr("Danmaku\nSource: %1\nProvider: %2\nComments: %3")
        .arg(sourceTitle, formatDanmakuProviderLabel(m_danmakuController->sourceProvider()), countText);
}

void PlayerView::closeActivePlayerDialog()
{
    if (m_activePlayerDialog)
    {
        m_activePlayerDialog->close();
        m_activePlayerDialog = nullptr;
    }
}

void PlayerView::trackPlayerDialog(PlayerOverlayDialog *dialog)
{
    if (!dialog)
    {
        return;
    }

    closeActivePlayerDialog();
    m_activePlayerDialog = dialog;

    connect(dialog, &PlayerOverlayDialog::finished, this,
            [this, dialog](int)
            {
                if (m_activePlayerDialog == dialog)
                {
                    m_activePlayerDialog = nullptr;
                    if (!m_isViewTearingDown)
                    {
                        showControls();
                    }
                }
            });

    showControls();
    // 独立窗口模式下先提升为原生子窗口，再 open（必须在首次 show 之前）
    promoteStandaloneLayer(dialog);
    dialog->open();
}

bool PlayerView::shouldShowDanmakuHudControls() const
{
    if (m_danmakuController)
    {
        return m_danmakuController->isDanmakuEnabled();
    }

    return ConfigStore::instance()->get<bool>(ConfigKeys::PlayerDanmakuEnabled, false);
}

void PlayerView::updateDanmakuButtonState()
{
    if (m_isViewTearingDown || !m_danmakuBtn)
    {
        return;
    }

    const bool showDanmakuControls = shouldShowDanmakuHudControls();
    m_danmakuBtn->setVisible(showDanmakuControls);
    if (!showDanmakuControls || !m_danmakuController)
    {
        return;
    }

    const bool visible = m_danmakuController->isDanmakuVisible();
    const bool loading = m_danmakuController->isLoading();
    const bool loaded = m_danmakuController->hasDanmakuTrack() || m_danmakuController->hasPreparedDanmaku();

    
    
    
    
    m_danmakuBtn->setEnabled(true);
    if (loading)
    {
        m_danmakuBtn->setToolTip(tr("Danmaku (Loading...)"));
    }
    else if (!visible)
    {
        m_danmakuBtn->setToolTip(tr("Danmaku (Hidden)"));
    }
    else if (loaded && !m_danmakuController->sourceTitle().isEmpty())
    {
        m_danmakuBtn->setToolTip(buildDanmakuTooltipText());
    }
    else if (loaded)
    {
        m_danmakuBtn->setToolTip(buildDanmakuTooltipText());
    }
    else
    {
        m_danmakuBtn->setToolTip(tr("Danmaku"));
    }
}


void PlayerView::showCenteredPopup(QWidget *popup, QPushButton *btn)
{
    if (m_isViewTearingDown)
    {
        if (popup)
        {
            popup->deleteLater();
        }
        return;
    }

    hideHudMediaSwitcher();

    if (m_activePopup)
    {
        m_activePopup->hide();
        m_activePopup->close();
        m_activePopup->deleteLater();
        m_activePopup = nullptr;
    }
    closeSubtitleSubmenu();
    m_activePopup = popup;
    connect(popup, &QObject::destroyed, this,
            [this, popup]()
            {
                if (m_activePopup == popup)
                {
                    m_activePopup = nullptr;
                }
            });
    popup->adjustSize();

    // 按钮已随 bottomHUD 迁进透明 HUD 窗口（B3），不再是本视图的子孙——直接
    // mapTo(this, …) 会命中 Qt 的 "parent must be in parent hierarchy" 警告并
    // 返回 (0,0)，弹层被夹取到窗口左上角，用户感知为"点按钮没反应"。
    // 用全局坐标中转：mapToGlobal 只依赖按钮自身、mapFromGlobal 只依赖本视图，
    // 不要求两者存在祖先关系。
    const QPoint btnPos = mapFromGlobal(btn->mapToGlobal(QPoint(btn->width() / 2, 0)));
    int mx = btnPos.x() - popup->sizeHint().width() / 2;
    int my = btnPos.y() - popup->sizeHint().height() - 10;

    
    if (mx < 10)
    {
        mx = 10;
    }
    const int maxX = width() - popup->sizeHint().width() - 10;
    if (mx > maxX)
    {
        mx = qMax(10, maxX);
    }
    if (my < 10)
    {
        my = 10;
    }

    popup->move(mx, my);
    // 独立窗口模式下先提升为原生子窗口，再 show（必须在首次 show 之前）
    promoteStandaloneLayer(popup);
    popup->show();
    popup->raise();

    showControls(); 
}

void PlayerView::populateRightSidebarFromCache()
{
    if (!m_resumeList || !m_sidebarTitleLabel)
    {
        return;
    }

    m_resumeList->clear();

    if (m_isSeriesMode && !m_seriesId.isEmpty())
    {
        m_sidebarTitleLabel->setText(m_seriesName.isEmpty() ? tr("Episodes") : m_seriesName);

        QListWidgetItem *scrollTarget = nullptr;
        for (const MediaItem &season : m_switcherSeriesSeasons)
        {
            auto *seasonHeader = new QListWidgetItem(season.name);
            seasonHeader->setFlags(Qt::NoItemFlags);
            QFont headerFont = m_resumeList->font();
            headerFont.setBold(true);
            headerFont.setPixelSize(13);
            seasonHeader->setFont(headerFont);
            seasonHeader->setForeground(QColor(140, 140, 140));
            m_resumeList->addItem(seasonHeader);

            const auto episodes = m_switcherSeasonEpisodes.value(season.id);
            for (const MediaItem &episode : episodes)
            {
                QString label = tr("  S%1E%2  %3")
                                    .arg(episode.parentIndexNumber, 2, 10, QChar('0'))
                                    .arg(episode.indexNumber, 2, 10, QChar('0'))
                                    .arg(episode.name);
                auto *epItem = new QListWidgetItem(label);
                epItem->setData(Qt::UserRole, episode.id);
                epItem->setData(Qt::UserRole + 1, episode.userData.playbackPositionTicks);
                epItem->setData(Qt::UserRole + 2, formatMediaSwitcherPlaybackTitle(episode));
                epItem->setToolTip(formatMediaSwitcherPlaybackTitle(episode));

                if (episode.id == m_currentMediaId)
                {
                    QFont boldFont = m_resumeList->font();
                    boldFont.setBold(true);
                    epItem->setFont(boldFont);
                    epItem->setForeground(QColor(100, 180, 255));
                    scrollTarget = epItem;
                }

                m_resumeList->addItem(epItem);
            }
        }

        if (m_resumeList->count() == 0)
        {
            auto *empty = new QListWidgetItem(tr("No episodes found."));
            empty->setFlags(Qt::NoItemFlags);
            m_resumeList->addItem(empty);
            return;
        }

        if (scrollTarget)
        {
            m_resumeList->setCurrentItem(scrollTarget);
            m_resumeList->scrollToItem(scrollTarget, QAbstractItemView::PositionAtCenter);
        }
        return;
    }

    m_sidebarTitleLabel->setText(tr("Continue Watching"));
    for (const MediaItem &item : m_switcherResumeItems)
    {
        const QString displayTitle = formatMediaSwitcherPlaybackTitle(item);
        auto *listItem = new QListWidgetItem(displayTitle);
        listItem->setData(Qt::UserRole, item.id);
        listItem->setData(Qt::UserRole + 1, item.userData.playbackPositionTicks);
        listItem->setData(Qt::UserRole + 2, displayTitle);
        listItem->setToolTip(displayTitle);
        m_resumeList->addItem(listItem);
    }

    if (m_switcherResumeItems.isEmpty())
    {
        auto *empty = new QListWidgetItem(tr("No active media found."));
        empty->setFlags(Qt::NoItemFlags);
        m_resumeList->addItem(empty);
    }
}

QCoro::Task<void> PlayerView::ensureMediaSwitcherDataLoaded()
{
    if (m_currentMediaId.isEmpty())
    {
        co_return;
    }

    if (m_switcherCacheReady && m_switcherCacheMediaId == m_currentMediaId)
    {
        qDebug().noquote() << "[PlayerView] Reuse cached media switcher data"
                           << "| mediaId:" << m_currentMediaId << "| seriesMode:" << m_isSeriesMode;
        if (useHudMediaSwitcher() && m_mediaSwitchDrawer && m_mediaSwitchDrawer->isVisible())
        {
            syncHudMediaSwitcherContent();
        }
        if (!useHudMediaSwitcher() && m_isRightSidebarVisible)
        {
            populateRightSidebarFromCache();
        }
        co_return;
    }

    QPointer<PlayerView> guard(this);
    const QString currentMediaId = m_currentMediaId;
    // 跨服路由：切换器「继续观看」列表跟随当前播放条目所属服务器。
    const QString resumeServerId = m_currentMediaItem.serverId;
    const bool seriesMode = m_isSeriesMode && !m_seriesId.isEmpty();
    const QString seriesId = m_seriesId;

    clearMediaSwitcherCache();

    qDebug().noquote() << "[PlayerView] Loading media switcher data"
                       << "| mediaId:" << currentMediaId << "| seriesMode:" << seriesMode << "| seriesId:" << seriesId;

    try
    {
        if (seriesMode)
        {
            const QList<MediaItem> seasons = co_await m_core->mediaService()->getSeasons(
                    seriesId, m_currentMediaItem.serverId);
            if (!guard || guard->m_currentMediaId != currentMediaId)
            {
                co_return;
            }

            guard->m_switcherSeriesSeasons = seasons;
            for (const MediaItem &season : seasons)
            {
                const QList<MediaItem> episodes = co_await m_core->mediaService()->getEpisodes(
                        seriesId, season.id,
                        QStringLiteral("ParentIndexNumber,IndexNumber"),
                        QStringLiteral("Ascending"), m_currentMediaItem.serverId);
                if (!guard || guard->m_currentMediaId != currentMediaId)
                {
                    co_return;
                }

                guard->m_switcherSeasonEpisodes.insert(season.id, episodes);
            }

            try
            {
                const QList<MediaItem> items = co_await m_core->mediaService()->getResumeItems(30, "", "", resumeServerId);
                if (!guard || guard->m_currentMediaId != currentMediaId)
                {
                    co_return;
                }

                guard->m_switcherResumeItems = items;
            }
            catch (const std::exception &e)
            {
                qDebug() << "[PlayerView] Resume media fallback fetch failed:" << e.what();
            }
        }
        else
        {
            const QList<MediaItem> items = co_await m_core->mediaService()->getResumeItems(30, "", "", resumeServerId);
            if (!guard || guard->m_currentMediaId != currentMediaId)
            {
                co_return;
            }

            guard->m_switcherResumeItems = items;
        }

        if (!guard || guard->m_currentMediaId != currentMediaId)
        {
            co_return;
        }

        guard->m_switcherCacheMediaId = currentMediaId;
        guard->m_switcherCacheReady = true;
        qDebug().noquote() << "[PlayerView] Media switcher data ready"
                           << "| mediaId:" << currentMediaId << "| resumeItems:" << guard->m_switcherResumeItems.size()
                           << "| seasons:" << guard->m_switcherSeriesSeasons.size();
        if (guard->useHudMediaSwitcher() && guard->m_mediaSwitchDrawer && guard->m_mediaSwitchDrawer->isVisible())
        {
            guard->syncHudMediaSwitcherContent();
        }
        if (!guard->useHudMediaSwitcher() && guard->m_isRightSidebarVisible)
        {
            guard->populateRightSidebarFromCache();
        }
    }
    catch (const std::exception &e)
    {
        if (!guard || guard->m_currentMediaId != currentMediaId)
        {
            co_return;
        }

        guard->clearMediaSwitcherCache();
        qDebug() << "[PlayerView] Media switcher data fetch failed:" << e.what();
        if (guard->useHudMediaSwitcher() && guard->m_mediaSwitchDrawer && guard->m_mediaSwitchDrawer->isVisible())
        {
            guard->syncHudMediaSwitcherContent();
        }
        if (!guard->useHudMediaSwitcher() && guard->m_isRightSidebarVisible)
        {
            guard->populateRightSidebarFromCache();
        }
    }
}

QCoro::Task<void> PlayerView::switchFromMediaSwitcher(QString mediaId, QString title, long long startPositionTicks)
{
    QPointer<PlayerView> guard(this);
    if (mediaId.isEmpty())
    {
        co_return;
    }

    if (mediaId == m_currentMediaId)
    {
        hideRightSidebar();
        hideHudMediaSwitcher();
        if (m_activePopup)
        {
            m_activePopup->deleteLater();
            m_activePopup = nullptr;
        }
        closeSubtitleSubmenu();
        co_return;
    }

    m_switcherPendingItemId = mediaId;
    m_switcherPendingTitle = title;
    m_switcherPendingTicks = startPositionTicks;

    try
    {
        MediaItem detail = co_await m_core->mediaService()->getItemDetail(
            mediaId, m_currentMediaItem.serverId);

        if (!guard)
        {
            co_return;
        }
        if (m_switcherPendingItemId.isEmpty() || detail.id != m_switcherPendingItemId)
        {
            co_return;
        }

        if (detail.mediaSources.isEmpty())
        {
            PlaybackInfo playbackInfo = co_await m_core->mediaService()->getPlaybackInfo(
                detail.id, m_currentMediaItem.serverId);

            if (!guard)
            {
                co_return;
            }
            if (m_switcherPendingItemId.isEmpty() || detail.id != m_switcherPendingItemId)
            {
                co_return;
            }

            detail.mediaSources = playbackInfo.mediaSources;
        }

        MediaSourceInfo selectedSource;
        QString mediaSourceId = detail.id;
        if (!detail.mediaSources.isEmpty())
        {
            int sourceIdx = MediaSourcePreferenceUtils::resolvePreferredMediaSourceIndex(
                detail.mediaSources,
                ConfigStore::instance()->get<QString>(ConfigKeys::PlayerPreferredVersion).trimmed(),
                MediaSourcePreferenceUtils::rememberedMediaSourceId(
                    m_core->serverManager() ? m_core->serverManager()->activeProfile().id : QString(),
                    detail.id));
            if (sourceIdx < 0 || sourceIdx >= detail.mediaSources.size())
            {
                sourceIdx = 0;
            }

            // 记忆/偏好流规则已由 playMedia 统一应用（见 playMedia 收口处），
            // 这里只选 source，不再重复标记 isDefault。
            selectedSource = detail.mediaSources.at(sourceIdx);
            mediaSourceId = selectedSource.id;
        }

        QString streamUrl = selectedSource.id.isEmpty()
                                ? m_core->mediaService()->getStreamUrl(detail.id, mediaSourceId, detail.serverId)
                                : m_core->mediaService()->getStreamUrl(detail.id, selectedSource, detail.serverId);
        const QString titleFallback = detail.seriesId == m_seriesId ? m_seriesName : QString();
        QString resolvedTitle = MediaItemUtils::playbackTitle(detail, titleFallback);
        if (resolvedTitle.isEmpty())
        {
            resolvedTitle = m_switcherPendingTitle.trimmed();
        }

        qDebug().noquote() << "[PlayerView] Switching playback item"
                           << "| mediaId:" << detail.id << "| sourceId:" << mediaSourceId << "| title:" << resolvedTitle
                           << "| startTicks:" << m_switcherPendingTicks;

        hideRightSidebar();
        hideHudMediaSwitcher();
        if (m_activePopup)
        {
            m_activePopup->deleteLater();
            m_activePopup = nullptr;
        }
        closeSubtitleSubmenu();

        stopAndReport();
        m_toastLabel->hide();

        PlayerLaunchContext launchContext;
        launchContext.mediaItem = detail;
        launchContext.selectedSource = selectedSource;
        const QVariant sourceInfoVar = QVariant::fromValue(launchContext);

        QTimer::singleShot(250, this,
                           [this, id = detail.id, resolvedTitle, streamUrl, startTicks = m_switcherPendingTicks,
                            sourceInfoVar]() { playMedia(id, resolvedTitle, streamUrl, startTicks, sourceInfoVar); });

        m_switcherPendingItemId.clear();
        m_switcherPendingTitle.clear();
        m_switcherPendingTicks = 0;
    }
    catch (const std::exception &e)
    {
        if (!guard)
        {
            co_return;
        }
        qDebug() << "[PlayerView] Media switch failed:" << e.what();
        m_switcherPendingItemId.clear();
        m_switcherPendingTitle.clear();
        m_switcherPendingTicks = 0;
    }
}

void PlayerView::setupRightSidebar()
{
    m_rightSidebar = new QWidget(this);
    m_rightSidebar->setObjectName("playerRightSidebar"); 

    m_rightSidebar->setFixedWidth(220);
    m_rightSidebar->setGeometry(20000, 0, 220, 100);

    auto *shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(35);
    shadow->setColor(QColor(0, 0, 0, 150));
    shadow->setOffset(-5, 0);
    m_rightSidebar->setGraphicsEffect(shadow);

    auto *layout = new QVBoxLayout(m_rightSidebar);
    layout->setContentsMargins(0, 15, 0, 15);
    layout->setSpacing(10);

    m_sidebarTitleLabel = new QLabel(tr("Continue Watching"), m_rightSidebar);
    m_sidebarTitleLabel->setObjectName("playerSidebarTitle"); 
    layout->addWidget(m_sidebarTitleLabel);

    m_resumeList = new QListWidget(m_rightSidebar);
    m_resumeList->setObjectName("playerResumeList"); 
    m_resumeList->setFocusPolicy(Qt::NoFocus);
    m_resumeList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_resumeList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_resumeList->setTextElideMode(Qt::ElideRight);
    layout->addWidget(m_resumeList);

    connect(m_resumeList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) -> QCoro::Task<void>
            {
                if (!item || item->flags() == Qt::NoItemFlags)
                {
                    co_return;
                }

                co_await switchFromMediaSwitcher(item->data(Qt::UserRole).toString(),
                                                 item->data(Qt::UserRole + 2).toString(),
                                                 item->data(Qt::UserRole + 1).toLongLong());
            });

    m_rightSidebarAnim = new QPropertyAnimation(m_rightSidebar, "pos", this);
    m_rightSidebarAnim->setDuration(350);
    m_rightSidebarAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_rightSidebarAnim, &QPropertyAnimation::finished, this,
            [this]()
            {
                if (!m_rightSidebar || m_isRightSidebarVisible)
                {
                    return;
                }
                m_rightSidebar->hide();
            });

    m_rightSidebar->installEventFilter(this);
    m_rightSidebar->hide();
}

QCoro::Task<void> PlayerView::showRightSidebar()
{
    if (m_isViewTearingDown || useHudMediaSwitcher() || m_hasReportedStop)
    {
        co_return;
    }

    m_isRightSidebarVisible = true;
    const int sidebarY = m_topHUD->height();
    const QPoint hiddenPos(width() + 30, sidebarY);

    if (!m_rightSidebar || !m_rightSidebarAnim || !m_rightSidebarAnim->targetObject())
    {
        if (m_rightSidebar)
        {
            m_rightSidebar->move(width() - m_rightSidebar->width(), sidebarY);
            m_rightSidebar->show();
        }
    }
    else
    {
        if (m_rightSidebarAnim->state() == QAbstractAnimation::Running)
        {
            stopPropertyAnimationSafely(m_rightSidebarAnim);
        }
        if (!m_rightSidebar->isVisible())
        {
            m_rightSidebar->move(hiddenPos);
        }
        m_rightSidebar->show();
        m_rightSidebarAnim->setStartValue(m_rightSidebar->pos());
        m_rightSidebarAnim->setEndValue(QPoint(width() - m_rightSidebar->width(), sidebarY));
        m_rightSidebarAnim->start();
    }

    showControls();
    // 侧边栏的半透明在 showRightSidebar() 自身调用 promoteStandaloneLayer 时
    // 已设好（WA_TranslucentBackground 在 WA_NativeWindow 之前），无需再补。

    if (m_switcherCacheReady && m_switcherCacheMediaId == m_currentMediaId)
    {
        populateRightSidebarFromCache();
        co_return;
    }

    m_sidebarTitleLabel->setText(m_isSeriesMode ? (m_seriesName.isEmpty() ? tr("Episodes") : m_seriesName)
                                                : tr("Continue Watching"));
    m_resumeList->clear();
    auto *loadingItem = new QListWidgetItem(tr("Loading..."));
    loadingItem->setFlags(Qt::NoItemFlags);
    loadingItem->setForeground(QColor(140, 140, 140));
    m_resumeList->addItem(loadingItem);

    co_await ensureMediaSwitcherDataLoaded();
    if (!m_switcherCacheReady || m_switcherCacheMediaId != m_currentMediaId)
    {
        co_return;
    }

    populateRightSidebarFromCache();
}

void PlayerView::hideRightSidebar(bool immediate)
{
    immediate = immediate || m_isViewTearingDown;
    m_isRightSidebarVisible = false;
    if (!m_rightSidebar)
    {
        return;
    }

    const int sidebarY = m_topHUD ? m_topHUD->height() : 0;
    const QPoint hiddenPos(width() + 30, sidebarY);

    if (immediate || m_hasReportedStop || !m_rightSidebarAnim || !m_rightSidebarAnim->targetObject())
    {
        if (m_rightSidebarAnim)
        {
            stopPropertyAnimationSafely(m_rightSidebarAnim);
        }
        m_rightSidebar->move(hiddenPos);
        m_rightSidebar->hide();
        return;
    }

    if (!m_rightSidebar->isVisible())
    {
        m_rightSidebar->move(hiddenPos);
        return;
    }

    if (m_rightSidebarAnim->state() == QAbstractAnimation::Running)
    {
        stopPropertyAnimationSafely(m_rightSidebarAnim);
    }
    m_rightSidebarAnim->setStartValue(m_rightSidebar->pos());
    m_rightSidebarAnim->setEndValue(hiddenPos);
    m_rightSidebarAnim->start();
}



void PlayerView::stopAndReport()
{
    
    if (m_hasReportedStop)
    {
        return;
    }
    m_hasReportedStop = true;
    updatePowerInhibition();
    m_longPressHandler->setTeardown(true);
    stopTransientUiAnimations(m_isViewTearingDown);

    
    if (m_reportTimer)
        m_reportTimer->stop();
    if (m_mousePollTimer)
        m_mousePollTimer->stop();
    m_longPressHandler->stopKeyLongPress(false);
    m_longPressHandler->stopMouseEdgeLongPress();
    if (m_bufferTimer)
        m_bufferTimer->stop();
    if (m_hideTimer)
        m_hideTimer->stop();
    if (m_osdLayer)
        m_osdLayer->forceHide();
    if (m_toastTimer)
        m_toastTimer->stop();
    if (m_singleClickTimer)
        m_singleClickTimer->stop();

    hideRightSidebar(true);
    hideHudMediaSwitcher();
    if (m_activePopup)
    {
        m_activePopup->deleteLater();
        m_activePopup = nullptr;
    }
    closeSubtitleSubmenu();
    closeActivePlayerDialog();

    if (!m_currentMediaId.isEmpty() && m_core && m_core->mediaService())
    {
        long long currentTicks = static_cast<long long>(m_currentPosition * 10000000.0);

        auto *service = m_core->mediaService();
        QString mediaId = m_currentMediaId;
        QString sourceId = m_currentMediaSourceId;

        QString sessionId = m_currentPlaySessionId;
        m_currentPlaySessionId.clear();

        // 跨服路由：上报走所属 server（lambda 无捕获，通过参数传入）。
        const QString reportServerId = m_currentMediaItem.serverId;



        auto taskRoutine = [](MediaService *s, QString mId, QString sId, long long ticks,
                              QString sessId, QString serverId) -> QCoro::Task<void>
        {
            try
            {
                co_await s->reportPlaybackProgress(mId, sId, ticks, true, sessId,
                                                   serverId);
            }
            catch (const std::exception &e)
            {
                qDebug() << "Progress sync failed (ignored):" << e.what();
            }
            try
            {
                co_await s->reportPlaybackStopped(mId, sId, ticks, sessId,
                                                  serverId);
            }
            catch (const std::exception &e)
            {
                qDebug() << "Stop sync failed:" << e.what();
            }
        };

        
        
        auto *lingeringTask = new QCoro::Task<void>(
            taskRoutine(service, mediaId, sourceId, currentTicks, sessionId,
                        reportServerId));

        
        
        QTimer::singleShot(10000, m_core, [lingeringTask]() { delete lingeringTask; });
    }

    traktOnPlaybackStopped();

    if (m_mpvWidget)
    {
        if (m_danmakuController)
        {
            m_danmakuController->clearPlaybackContext();
        }
        
        disconnect(m_mpvWidget, &MpvWidget::positionChanged, this, &PlayerView::onPositionChanged);
        m_mpvWidget->controller()->command(QVariantList{"stop"});
    }
}

void PlayerView::beginViewTeardown()
{
    if (m_isViewTearingDown)
    {
        return;
    }

    m_isViewTearingDown = true;
    updatePowerInhibition();
    setPlayerChromeVisible(false);
    qDebug() << "[PlayerView] Begin teardown: stop timers, disconnect late signals, detach animations";

    if (m_reportTimer)
        m_reportTimer->stop();
    if (m_mousePollTimer)
        m_mousePollTimer->stop();
    if (m_bufferTimer)
        m_bufferTimer->stop();
    if (m_hideTimer)
        m_hideTimer->stop();
    if (m_toastTimer)
        m_toastTimer->stop();
    if (m_singleClickTimer)
        m_singleClickTimer->stop();

    if (m_longPressHandler)
    {
        m_longPressHandler->setTeardown(true);
        m_longPressHandler->stopKeyLongPress(false);
        m_longPressHandler->stopMouseEdgeLongPress();
    }

    disconnect(ConfigStore::instance(), nullptr, this, nullptr);
    if (m_danmakuController)
    {
        disconnect(m_danmakuController, nullptr, this, nullptr);
    }
    if (m_mpvWidget)
    {
        disconnect(m_mpvWidget, nullptr, this, nullptr);
        if (m_mpvWidget->controller())
        {
            disconnect(m_mpvWidget->controller(), nullptr, this, nullptr);
        }
    }

    stopTransientUiAnimations(true);
    detachFadeGroupTargets(m_fadeGroup);
    detachPropertyAnimationTarget(m_rightSidebarAnim);

    m_isRightSidebarVisible = false;
    if (m_mediaSwitchDrawer)
    {
        m_mediaSwitchDrawer->hide();
    }
    if (m_topHUD)
    {
        m_topHUD->hide();
    }
    if (m_bottomHUD)
    {
        m_bottomHUD->hide();
    }
    if (m_rightTrigger)
    {
        m_rightTrigger->hide();
    }
    if (m_rightSidebar)
    {
        m_rightSidebar->hide();
    }
    if (m_logoLabel)
    {
        m_logoLabel->hide();
    }
    if (m_networkSpeedLabel)
    {
        m_networkSpeedLabel->hide();
    }
    // 统计面板宿主是独立顶层窗口（不随本视图隐藏而隐藏），必须显式隐藏它，
    // 否则会在屏幕上残留一块浮在视频上的文字。面板本身不必也没必要单独 hide()
    // ——宿主窗口隐藏即可，避免下次显示时「窗口在、内容是空的」。
    if (m_statisticsWindow)
    {
        m_statisticsWindow->hide();
    }
    else if (m_statisticsOverlay)
    {
        m_statisticsOverlay->hide();
    }
    if (m_toastLabel)
    {
        m_toastLabel->hide();
    }
    if (m_activePopup)
    {
        m_activePopup->deleteLater();
        m_activePopup = nullptr;
    }
    closeSubtitleSubmenu();
    closeActivePlayerDialog();

    setCursorHidden(false);
}

void PlayerView::keyPressEvent(QKeyEvent *event)
{
    if (m_isViewTearingDown)
    {
        event->ignore();
        return;
    }

    bool isHudVisible = (m_topOpacity->opacity() > 0.0);

    if (event->key() == Qt::Key_Escape)
    {
        if (window()->isFullScreen())
        {
            window()->showNormal();
        }
        showControls();
        event->accept();
    }
    else if (event->key() == Qt::Key_Space)
    {
        if (!event->isAutoRepeat())
        {
            togglePlayPause();
        }
        showControls();
        event->accept();
    }
    else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
    {
        if (!event->isAutoRepeat())
        {
            toggleFullscreenWindow();
        }
        showControls();
        event->accept();
    }
    else if (event->key() == Qt::Key_Left)
    {
        if (!event->isAutoRepeat())
        {
            m_longPressHandler->startKeyLongPress(-1, !isHudVisible);
        }
        event->accept();
    }
    else if (event->key() == Qt::Key_Right)
    {
        if (!event->isAutoRepeat())
        {
            m_longPressHandler->startKeyLongPress(1, !isHudVisible);
        }
        event->accept();
    }
    else if (event->key() == Qt::Key_Up)
    {
        changeVolume(5, !isHudVisible);
        event->accept();
    }
    else if (event->key() == Qt::Key_Down)
    {
        changeVolume(-5, !isHudVisible);
        event->accept();
    }
    else if (event->key() == Qt::Key_PageUp)
    {
        if (!event->isAutoRepeat() && m_prevMediaBtn)
        {
            m_prevMediaBtn->click();
        }
        event->accept();
    }
    else if (event->key() == Qt::Key_PageDown)
    {
        if (!event->isAutoRepeat() && m_nextMediaBtn)
        {
            m_nextMediaBtn->click();
        }
        event->accept();
    }
    else
    {
        showControls();
        BaseView::keyPressEvent(event);
    }
}

void PlayerView::keyReleaseEvent(QKeyEvent *event)
{
    if (m_isViewTearingDown)
    {
        event->ignore();
        return;
    }

    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)
    {
        if (!event->isAutoRepeat())
        {
            m_longPressHandler->stopKeyLongPress();
        }
        event->accept();
    }
    else
    {
        BaseView::keyReleaseEvent(event);
    }
}


void PlayerView::setEffectivePlaybackSpeed(double speed)
{
    if (!m_mpvWidget || !m_mpvWidget->controller())
    {
        return;
    }

    m_currentSpeed = speed;
    m_mpvWidget->controller()->setProperty("speed", speed);
    if (m_speedBtn)
    {
        m_speedBtn->setText(PlayerLongPressHandler::formatSpeedText(speed));
    }
}

void PlayerView::stopTransientUiAnimations(bool immediate)
{
    if (m_longPressHandler)
    {
        m_longPressHandler->stopAllAnimations();
    }

    if (m_loadingHideTimer)
    {
        m_loadingHideTimer->stop();
    }
    m_loadingShown = false;
    if (m_loadingOverlay)
    {
        if (immediate)
        {
            m_loadingOverlay->forceStop();
        }
        else
        {
            m_loadingOverlay->stop();
        }
    }
    if (m_rightSidebarAnim)
    {
        stopPropertyAnimationSafely(m_rightSidebarAnim);
    }
    if (m_osdLayer)
    {
        m_osdLayer->stopAnimations();
    }
    if (m_fadeGroup)
    {
        stopFadeGroupSafely(m_fadeGroup);
    }
}

void PlayerView::updateOverlayLayout()
{
    if (m_isViewTearingDown)
    {
        return;
    }

    if (!m_mpvWidget || !m_topHUD || !m_bottomHUD)
    {
        return;
    }

    const int drawerHeight = (m_mediaSwitchDrawer && m_mediaSwitchDrawer->isVisible() && useHudMediaSwitcher())
                                 ? m_mediaSwitchDrawer->preferredDrawerHeight()
                                 : 0;
    const int targetBottomHudHeight = m_bottomHudBaseHeight + (drawerHeight > 0 ? drawerHeight + 8 : 0);

    m_mpvWidget->setGeometry(0, 0, width(), height());
    if (m_nativeDanmakuOverlay)
    {
        m_nativeDanmakuOverlay->setGeometry(0, 0, width(), height());
    }
    m_loadingOverlay->setGeometry(0, 0, width(), height());
    m_topHUD->setGeometry(0, 0, width(), m_topHUD->height());
    m_bottomHUD->setFixedHeight(targetBottomHudHeight);
    m_bottomHUD->setGeometry(0, height() - m_bottomHUD->height(), width(), m_bottomHUD->height());

    if (m_mediaSwitchDrawer)
    {
        m_mediaSwitchDrawer->setVisible(drawerHeight > 0 && useHudMediaSwitcher());
    }

    if (m_logoLabel)
    {
        m_logoLabel->move(24, m_topHUD->height());
    }

    if (m_networkSpeedLabel)
    {
        m_networkSpeedLabel->setGeometry(width() - 210, m_topHUD->height(), 200, 30);
    }

    if (m_statisticsOverlay)
    {
        int statisticsY = m_topHUD->height() + 10;
        if (m_logoLabel && m_logoLabel->isVisible())
        {
            statisticsY = qMax(statisticsY, m_logoLabel->y() + m_logoLabel->height() + 8);
        }

        const int statisticsWidth = qMax(280, qMin(width() - 24, 620));
        m_statisticsOverlay->setFixedWidth(statisticsWidth);
        m_statisticsOverlay->setFixedHeight(m_statisticsOverlay->sizeHint().height());
        // 面板本身住在独立顶层窗口（m_statisticsWindow）里，不在本视图坐标系中，
        // 所以不在这里 move()，只记录目标位置并由 syncStatisticsWindow() 换算。
        m_statisticsOverlayX = 12;
        m_statisticsOverlayY = statisticsY;
        syncStatisticsWindow();
    }

    const int sidebarY = m_topHUD->height();
    const int sidebarH = height() - m_topHUD->height() - m_bottomHUD->height();

    m_rightTrigger->setGeometry(width() - 15, sidebarY, 15, sidebarH);

    if (!m_isRightSidebarVisible)
    {
        m_rightSidebar->setGeometry(width() + 30, sidebarY, m_rightSidebar->width(), sidebarH);
    }
    else
    {
        m_rightSidebar->setGeometry(width() - m_rightSidebar->width(), sidebarY, m_rightSidebar->width(), sidebarH);
    }

    if (m_osdLayer)
    {
        m_osdLayer->updateGeometry(width(), height());
    }

    if (m_longPressHandler)
    {
        m_longPressHandler->updateGeometry(width(), height(), m_topHUD ? m_topHUD->height() : 0);
    }

    if (m_activePopup)
    {
        m_activePopup->raise();
    }
    if (m_subtitleSubmenu)
    {
        m_subtitleSubmenu->raise();
    }

    // standalone：HUD 窗口常显（所有覆盖层都住在里面，见 setStandaloneHudVisible）。
    // 若因任何原因尚未显示而几何已有效，这里兜底补一次 show（幂等、廉价），
    // 保证侧边栏/统计/弹层等独立显隐的元素任何时候都能显示出来。
    if (m_standalone && m_hudWindow && !m_hudWindow->isVisible() && width() > 0 && height() > 0)
    {
        syncStandaloneHudWindow();
        m_hudWindow->show();
    }

    updateTitleElision();
}

void PlayerView::resizeEvent(QResizeEvent *event)
{
    BaseView::resizeEvent(event);

    if (m_isViewTearingDown)
    {
        return;
    }

    // HUD 透明顶层窗口（所有覆盖层的宿主）跟随本视图客户区尺寸
    syncStandaloneHudWindow();

    
    
    
    QTimer::singleShot(
        0, this,
        [this]()
        {
            if (window() && m_maxBtn)
            {
                bool isMax = window()->isMaximized();
                if (!m_maxBtn->property("isMax").isValid() || m_maxBtn->property("isMax").toBool() != isMax)
                {
                    m_maxBtn->setIcon(QIcon(isMax ? ":/svg/player/restore.svg" : ":/svg/player/max.svg"));
                    m_maxBtn->setProperty("isMax", isMax);
                }
            }
        });
    updateOverlayLayout();
}

void PlayerView::showEvent(QShowEvent *event)
{
    BaseView::showEvent(event);

    // 独立窗口模式：HUD 是独立顶层窗口，不随本视图显隐自动跟随——重新显示
    // 本视图时同步几何并显示（首次显示同样走这里）。
    if (m_standalone && m_hudWindow && width() > 0 && height() > 0)
    {
        syncStandaloneHudWindow();
        m_hudWindow->show();
    }
    // 统计窗口：原来是打开状态就一并恢复（hideEvent 里被强制隐藏了）。
    if (m_showStatisticsOverlay && m_statisticsWindow && width() > 0 && height() > 0)
    {
        syncStatisticsWindow();
        m_statisticsWindow->show();
    }
}

void PlayerView::hideEvent(QHideEvent *event)
{
    BaseView::hideEvent(event);

    // 独立窗口模式：HUD 是独立顶层窗口，本视图隐藏（如切回详情页）时必须主动
    // 隐藏，否则会在屏幕上残留浮层；再次显示由 showEvent 恢复。
    if (m_standalone && m_hudWindow)
    {
        m_hudWindow->hide();
    }
    // 统计窗口同理：它是独立顶层窗口，不随本视图隐藏而隐藏。
    if (m_statisticsWindow)
    {
        m_statisticsWindow->hide();
    }
}


void PlayerView::updateTitleElision()
{
    if (m_fullTitle.isEmpty() || !m_titleLabel)
    {
        return;
    }

    int safeWidth = m_titleLabel->contentsRect().width();
    if (safeWidth <= 0)
    {
        safeWidth = this->width() - 320;
    }
    if (safeWidth < 50)
    {
        safeWidth = 50;
    }

    
    QFontMetrics fm(m_titleLabel->font());
    QString elided = fm.elidedText(m_fullTitle, Qt::ElideRight, safeWidth);

    m_titleLabel->setText(elided);
    m_titleLabel->setToolTip(m_fullTitle); 
}

bool PlayerView::eventFilter(QObject *watched, QEvent *event)
{
    if (m_isViewTearingDown)
    {
        return BaseView::eventFilter(watched, event);
    }

    // HUD 透明顶层窗口自身的鼠标活动 → 保持 HUD 显示（显隐逻辑）。
    if (watched == m_hudWindow && event->type() == QEvent::MouseMove)
    {
        auto *me = static_cast<QMouseEvent *>(event);
        handlePointerActivity(me->globalPosition().toPoint());
        return false;
    }
    // 播放窗口移动/缩放/显示 → 同步覆盖层宿主窗口位置（播放窗口本身
    // move 时 PlayerView 的局部坐标不变、resizeEvent 不触发，必须在这里同步；
    // Show 覆盖首次显示——那时 mapToGlobal 才有真实坐标）。
    // 统计面板宿主是独立顶层窗口，两种模式（standalone / 内嵌）都要跟随，
    // 所以它的同步**不**受 m_standalone 限制。
    if (watched == window() &&
        (event->type() == QEvent::Move || event->type() == QEvent::Resize ||
         event->type() == QEvent::Show))
    {
        if (m_standalone)
        {
            syncStandaloneHudWindow();
        }
        syncStatisticsWindow();
        return BaseView::eventFilter(watched, event);
    }

    if (watched == m_mpvWidget || watched == this || watched == m_topHUD || watched == m_titleLabel ||
        watched == m_bottomHUD)
    {
        
        
        if (m_activePlayerDialog && m_activePlayerDialog->isVisible())
        {
            const auto type = event->type();
            if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease ||
                type == QEvent::MouseButtonDblClick || type == QEvent::MouseMove || type == QEvent::Wheel)
            {
                return true;
            }
        }

        if (event->type() == QEvent::MouseButtonPress)
        {
            auto *me = static_cast<QMouseEvent *>(event);
            const QPoint clickPos = this->mapFromGlobal(me->globalPosition().toPoint());
            const bool isOnHud = m_topHUD->geometry().contains(clickPos) || m_bottomHUD->geometry().contains(clickPos);

            
            
            
            if (me->button() == Qt::BackButton || me->button() == Qt::XButton1)
            {
                onBackClicked();
                return true; 
            }

            
            if (m_activePopup)
            {
                QPoint localPos = this->mapFromGlobal(me->globalPosition().toPoint());
                if (!m_activePopup->geometry().contains(localPos))
                {
                    m_activePopup->hide();
                    m_activePopup->close();
                    m_activePopup->deleteLater();
                    m_activePopup = nullptr;
                    closeSubtitleSubmenu();
                    return true;
                }
            }

            showControls();
            if (me->button() == Qt::LeftButton && watched != m_bottomHUD)
            {
                m_singleClickTimer->stop();

                if (!isOnHud)
                {
                    m_longPressHandler->startMouseEdgeLongPress(clickPos.x() < width() / 2 ? -1 : 1);
                }

                
                m_dragPos = me->globalPosition().toPoint();
                m_didDrag = false; 

                // 字幕拖动：按在字幕带内时先进入待定状态，移动超过阈值后由
                // MouseMove 分支升级为拖动；拖动开关关闭时此处直接返回 false。
                m_subtitleDragPending = false;
                bool subtitleDragHit = false;
                if (!isOnHud)
                {
                    subtitleDragHit = beginSubtitleDragIfHit(me->globalPosition().toPoint());
                }
                // 诊断：拖动链路第一环——事件到达谁、是否命中字幕带。
                if (ConfigStore::instance()->get<bool>(ConfigKeys::PlayerSubtitleDragEnabled, false))
                {
                    qInfo().noquote()
                        << "[SubtitleDrag] press"
                        << "| watched:" << (watched == m_mpvWidget ? QStringLiteral("mpvWidget")
                                          : watched == this ? QStringLiteral("playerView")
                                          : watched == m_hudWindow ? QStringLiteral("hudWindow")
                                          : watched == m_topHUD ? QStringLiteral("topHUD")
                                          : watched == m_bottomHUD ? QStringLiteral("bottomHUD")
                                          : QStringLiteral("other"))
                        << "| isOnHud:" << isOnHud
                        << "| hit:" << subtitleDragHit;
                }
            }
        }
        else if (event->type() == QEvent::MouseMove)
        {
            auto *me = static_cast<QMouseEvent *>(event);
            m_lastMousePos = me->globalPosition().toPoint();
            handlePointerActivity(m_lastMousePos);
            const QPoint localPos = this->mapFromGlobal(me->globalPosition().toPoint());
            const bool isOnHud = m_topHUD->geometry().contains(localPos) || m_bottomHUD->geometry().contains(localPos);
            const bool movedEnough =
                (me->globalPosition().toPoint() - m_dragPos).manhattanLength() > QApplication::startDragDistance();

            if (m_longPressHandler->mouseEdgeLongPressDirection() != 0 && (movedEnough || isOnHud))
            {
                m_longPressHandler->stopMouseEdgeLongPress();
            }

            if ((me->buttons() & Qt::LeftButton) && watched != m_bottomHUD)
            {

                // 字幕拖动优先于窗口拖动：命中字幕带后等移动阈值，超过阈值
                // 升级为拖动并实时写位置，不再触发窗口移动。
                if (m_subtitleDragPending || m_subtitleDragActive)
                {
                    if (!m_subtitleDragActive && movedEnough)
                    {
                        m_subtitleDragActive = true;
                        m_didDrag = true; 
                        m_longPressHandler->stopMouseEdgeLongPress();
                        qInfo().noquote()
                            << "[SubtitleDrag] active | startPos:" << m_subtitleDragStartPos
                            << "| target:" << (m_subtitleDragTargetSecondary
                                                   ? QStringLiteral("secondary")
                                                   : QStringLiteral("primary"));
                    }
                    if (m_subtitleDragActive)
                    {
                        updateSubtitleDrag(me->globalPosition().toPoint());
                    }
                    return true;
                }

                
                if (!window()->isFullScreen())
                {
                    
                    if (movedEnough)
                    {
                        m_didDrag = true; 
                        if (window() && window()->windowHandle())
                        {
                            
                            window()->windowHandle()->startSystemMove();
                        }
                    }
                }
                return true;
            }
        }
        else if (event->type() == QEvent::MouseButtonRelease)
        {
            auto *me = static_cast<QMouseEvent *>(event);
            // 字幕拖动收尾：落配置 + 提示，不进入单击暂停（m_didDrag 已置位）。
            if (m_subtitleDragActive)
            {
                m_subtitleDragActive = false;
                m_subtitleDragPending = false;
                finishSubtitleDrag();
                return true;
            }
            m_subtitleDragPending = false;

            const bool consumedByMouseEdge =
                me->button() == Qt::LeftButton && m_longPressHandler->stopMouseEdgeLongPress();
            if (consumedByMouseEdge)
            {
                m_singleClickTimer->stop();
                m_didDrag = true;
                return true;
            }

            
            QPoint clickPos = this->mapFromGlobal(me->globalPosition().toPoint());
            bool isOnHud = m_topHUD->geometry().contains(clickPos) || m_bottomHUD->geometry().contains(clickPos);
            if (me->button() == Qt::LeftButton && !isOnHud && !m_didDrag &&
                ConfigStore::instance()->get<bool>(ConfigKeys::PlayerClickToPause, true))
            {
                m_singleClickTimer->start(); 
            }
        }
        else if (event->type() == QEvent::MouseButtonDblClick)
        {
            auto *me = static_cast<QMouseEvent *>(event);
            m_subtitleDragPending = false;
            if (me->button() == Qt::LeftButton)
            {
                m_longPressHandler->stopMouseEdgeLongPress();
            }
            QPoint clickPos = this->mapFromGlobal(me->globalPosition().toPoint());
            bool isOnHud = m_topHUD->geometry().contains(clickPos) || m_bottomHUD->geometry().contains(clickPos);
            if (me->button() == Qt::LeftButton && !isOnHud)
            {
                m_singleClickTimer->stop(); 
                m_didDrag = true;           
                toggleFullscreenWindow();
                return true;
            }
        }
    }

    
    if (watched == m_rightTrigger && event->type() == QEvent::Enter)
    {
        showRightSidebar();
        return true;
    }
    
    if (watched == m_rightSidebar && event->type() == QEvent::Leave)
    {
        QPoint globalPos = QCursor::pos();
        QPoint localPos = m_rightSidebar->mapFromGlobal(globalPos);
        if (!m_rightSidebar->rect().contains(localPos))
        {
            hideRightSidebar();
        }
        return false;
    }

    if (event->type() == QEvent::Wheel)
    {
        
        if (m_activePlayerDialog && m_activePlayerDialog->isVisible())
        {
            return true;
        }
        if (watched == m_volumeBtn || watched == m_volumeSlider || watched == m_bottomHUD || watched == m_mpvWidget ||
            watched == this)
        {
            showControls();
            auto *we = static_cast<QWheelEvent *>(event);
            changeVolume(we->angleDelta().y() > 0 ? 5 : -5);
            return true;
        }
    }

    return BaseView::eventFilter(watched, event);
}

// ---- 字幕拖动（B 部分）-------------------------------------------------
// 拖动开关开启时，按住字幕附近并上下拖动即可实时调整字幕位置；拖动只改
// mpv 的 sub-pos / secondary-sub-pos（与字幕设置里的位置滑块同范围 60-100）。
//
// 坐标参考系：字幕垂直位置相对视频显示区（dwidth x dheight，居中于
// MpvWidget）——pos=100 贴视频区底部、pos=0 在顶部，换算
// y = videoTop + displayH * pos / 100。ass-track 弹幕模式下内容主字幕物理上
// 位于 secondary 通道（弹幕占用 sid），此时拖动写 secondary-sub-pos。

bool PlayerView::beginSubtitleDragIfHit(const QPoint &globalPos)
{
    if (!ConfigStore::instance()->get<bool>(ConfigKeys::PlayerSubtitleDragEnabled,
                                            false))
    {
        return false;
    }
    if (!m_mpvWidget || !m_mpvWidget->controller() || !m_danmakuController ||
        m_mpvWidget->height() <= 0)
    {
        return false;
    }

    // 视频显示尺寸（随窗口缩放）；无效时退化为 widget 尺寸。
    int displayW = m_mpvWidget->controller()
                       ->getProperty(QStringLiteral("video-params/dw"))
                       .toInt();
    int displayH = m_mpvWidget->controller()
                       ->getProperty(QStringLiteral("video-params/dh"))
                       .toInt();
    if (displayW <= 0 || displayH <= 0)
    {
        displayW = m_mpvWidget->width();
        displayH = m_mpvWidget->height();
    }

    // ass-track 弹幕模式：弹幕占用 sid，内容主字幕物理上位于 secondary 通道。
    const bool contentOnSecondaryChannel =
        m_danmakuController->secondarySubtitleBlockedByDanmaku();
    const double videoTop = (m_mpvWidget->height() - displayH) / 2.0;
    const auto referenceY = [videoTop, displayH](double pos) {
        return videoTop + displayH * (pos / 100.0);
    };
    const auto readMpvPos = [this](const char *name) {
        return m_mpvWidget->controller()
            ->getProperty(QString::fromLatin1(name))
            .toDouble();
    };

    const QPoint localPos = m_mpvWidget->mapFromGlobal(globalPos);
    const double tolerance = qMax(56.0, displayH / 16.0);

    bool found = false;
    bool foundTargetSecondary = false;
    bool foundWritesSecondaryPos = false;
    double foundPos = 0.0;
    double bestDistance = tolerance;
    double mainDistance = -1.0;      // -1 = 候选不适用（未选中/被占轨）
    double secondaryDistance = -1.0;

    // 候选 1：内容主字幕（已选中时）
    if (m_danmakuController->selectedSubtitleTrackId() > 0)
    {
        const double pos = contentOnSecondaryChannel
                               ? readMpvPos("secondary-sub-pos")
                               : readMpvPos("sub-pos");
        mainDistance = qAbs(localPos.y() - referenceY(pos));
        if (mainDistance < bestDistance)
        {
            found = true;
            foundTargetSecondary = false;
            foundWritesSecondaryPos = contentOnSecondaryChannel;
            foundPos = pos;
            bestDistance = mainDistance;
        }
    }

    // 候选 2：副字幕（启用且未被弹幕占轨阻塞时）
    if (!contentOnSecondaryChannel &&
        ConfigStore::instance()->get<bool>(
            ConfigKeys::PlayerSubtitleSecondaryEnabled, false) &&
        m_danmakuController->secondarySubtitleTrackId() > 0)
    {
        const double pos = readMpvPos("secondary-sub-pos");
        secondaryDistance = qAbs(localPos.y() - referenceY(pos));
        if (secondaryDistance < bestDistance)
        {
            found = true;
            foundTargetSecondary = true;
            foundWritesSecondaryPos = true;
            foundPos = pos;
        }
    }

    // 诊断：命中判定的全部输入（几何换算 + 距离 + 容差）。
    qInfo().noquote()
        << "[SubtitleDrag] hit test"
        << "| selectedId:" << m_danmakuController->selectedSubtitleTrackId()
        << "| secondaryId:" << m_danmakuController->secondarySubtitleTrackId()
        << "| contentOnSecondary:" << contentOnSecondaryChannel
        << "| display:" << QStringLiteral("%1x%2").arg(displayW).arg(displayH)
        << "| widgetH:" << m_mpvWidget->height()
        << "| videoTop:" << videoTop
        << "| localY:" << localPos.y()
        << "| subPos:" << readMpvPos("sub-pos")
        << "| secPos:" << readMpvPos("secondary-sub-pos")
        << "| mainDist:" << mainDistance
        << "| secDist:" << secondaryDistance
        << "| tolerance:" << tolerance
        << "| found:" << found
        << "| target:" << (foundTargetSecondary ? QStringLiteral("secondary")
                                                : QStringLiteral("primary"));

    if (!found)
    {
        return false;
    }

    m_subtitleDragPending = true;
    m_subtitleDragTargetSecondary = foundTargetSecondary;
    m_subtitleDragWritesSecondaryPos = foundWritesSecondaryPos;
    m_subtitleDragStartPos = foundPos;
    m_subtitleDragLastPos = foundPos;
    m_subtitleDragStartGlobalPos = globalPos;
    m_subtitleDragViewHeight = qMax(1, displayH);
    return true;
}

void PlayerView::updateSubtitleDrag(const QPoint &globalPos)
{
    if (!m_mpvWidget || !m_mpvWidget->controller())
    {
        return;
    }
    const int deltaY = globalPos.y() - m_subtitleDragStartGlobalPos.y();
    double newPos = m_subtitleDragStartPos +
                    deltaY * 100.0 / qMax(1, m_subtitleDragViewHeight);
    // 与字幕设置里的位置滑块保持一致的范围（0-100）。
    newPos = qBound(0.0, newPos, 100.0);
    if (qFuzzyCompare(newPos, m_subtitleDragLastPos))
    {
        return;
    }
    m_subtitleDragLastPos = newPos;
    m_mpvWidget->controller()->setProperty(
        m_subtitleDragWritesSecondaryPos ? QStringLiteral("secondary-sub-pos")
                                         : QStringLiteral("sub-pos"),
        newPos);
}

void PlayerView::finishSubtitleDrag()
{
    const int value = qBound(0, qRound(m_subtitleDragLastPos), 100);
    const QString key =
        m_subtitleDragTargetSecondary
            ? QString::fromLatin1(ConfigKeys::PlayerSubtitleSecondaryPosition)
            : QString::fromLatin1(ConfigKeys::PlayerSubtitlePosition);
    // 写入配置后由 valueChanged → applySubtitleStyleSettings 按当前通道场景
    // 重新应用（与拖动中写入值一致，幂等）；值未变化则不重复写。
    if (ConfigStore::instance()->get<int>(key, 0) != value)
    {
        ConfigStore::instance()->set(key, value);
    }
    qInfo().noquote()
        << "[SubtitleDrag] finish | value:" << value
        << "| target:" << (m_subtitleDragTargetSecondary ? QStringLiteral("secondary")
                                                         : QStringLiteral("primary"))
        << "| writesSecondaryPos:" << m_subtitleDragWritesSecondaryPos;
    showToast(tr("Subtitle Position: %1%").arg(value));
}

void PlayerView::handlePointerActivity(const QPoint &globalPos)
{
    if (m_isViewTearingDown || m_hasReportedStop)
    {
        return;
    }

    const QPoint localPos = mapFromGlobal(globalPos);
    if (!rect().contains(localPos))
    {
        return;
    }

    // standalone 下 m_rightTrigger（窗口右侧 15px 的鼠标热区）被排除在原生提升
    // 列表之外——它没有背景，提升为原生窗口后会渲染成一条黑色窄条。这里改按
    // 坐标命中它的区域来弹出选集侧边栏，功能与原生 Enter 事件等价。
    if (m_standalone && m_rightTrigger && m_rightTrigger->isVisible() &&
        !m_isRightSidebarVisible &&
        m_rightTrigger->geometry().contains(localPos))
    {
        showRightSidebar();
    }

    setCursorHidden(false);

    if (m_osdLayer && m_osdLayer->isVisible())
    {
        m_osdLayer->forceHide();
    }

    if (m_isPlaying)
    {
        m_hideTimer->start(m_standalone ? kStandaloneHudAutoHideDelayMs
                                        : kHudAutoHideDelayMs);
    }
    else
    {
        m_hideTimer->stop();
    }

    if (areControlsFullyVisible())
    {
        return;
    }

    showControls();
}

void PlayerView::setCursorHidden(bool hidden)
{
    const auto targetShape = hidden ? Qt::BlankCursor : Qt::ArrowCursor;

    if (cursor().shape() != targetShape)
    {
        setCursor(targetShape);
    }

    if (m_mpvWidget && m_mpvWidget->cursor().shape() != targetShape)
    {
        m_mpvWidget->setCursor(targetShape);
    }
}

void PlayerView::setPlayerChromeVisible(bool visible)
{
    if (m_playerChromeVisible == visible)
    {
        return;
    }

    m_playerChromeVisible = visible;
    Q_EMIT playerChromeVisibilityChanged(visible);
}

bool PlayerView::areControlsFullyVisible() const
{
    if (m_standalone)
    {
        // standalone 下 HUD 元素在透明 HUD 窗口里（该窗口本身常显，不能作为
        // 判据），以 topHUD 的可见性为准。
        return m_topHUD && m_topHUD->isVisible();
    }
    return m_topOpacity && m_topOpacity->opacity() >= 1.0 &&
           (!m_fadeGroup || m_fadeGroup->state() != QAbstractAnimation::Running);
}


void PlayerView::showControls()
{
    if (m_isViewTearingDown || m_hasReportedStop)
    {
        return;
    }

    setPlayerChromeVisible(true);
    setCursorHidden(false);

    
    if (m_osdLayer && m_osdLayer->isVisible())
    {
        m_osdLayer->forceHide();
    }

    if (m_isPlaying)
    {
        m_hideTimer->start(m_standalone ? kStandaloneHudAutoHideDelayMs
                                        : kHudAutoHideDelayMs);
    }
    else
    {
        m_hideTimer->stop(); 
    }

    if (m_standalone)
    {
        // standalone 下 HUD 是原生窗口，opacity 动画（QGraphicsOpacityEffect）
        // 对其无效，直接显隐。
        setStandaloneHudVisible(true);
        return;
    }

    if (m_topOpacity->opacity() >= 1.0 && m_fadeGroup->state() != QAbstractAnimation::Running)
    {
        return;
    }

    if (m_fadeGroup->state() == QAbstractAnimation::Running)
    {
        stopFadeGroupSafely(m_fadeGroup);
    }

    bool canAnimate = true;
    for (int i = 0; i < m_fadeGroup->animationCount(); ++i)
    {
        auto *anim = qobject_cast<QPropertyAnimation *>(m_fadeGroup->animationAt(i));
        if (!anim || !anim->targetObject())
        {
            canAnimate = false;
            break;
        }
        anim->setStartValue(anim->targetObject()->property("opacity")); 
        anim->setEndValue(1.0);
    }
    if (!canAnimate)
    {
        if (m_topOpacity)
            m_topOpacity->setOpacity(1.0);
        if (m_bottomOpacity)
            m_bottomOpacity->setOpacity(1.0);
        if (m_logoOpacity)
            m_logoOpacity->setOpacity(1.0);
        if (m_speedOpacity)
            m_speedOpacity->setOpacity(1.0);
        return;
    }
    m_fadeGroup->setDirection(QAbstractAnimation::Forward);
    m_fadeGroup->start();
}


void PlayerView::hideControls()
{
    if (m_isViewTearingDown || m_hasReportedStop)
    {
        return;
    }

    if (!m_isPlaying)
    {
        return;
    }

    QPoint localPos = this->mapFromGlobal(QCursor::pos());
    bool isAppActive = this->window()->isActiveWindow();
    bool isMouseInside = this->rect().contains(localPos);
    
    bool isMouseInsidePopup = m_activePopup && m_activePopup->geometry().contains(localPos);
    const bool hasActivePlayerDialog = m_activePlayerDialog && m_activePlayerDialog->isVisible();

    
    if (hasActivePlayerDialog)
    {
        m_hideTimer->start(1000);
        return;
    }

    // 弹层菜单打开时同样保持 HUD 与弹层可见：用户可能在滚动很长的列表（如
    // 字幕轨列表），自动隐藏的鼠标判定/时序会把菜单一起关掉（用户实测"滚
    // 动时弹出菜单消失，需要重新打开"）。
    if (m_activePopup || m_subtitleSubmenu)
    {
        m_hideTimer->start(1000);
        return;
    }

    if (isAppActive && isMouseInside &&
        (m_topHUD->geometry().contains(localPos) || m_bottomHUD->geometry().contains(localPos) || isMouseInsidePopup ||
         (m_isRightSidebarVisible && m_rightSidebar->geometry().contains(localPos))))
    {
        m_hideTimer->start(1000);
        return;
    }

    hideRightSidebar();
    hideHudMediaSwitcher();
    setPlayerChromeVisible(false);

    if (m_activePopup)
    {
        m_activePopup->hide();
        m_activePopup->close();
        m_activePopup->deleteLater();
        m_activePopup = nullptr;
    }

    if (m_standalone)
    {
        // standalone 下 HUD 是原生窗口，opacity 淡出对其无效（会常驻显示），
        // 改用显隐。
        setStandaloneHudVisible(false);
        setCursorHidden(true);
        return;
    }

    if (m_topOpacity->opacity() <= 0.0)
    {
        return;
    }

    if (m_fadeGroup->state() == QAbstractAnimation::Running)
    {
        stopFadeGroupSafely(m_fadeGroup);
    }

    bool canAnimate = true;
    for (int i = 0; i < m_fadeGroup->animationCount(); ++i)
    {
        auto *anim = qobject_cast<QPropertyAnimation *>(m_fadeGroup->animationAt(i));
        if (!anim || !anim->targetObject())
        {
            canAnimate = false;
            break;
        }
        anim->setStartValue(anim->targetObject()->property("opacity")); 
        anim->setEndValue(0.0);
    }
    if (!canAnimate)
    {
        if (m_topOpacity)
            m_topOpacity->setOpacity(0.0);
        if (m_bottomOpacity)
            m_bottomOpacity->setOpacity(0.0);
        if (m_logoOpacity)
            m_logoOpacity->setOpacity(0.0);
        if (m_speedOpacity)
            m_speedOpacity->setOpacity(0.0);
        return;
    }
    m_fadeGroup->setDirection(QAbstractAnimation::Forward);
    m_fadeGroup->start();

    setCursorHidden(true);
}

void PlayerView::onMpvPropertyChanged(const QString &property, const QVariant &value)
{
    if (m_isViewTearingDown)
    {
        return;
    }

    if (property == "cache-speed")
    {
        qint64 speedBytes = value.toLongLong();
        if (!m_useRelayNetworkSpeed)
        {
            m_effectiveNetworkSpeed = speedBytes;
            m_networkSpeedLabel->setText(formatDataRateValue(speedBytes));
        }
    }
    else if (property == "mute")
    {
        m_isMuted = value.toBool();
        m_volumeBtn->setIcon(QIcon(m_isMuted ? ":/svg/player/volume-mute.svg" : ":/svg/player/volume.svg"));

        m_volumeSlider->blockSignals(true);
        if (m_isMuted)
        {
            m_volumeSlider->setValue(0);
        }
        else
        {
            m_volumeSlider->setValue(static_cast<int>(m_currentVolume));
        }
        m_volumeSlider->blockSignals(false);
    }
    else if (property == "volume")
    {
        m_currentVolume = value.toDouble();
        if (!m_isMuted)
        { 
            m_volumeSlider->blockSignals(true);
            m_volumeSlider->setValue(static_cast<int>(m_currentVolume));
            m_volumeSlider->blockSignals(false);
        }
    }
    else if (property == "speed")
    {
        m_currentSpeed = value.toDouble();
        if (m_speedBtn)
        {
            m_speedBtn->setText(PlayerLongPressHandler::formatSpeedText(m_currentSpeed));
        }
    }
    else if (property == "paused-for-cache")
    {
        
        m_isBuffering = value.toBool();
        updateLoadingState();
    }
    else if (property == "seeking")
    {
        
        m_isSeeking = value.toBool();
        updateLoadingState();
    }
    else if (property == "eof-reached")
    {
        m_isPlaybackFinished = value.toBool();
        if (m_isPlaybackFinished)
        {
            qDebug() << "[PlayerView] MPV reached EOF"
                     << "| position=" << m_currentPosition
                     << "| duration=" << m_totalDuration;
            m_isPlaying = false;
            m_isBuffering = false;
            m_isSeeking = false;
            updatePowerInhibition();
            updateLoadingState();
            if (m_playPauseBtn)
            {
                m_playPauseBtn->setIcon(QIcon(":/svg/player/play.svg"));
            }
            autoPlayNextMediaIfEnabled();
        }
    }

    if (m_showStatisticsOverlay && (property == QLatin1String("cache-speed") ||
                                    property == QLatin1String("track-list") || property == QLatin1String("pause")))
    {
        updateStatisticsDisplay();
    }
}

void PlayerView::toggleMute()
{
    m_isMuted = !m_isMuted;
    m_mpvWidget->controller()->setProperty("mute", m_isMuted);
    showToast(m_isMuted ? tr("Muted") : tr("Volume: %1%").arg(static_cast<int>(m_currentVolume)));
    showControls();

    
    ConfigStore::instance()->set(ConfigKeys::PlayerVolumeMuted, m_isMuted);
}


void PlayerView::changeVolume(int delta, bool silent)
{
    if (m_isMuted)
    {
        return;
    }

    m_currentVolume += delta;
    if (m_currentVolume < 0)
    {
        m_currentVolume = 0;
    }
    if (m_currentVolume > 100)
    {
        m_currentVolume = 100;
    }

    m_mpvWidget->controller()->setProperty("volume", m_currentVolume);

    if (!silent)
    {
        showToast(tr("Volume: %1%").arg(static_cast<int>(m_currentVolume)));
        showControls();
    }
    else
    {
        if (m_osdLayer && m_topOpacity->opacity() <= 0.0)
        {
            const int volumePercent = static_cast<int>(m_currentVolume);
            m_osdLayer->showVolume(volumePercent, tr("%1%").arg(volumePercent), m_isMuted);
        }
    }

    
    ConfigStore::instance()->set(ConfigKeys::PlayerVolumeLevel, m_currentVolume);
}

void PlayerView::onVolumeSliderMoved(int value)
{
    m_currentVolume = value;
    if (m_isMuted && m_currentVolume > 0)
    {
        m_isMuted = false;
        m_mpvWidget->controller()->setProperty("mute", false);
    }
    m_mpvWidget->controller()->setProperty("volume", m_currentVolume);
    showToast(tr("Volume: %1%").arg(static_cast<int>(m_currentVolume)));
    showControls();

    
    ConfigStore::instance()->set(ConfigKeys::PlayerVolumeLevel, m_currentVolume);
    ConfigStore::instance()->set(ConfigKeys::PlayerVolumeMuted, m_isMuted);
}





void PlayerView::showSettingsMenu()
{
    auto *panel = new ModernScrollPanel(this);
    const bool showDanmakuControls = shouldShowDanmakuHudControls();

    
    panel->addItem(tr("Network Speed"), "network_speed", m_showNetworkSpeed);
    panel->addItem(tr("Statistics"), "statistics", m_showStatisticsOverlay);
    panel->addItem(tr("Subtitle Settings"), "subtitle_settings", false);
    if (showDanmakuControls)
    {
        panel->addItem(tr("Danmaku Settings"), "danmaku_settings", false);
    }
    panel->addItem(tr("Skip Intro / Outro"), "skip_segments", false);

    
    ServerProfile profile = m_core->serverManager()->activeProfile();
    bool defaultStrmDirect = (profile.type == ServerProfile::Jellyfin);
    QSettings settings("ReEmby", "Player");
    bool strmDirect = settings.value("EnableStrmDirectPlay", defaultStrmDirect).toBool();

    panel->addItem(tr("STRM Direct Play"), "strm_direct", strmDirect);

    int maxHeight = this->height() - m_bottomHUD->height() - 40;
    
    panel->finalizeLayout(maxHeight < 150 ? 150 : maxHeight, 300);

    
    connect(panel, &ModernScrollPanel::itemTriggered, this,
            [this](const QVariant &data, const QString &text)
            {
                Q_UNUSED(text);
                QString action = data.toString();
                auto dismissPopup = [this]()
                {
                    if (m_activePopup)
                    {
                        m_activePopup->hide();
                        m_activePopup->close();
                        m_activePopup->deleteLater();
                        m_activePopup = nullptr;
                    }
                };

                if (action == "network_speed")
                {
                    m_showNetworkSpeed = !m_showNetworkSpeed;
                    m_networkSpeedLabel->setVisible(m_showNetworkSpeed);
                    showToast(m_showNetworkSpeed ? tr("Network Speed Enabled") : tr("Network Speed Disabled"));
                }
                else if (action == "statistics")
                {
                    m_showStatisticsOverlay = !m_showStatisticsOverlay;
                    if (m_showStatisticsOverlay)
                    {
                        updateStatisticsDisplay();
                        ensureStatisticsWindow();
                        if (m_statisticsWindow)
                        {
                            m_statisticsWindow->show();
                            // 面板宿主是独立顶层窗口，几何要在它显示后才同步得准。
                            syncStatisticsWindow();
                        }
                        updateOverlayLayout();
                    }
                    else
                    {
                        if (m_statisticsWindow)
                        {
                            m_statisticsWindow->hide();
                        }
                        else if (m_statisticsOverlay)
                        {
                            m_statisticsOverlay->hide();
                        }
                    }
                }
                else if (action == "strm_direct")
                {
                    ServerProfile profile = m_core->serverManager()->activeProfile();
                    bool defaultStrmDirect = (profile.type == ServerProfile::Jellyfin);
                    QSettings settings("ReEmby", "Player");
                    bool currentStrm = settings.value("EnableStrmDirectPlay", defaultStrmDirect).toBool();
                    bool newStrm = !currentStrm;

                    settings.setValue("EnableStrmDirectPlay", newStrm);
                    showToast(newStrm ? tr("STRM Direct Play: ON (Reloading...)")
                                      : tr("STRM Direct Play: OFF (Reloading...)"));

                    
                    QString mediaId = m_currentMediaId;
                    QString title = m_fullTitle;
                    QString origUrl = m_originalStreamUrl;
                    QVariant sourceInfo = m_currentSourceInfoVar;
                    long long currentTicks = static_cast<long long>(m_currentPosition * 10000000.0);

                    
                    stopAndReport();

                    
                    QTimer::singleShot(150, this, [this, mediaId, title, origUrl, currentTicks, sourceInfo]()
                                       { playMedia(mediaId, title, origUrl, currentTicks, sourceInfo); });
                }
                else if (action == "subtitle_settings")
                {
                    dismissPopup();
                    openSubtitleSettingsDialog();
                    return;
                }
                else if (action == "danmaku_settings")
                {
                    dismissPopup();
                    openDanmakuSettingsDialog();
                    return;
                }
                else if (action == "skip_segments")
                {
                    dismissPopup();
                    openSkipSettingsDialog();
                    return;
                }

                
                dismissPopup();
            });

    showCenteredPopup(panel, m_settingsBtn);
}





void PlayerView::showSpeedMenu()
{
    auto *panel = new ModernScrollPanel(this);

    QList<double> speeds = {0.5, 1.0, 1.25, 1.5, 1.75, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};

    for (double s : speeds)
    {
        bool isSelected = qFuzzyCompare(m_currentSpeed, s);
        panel->addItem(QString("%1X").arg(s), s, isSelected);
    }

    int maxHeight = this->height() - m_bottomHUD->height() - 40;
    panel->finalizeLayout(maxHeight < 150 ? 150 : maxHeight, 130);

    connect(panel, &ModernScrollPanel::itemTriggered, this,
            [this](const QVariant &data, const QString &text)
            {
                Q_UNUSED(text);
                double s = data.toDouble();
                m_currentSpeed = s;
                setEffectivePlaybackSpeed(s);
                showToast(tr("Speed: %1X").arg(PlayerLongPressHandler::formatSpeedText(s).chopped(1)));

                if (m_activePopup)
                {
                    m_activePopup->deleteLater();
                    m_activePopup = nullptr;
                }
            });

    showCenteredPopup(panel, m_speedBtn);
}

void PlayerView::showAudioMenu()
{
    auto *panel = new ModernScrollPanel(this);

    auto tracks = m_mpvWidget->controller()->getProperty("track-list").toList();
    bool anyAudioSelected = false;

    for (const QVariant &v : tracks)
    {
        auto map = v.toMap();
        if (map["type"].toString() == "audio")
        {
            int id = map["id"].toInt();
            QString title = map["title"].toString();
            QString lang = map["lang"].toString();
            bool selected = map["selected"].toBool();
            if (selected)
            {
                anyAudioSelected = true;
            }

            QString text = title.isEmpty() ? (lang.isEmpty() ? tr("Track %1").arg(id) : lang) : title;
            panel->addItem(text, id, selected);
        }
    }

    panel->addItem(tr("Disable Audio"), "no", !anyAudioSelected);

    int maxHeight = this->height() - m_bottomHUD->height() - 40;
    panel->finalizeLayout(maxHeight < 150 ? 150 : maxHeight, 200);

    connect(panel, &ModernScrollPanel::itemTriggered, this,
            [this](const QVariant &data, const QString &text)
            {
                QString valStr = data.toString();
                if (valStr == "no")
                {
                    m_mpvWidget->controller()->setProperty("aid", "no");
                    showToast(tr("Audio Disabled"));
                }
                else
                {
                    int id = data.toInt();
                    m_mpvWidget->controller()->setProperty("aid", id);
                    showToast(tr("Audio: %1").arg(text));
                }

                if (m_activePopup)
                {
                    m_activePopup->deleteLater();
                    m_activePopup = nullptr;
                }
            });

    showCenteredPopup(panel, m_audioBtn);
}

void PlayerView::showSubtitleMenu()
{
    auto *panel = new ModernScrollPanel(this);

    // 一级菜单只放"主字幕 / 副字幕"两项（悬停/点击后在右侧展开该通道的轨道
    // 列表），下方保留加载本地字幕与设置入口。轨道列表不平铺在这里——字幕多
    // 的片源（10+ 条）会把一级菜单拉得很长，滚动查找也不便。
    panel->addItem(tr("Primary Subtitle"), QStringLiteral("sub_menu_primary"),
                   false, true);
    if (ConfigStore::instance()->get<bool>(
            ConfigKeys::PlayerSubtitleSecondaryEnabled, false))
    {
        panel->addItem(tr("Secondary Subtitle"),
                       QStringLiteral("sub_menu_secondary"), false, true);
    }
    panel->addItem(tr("Load Local Subtitle File"), QStringLiteral("load_local"), false);
    panel->addItem(tr("Subtitle Settings"), QStringLiteral("settings"), false);

    int maxHeight = this->height() - m_bottomHUD->height() - 40;
    panel->finalizeLayout(maxHeight < 150 ? 150 : maxHeight, 240);

    // 悬停：展开/切换/收起右侧字幕列表（悬停到普通项时收起）。
    connect(panel, &ModernScrollPanel::itemHovered, this,
            [this](const QVariant &data, int anchorY)
            {
                const QString action = data.toString();
                if (action == QLatin1String("sub_menu_primary"))
                {
                    showSubtitleTrackSubmenu(false, anchorY);
                }
                else if (action == QLatin1String("sub_menu_secondary"))
                {
                    showSubtitleTrackSubmenu(true, anchorY);
                }
                else
                {
                    closeSubtitleSubmenu();
                }
            });

    connect(panel, &ModernScrollPanel::itemTriggered, this,
            [this](const QVariant &data, const QString &text)
            {
                Q_UNUSED(text);
                const QString action = data.toString();

                auto dismissMenus = [this]()
                {
                    if (m_activePopup)
                    {
                        m_activePopup->hide();
                        m_activePopup->close();
                        m_activePopup->deleteLater();
                        m_activePopup = nullptr;
                    }
                    closeSubtitleSubmenu();
                };

                if (action == QLatin1String("sub_menu_primary"))
                {
                    // 点击（而非悬停）也展开二级，方便不习惯悬停操作的方式。
                    showSubtitleTrackSubmenu(false, 0);
                    return;
                }
                if (action == QLatin1String("sub_menu_secondary"))
                {
                    showSubtitleTrackSubmenu(true, 0);
                    return;
                }
                if (action == QLatin1String("settings"))
                {
                    dismissMenus();
                    openSubtitleSettingsDialog();
                    return;
                }
                if (action == QLatin1String("load_local"))
                {
                    dismissMenus();
                    loadExternalSubtitleFile();
                    return;
                }
            });

    showCenteredPopup(panel, m_subtitleBtn);
}

// 二级字幕列表：一级菜单里"主字幕/副字幕"悬停或点击后在其右侧弹出该通道的
// 轨道列表。勾选状态主/副各取各的（contentSubtitleTracks(forSecondary)），
// 互相独立。选中"关闭字幕/关闭副字幕"或任一轨道后两级菜单一并关闭。
void PlayerView::showSubtitleTrackSubmenu(bool secondary, int anchorY)
{
    if (!m_danmakuController || !m_activePopup)
    {
        return;
    }

    // 内容未变（同为该通道）时只重新对齐位置，避免悬停来回划动反复重建闪烁。
    if (m_subtitleSubmenu && m_subtitleSubmenuIsSecondary == secondary)
    {
        positionSubtitleSubmenu(anchorY);
        return;
    }

    closeSubtitleSubmenu();

    // 与一级相同的宿主（内嵌 = 本视图；独立窗口 = 透明 HUD 窗口），保证两者
    // 坐标系一致（B3：独立窗口下弹层都在 HUD 窗口内）。
    QWidget *host = m_activePopup->parentWidget() ? m_activePopup->parentWidget()
                                                  : this;
    auto *panel = new ModernScrollPanel(host);

    const QList<QVariantMap> tracks =
        m_danmakuController->contentSubtitleTracks(secondary);
    bool anySelected = false;
    for (const QVariantMap &map : tracks)
    {
        const int id = map["id"].toInt();
        const QString title = map["title"].toString();
        const QString lang = map["lang"].toString();
        const bool selected = map["selected"].toBool();
        if (selected)
        {
            anySelected = true;
        }

        const QString text = title.isEmpty()
                                 ? (lang.isEmpty()
                                        ? tr("Subtitle %1").arg(id)
                                        : lang)
                                 : title;
        panel->addItem(text, id, selected);
    }
    panel->addItem(secondary ? tr("Disable Secondary Subtitle")
                             : tr("Disable Subtitles"),
                   QStringLiteral("no"), !anySelected);

    int maxHeight = this->height() - m_bottomHUD->height() - 40;
    panel->finalizeLayout(maxHeight < 150 ? 150 : maxHeight, 280);

    connect(panel, &ModernScrollPanel::itemTriggered, this,
            [this, secondary](const QVariant &data, const QString &text)
            {
                if (m_activePopup)
                {
                    m_activePopup->hide();
                    m_activePopup->close();
                    m_activePopup->deleteLater();
                    m_activePopup = nullptr;
                }
                closeSubtitleSubmenu();

                if (!m_danmakuController)
                {
                    return;
                }

                if (secondary)
                {
                    m_danmakuController->selectSecondarySubtitleTrack(data);
                    const bool disabled = data.toString() == QLatin1String("no");
                    showToast(disabled ? tr("Secondary Subtitle Disabled")
                                       : tr("Secondary Subtitle: %1").arg(text));
                    // ass-track 弹幕占用 sid、secondary-sid 又要留给主字幕 ——
                    // 两条字幕轨已满，副字幕无法显示；给出明确提示。
                    if (!disabled &&
                        m_danmakuController->secondarySubtitleBlockedByDanmaku())
                    {
                        showToast(tr("Secondary subtitle requires native danmaku rendering"));
                    }
                }
                else
                {
                    clearPersistedExternalSubtitle();
                    m_danmakuController->selectSubtitleTrack(data);
                    showToast(data.toString() == QLatin1String("no")
                                  ? tr("Subtitles Disabled")
                                  : tr("Subtitle: %1").arg(text));
                }
            });

    m_subtitleSubmenu = panel;
    m_subtitleSubmenuIsSecondary = secondary;
    positionSubtitleSubmenu(anchorY);
    promoteStandaloneLayer(panel);
    panel->show();
    panel->raise();
}

// 把二级面板对齐到一级面板右侧（悬停项处），并夹取在宿主范围内；右侧放不
// 下时改放左侧。
void PlayerView::positionSubtitleSubmenu(int anchorY)
{
    if (!m_subtitleSubmenu || !m_activePopup)
    {
        return;
    }

    QWidget *host = m_activePopup->parentWidget() ? m_activePopup->parentWidget()
                                                  : this;
    const QPoint mainTopLeft = m_activePopup->pos();
    const int subW = m_subtitleSubmenu->width();
    const int subH = m_subtitleSubmenu->height();
    int x = mainTopLeft.x() + m_activePopup->width() + 4;
    int y = mainTopLeft.y() + anchorY;

    const int hostW = host->width();
    const int hostH = host->height();
    if (x + subW > hostW - 8)
    {
        x = qMax(8, mainTopLeft.x() - subW - 4);
    }
    if (y + subH > hostH - 8)
    {
        y = qMax(8, hostH - 8 - subH);
    }
    m_subtitleSubmenu->move(x, y);
}

void PlayerView::closeSubtitleSubmenu()
{
    if (m_subtitleSubmenu)
    {
        m_subtitleSubmenu->hide();
        m_subtitleSubmenu->deleteLater();
        m_subtitleSubmenu = nullptr;
    }
}

void PlayerView::showDanmakuMenu()
{
    if (!shouldShowDanmakuHudControls())
    {
        return;
    }

    auto *panel = new ModernScrollPanel(this);

    const bool visible = m_danmakuController && m_danmakuController->isDanmakuVisible();
    panel->addItem(buildDanmakuSummaryText(), QStringLiteral("info"), false);
    panel->addItem(visible ? tr("Hide Danmaku") : tr("Show Danmaku"), QStringLiteral("toggle"), visible);
    panel->addItem(tr("Reload Danmaku"), QStringLiteral("reload"), false);
    panel->addItem(tr("Load Local Danmaku File"), QStringLiteral("load_local"), false);
    panel->addItem(tr("Search Danmaku"), QStringLiteral("search"), false);
    panel->addItem(tr("Danmaku Settings"), QStringLiteral("quick_settings"), false);

    int maxHeight = this->height() - m_bottomHUD->height() - 40;
    panel->finalizeLayout(maxHeight < 160 ? 160 : maxHeight, 360);

    connect(panel, &ModernScrollPanel::itemTriggered, this,
            [this](const QVariant &data, const QString &text)
            {
                Q_UNUSED(text);
                if (!m_danmakuController)
                {
                    return;
                }

                auto dismissPopup = [this]()
                {
                    if (m_activePopup)
                    {
                        m_activePopup->hide();
                        m_activePopup->close();
                        m_activePopup->deleteLater();
                        m_activePopup = nullptr;
                    }
                };

                const QString action = data.toString();
                if (action == QLatin1String("info"))
                {
                    showToast(buildDanmakuSummaryText());
                }
                else if (action == QLatin1String("toggle"))
                {
                    m_danmakuController->setDanmakuVisible(!m_danmakuController->isDanmakuVisible());
                }
                else if (action == QLatin1String("reload"))
                {
                    m_danmakuController->reload();
                }
                else if (action == QLatin1String("load_local"))
                {
                    dismissPopup();
                    loadLocalDanmakuFile();
                    return;
                }
                else if (action == QLatin1String("search"))
                {
                    dismissPopup();
                    showDanmakuIdentifyDialog();
                    return;
                }
                else if (action == QLatin1String("quick_settings"))
                {
                    dismissPopup();
                    openDanmakuSettingsDialog();
                    return;
                }

                dismissPopup();
            });

    showCenteredPopup(panel, m_danmakuBtn);
}

void PlayerView::showDanmakuIdentifyDialog()
{
    if (!m_danmakuController || !m_danmakuController->hasPlaybackContext())
    {
        showToast(tr("Danmaku is unavailable for the current media"));
        return;
    }

    if (m_activePopup)
    {
        m_activePopup->hide();
        m_activePopup->close();
        m_activePopup->deleteLater();
        m_activePopup = nullptr;
    }

    const QString activeTargetId = m_danmakuController ? m_danmakuController->activeTargetId() : QString();
    const QString activeEndpointId = m_danmakuController ? m_danmakuController->activeEndpointId() : QString();

    // Rollback switch: "classic" keeps the old flat PlayerDanmakuIdentifyDialog
    // (one row per episode, right-hand details panel). "series" (default) uses
    // the same aggregated two-stage picker as the detail page — series rows
    // (per provider/season) then a single-episode picker. If the new flow
    // misbehaves, flip danmaku/player_search_ui back to "classic" in config.
    const QString playerSearchUi = ConfigStore::instance()->get<QString>(
        QString::fromLatin1(ConfigKeys::DanmakuPlayerSearchUi),
        QString::fromLatin1("series"));
    if (playerSearchUi == QLatin1String("classic"))
    {
        auto *dialog = new PlayerDanmakuIdentifyDialog(m_core, m_danmakuController->mediaContext(), QString(),
                                                       activeTargetId, activeEndpointId, this);
        connect(dialog, &PlayerDanmakuIdentifyDialog::finished, this,
                [this, dialog](int result)
                {
                    if (result != PlayerOverlayDialog::Accepted || !dialog->selectedCandidate().isValid() ||
                        !m_danmakuController)
                    {
                        return;
                    }

                    m_danmakuController->loadFromCandidate(dialog->selectedCandidate(), true);
                });
        trackPlayerDialog(dialog);
        return;
    }

    auto *dialog = new SeriesDanmakuMatchDialog(
        m_core, SeriesDanmakuMatchDialog::Mode::Single, {},
        m_danmakuController->mediaContext(), QString(), activeTargetId,
        activeEndpointId, QList<int>(), this);
    connect(dialog, &SeriesDanmakuMatchDialog::finished, this,
            [this, dialog](int result)
            {
                if (result != PlayerOverlayDialog::Accepted || !m_danmakuController)
                {
                    return;
                }
                const QList<DanmakuMatchCandidate> picked = dialog->selectedEpisodes();
                if (picked.isEmpty() || !picked.constFirst().isValid())
                {
                    return;
                }
                m_danmakuController->loadFromCandidate(picked.constFirst(), true);
            });
    trackPlayerDialog(dialog);
}

void PlayerView::loadLocalDanmakuFile()
{
    if (!m_danmakuController || !m_danmakuController->hasPlaybackContext())
    {
        showToast(tr("Danmaku is unavailable for the current media"));
        return;
    }

    if (m_activePopup)
    {
        m_activePopup->hide();
        m_activePopup->close();
        m_activePopup->deleteLater();
        m_activePopup = nullptr;
    }
    closeActivePlayerDialog();

    const QString serverId = m_danmakuController->mediaContext().serverId;
    const QString startDir = DanmakuService::ensureLocalDanmakuDirectory(serverId)
                                 ? DanmakuService::localDanmakuDirectoryPath(serverId)
                                 : QDir::homePath();
    const QString filePath = openPlayerFileDialog(
        tr("Load Local Danmaku File"), startDir,
        tr("Danmaku Files (*.ass *.json *.xml);;ASS Files (*.ass);;JSON Files (*.json);;XML Files (*.xml)"));
    if (filePath.trimmed().isEmpty())
    {
        return;
    }

    qDebug().noquote() << "[Danmaku][PlayerView] Load local danmaku file"
                       << "| mediaId:" << m_danmakuController->mediaContext().mediaId << "| path:" << filePath;
    m_danmakuController->loadLocalFile(filePath);
}

void PlayerView::loadExternalSubtitleFile()
{
    if (!m_mpvWidget || !m_mpvWidget->controller())
    {
        showToast(tr("Player is not ready"));
        return;
    }

    if (m_activePopup)
    {
        m_activePopup->hide();
        m_activePopup->close();
        m_activePopup->deleteLater();
        m_activePopup = nullptr;
    }
    closeActivePlayerDialog();

    const QString lastDir = ConfigStore::instance()->get<QString>(ConfigKeys::PlayerLastSubtitleDir, QString());
    const QString startDir = (!lastDir.isEmpty() && QDir(lastDir).exists()) ? lastDir : QDir::homePath();

    const QString filter = tr("Subtitle Files (*.srt *.ass *.ssa *.sub *.idx *.vtt *.smi *.sup *.txt);;"
                              "SubRip (*.srt);;"
                              "Advanced SubStation Alpha (*.ass *.ssa);;"
                              "WebVTT (*.vtt);;"
                              "MicroDVD / SubViewer (*.sub);;"
                              "VobSub (*.idx);;"
                              "SAMI (*.smi);;"
                              "PGS / HDMV (*.sup);;"
                              "All Files (*)");

    const QString filePath = openPlayerFileDialog(tr("Load Local Subtitle File"), startDir, filter);
    if (filePath.trimmed().isEmpty())
    {
        return;
    }

    const QFileInfo info(filePath);
    ConfigStore::instance()->set(ConfigKeys::PlayerLastSubtitleDir, info.absolutePath());

    qDebug().noquote() << "[Subtitle][PlayerView] Load external subtitle file"
                       << "| path:" << filePath;

    
    
    
    m_mpvWidget->controller()->command(QVariantList{QStringLiteral("sub-add"), QDir::toNativeSeparators(filePath),
                                                    QStringLiteral("auto"), info.fileName(), QString()});

    
    const int newTrackId = findSubtitleTrackIdByPath(info.absoluteFilePath());
    if (newTrackId > 0 && m_danmakuController)
    {
        m_danmakuController->selectSubtitleTrack(newTrackId);
    }
    else
    {
        qWarning().noquote() << "[Subtitle][PlayerView] Failed to locate newly added subtitle track"
                             << "| path:" << filePath;
    }

    persistExternalSubtitle(info.absoluteFilePath());

    showToast(tr("Subtitle Loaded: %1").arg(info.fileName()));
}

QString PlayerView::externalSubtitleConfigKey() const
{
    if (!m_core)
    {
        return QString();
    }
    const QString serverId = m_core->serverManager()->activeProfile().id;
    if (serverId.isEmpty() || m_currentMediaId.isEmpty())
    {
        return QString();
    }
    return ConfigKeys::forServerMedia(serverId, m_currentMediaId, ConfigKeys::PlayerExternalSubtitle);
}

QString PlayerView::readPersistedExternalSubtitle() const
{
    const QString key = externalSubtitleConfigKey();
    if (key.isEmpty())
    {
        return QString();
    }
    return ConfigStore::instance()->get<QString>(key, QString()).trimmed();
}

void PlayerView::persistExternalSubtitle(const QString &absPath)
{
    const QString key = externalSubtitleConfigKey();
    if (key.isEmpty())
    {
        return;
    }
    ConfigStore::instance()->set(key, absPath);
    qDebug().noquote() << "[Subtitle][PlayerView] Persist external subtitle"
                       << "| key:" << key << "| path:" << absPath;
}

void PlayerView::clearPersistedExternalSubtitle()
{
    const QString key = externalSubtitleConfigKey();
    if (key.isEmpty())
    {
        return;
    }
    const QString existing = ConfigStore::instance()->get<QString>(key, QString()).trimmed();
    if (existing.isEmpty())
    {
        return;
    }
    ConfigStore::instance()->set(key, QString());
    qDebug().noquote() << "[Subtitle][PlayerView] Clear external subtitle record"
                       << "| key:" << key << "| previousPath:" << existing;
}

void PlayerView::applyPersistedExternalSubtitleIfAny()
{
    if (!m_mpvWidget || !m_mpvWidget->controller())
    {
        return;
    }

    const QString persistedPath = readPersistedExternalSubtitle();
    if (persistedPath.isEmpty())
    {
        return;
    }

    const QFileInfo info(persistedPath);
    if (!info.exists() || !info.isFile() || !info.isReadable())
    {
        qDebug().noquote() << "[Subtitle][PlayerView] Persisted external subtitle missing, "
                              "fallback to default selection"
                           << "| path:" << persistedPath;
        clearPersistedExternalSubtitle();
        return;
    }

    qDebug().noquote() << "[Subtitle][PlayerView] Restore persisted external subtitle"
                       << "| path:" << persistedPath;

    
    
    m_mpvWidget->controller()->command(QVariantList{QStringLiteral("sub-add"),
                                                    QDir::toNativeSeparators(info.absoluteFilePath()),
                                                    QStringLiteral("auto"), info.fileName(), QString()});

    const int newTrackId = findSubtitleTrackIdByPath(info.absoluteFilePath());
    if (newTrackId > 0 && m_danmakuController)
    {
        m_danmakuController->selectSubtitleTrack(newTrackId);
    }
    else
    {
        qWarning().noquote() << "[Subtitle][PlayerView] Failed to locate restored subtitle track"
                             << "| path:" << persistedPath;
    }
}

QString PlayerView::openPlayerFileDialog(const QString &title, const QString &startDir, const QString &filter)
{
    QFileDialog dialog(this);
#ifdef Q_OS_LINUX
    dialog.setOption(QFileDialog::DontUseNativeDialog, true);
    dialog.setOption(QFileDialog::DontUseCustomDirectoryIcons, true);
#endif
    dialog.setWindowTitle(title);
    dialog.setDirectory(startDir);
    dialog.setNameFilter(filter);
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setAcceptMode(QFileDialog::AcceptOpen);

    QElapsedTimer timer;
    timer.start();
    qDebug().noquote() << "[PlayerView] Open local file dialog"
                       << "| title:" << title << "| startDir:" << startDir;

    if (dialog.exec() != QDialog::Accepted)
    {
        qDebug().noquote() << "[PlayerView] Local file dialog canceled"
                           << "| title:" << title << "| elapsedMs:" << timer.elapsed();
        return QString();
    }

    const QStringList selectedFiles = dialog.selectedFiles();
    const QString filePath = selectedFiles.isEmpty() ? QString() : selectedFiles.first();
    qDebug().noquote() << "[PlayerView] Local file dialog accepted"
                       << "| title:" << title << "| elapsedMs:" << timer.elapsed() << "| path:" << filePath;
    return filePath;
}

int PlayerView::findSubtitleTrackIdByPath(const QString &absPath) const
{
    if (!m_mpvWidget || !m_mpvWidget->controller() || absPath.isEmpty())
    {
        return -1;
    }
    const QString normalizedTarget = QDir::fromNativeSeparators(absPath);
    const QVariantList tracks = m_mpvWidget->controller()->getProperty(QStringLiteral("track-list")).toList();
    for (const QVariant &v : tracks)
    {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("type")).toString() != QLatin1String("sub"))
        {
            continue;
        }
        const QString external = m.value(QStringLiteral("external-filename")).toString();
        if (!external.isEmpty() && QDir::fromNativeSeparators(external) == normalizedTarget)
        {
            return m.value(QStringLiteral("id")).toInt();
        }
    }
    return -1;
}

void PlayerView::openSubtitleSettingsDialog()
{
    if (m_activePopup)
    {
        m_activePopup->hide();
        m_activePopup->close();
        m_activePopup->deleteLater();
        m_activePopup = nullptr;
    }

    auto *dialog = new PlayerSubtitleSettingsDialog(this);
    trackPlayerDialog(dialog);
}

void PlayerView::openSkipSettingsDialog()
{
    if (m_activePopup)
    {
        m_activePopup->hide();
        m_activePopup->close();
        m_activePopup->deleteLater();
        m_activePopup = nullptr;
    }

    auto *dialog = new SkipSettingsDialog(
        m_seriesId, m_currentMediaId, m_seriesName,
        [this]() { return m_currentPosition; },
        [this]() { return m_totalDuration; },
        this);
    connect(dialog, &SkipSettingsDialog::settingsSaved, this,
            [this]() { refreshManualSkipSettings(); });
    trackPlayerDialog(dialog);
}

void PlayerView::openDanmakuSettingsDialog()
{
    if (!shouldShowDanmakuHudControls())
    {
        return;
    }

    if (m_activePopup)
    {
        m_activePopup->hide();
        m_activePopup->close();
        m_activePopup->deleteLater();
        m_activePopup = nullptr;
    }

    auto *dialog = new PlayerDanmakuSettingsDialog(this);
    connect(dialog, &PlayerDanmakuSettingsDialog::liveReloadRequested, this,
            [this]()
            {
                if (!m_danmakuController || !m_danmakuController->hasPlaybackContext() ||
                    !m_danmakuController->isDanmakuEnabled())
                {
                    return;
                }
                m_danmakuController->reload();
            });
    connect(dialog, &PlayerDanmakuSettingsDialog::finished, this,
            [this, dialog](int)
            {
                if (!m_danmakuController)
                {
                    return;
                }

                if (dialog->requiresReload() && m_danmakuController->hasPlaybackContext() &&
                    m_danmakuController->isDanmakuEnabled())
                {
                    m_danmakuController->reload();
                }
            });
    trackPlayerDialog(dialog);
}

void PlayerView::applySubtitleStyleSettings()
{
    if (!m_mpvWidget || !m_mpvWidget->controller())
    {
        return;
    }

    const bool protectDanmakuPrimary =
        m_danmakuController && m_danmakuController->isDanmakuEnabled() && m_danmakuController->hasDanmakuTrack();
    SubtitleStyleUtils::applyToController(m_mpvWidget->controller(), protectDanmakuPrimary);

    // 副字幕开关/参数变化时同步轨道分配（幂等）：开启/关闭副字幕、修改副字幕
    // 参数都能立即生效，无需重载媒体。
    if (m_danmakuController)
    {
        m_danmakuController->refreshTrackSelection();
    }
}

void PlayerView::resumePlaybackAfterFinishedSeek()
{
    if (!m_isPlaybackFinished || !m_mpvWidget)
    {
        return;
    }

    qDebug() << "[PlayerView] Resume playback after finished seek"
             << "| position=" << m_currentPosition
             << "| duration=" << m_totalDuration;

    m_isPlaybackFinished = false;
    m_isPlaying = true;
    m_isBuffering = false;
    m_isSeeking = false;
    m_mpvWidget->play();
    updatePowerInhibition();
    updateLoadingState();
    if (m_playPauseBtn)
    {
        m_playPauseBtn->setIcon(QIcon(":/svg/player/pause.svg"));
    }
}



void PlayerView::seekRelative(double delta, bool silent)
{
    const bool shouldResumeFinishedPlayback = m_isPlaybackFinished && delta < 0.0;
    m_mpvWidget->controller()->command(QVariantList{"seek", delta, "relative"});
    if (shouldResumeFinishedPlayback)
    {
        resumePlaybackAfterFinishedSeek();
    }

    if (!silent)
    {
        m_osdSeekPreviewPosition = -1.0;
        showToast(delta > 0 ? tr("Forward %1s").arg(std::abs(delta)) : tr("Rewind %1s").arg(std::abs(delta)));
        showControls();
    }
    else
    {
        if (m_osdLayer && m_topOpacity && m_topOpacity->opacity() <= 0.0)
        {
            const bool continuePreview = m_osdLayer->isSeekLineVisible() && m_osdSeekPreviewPosition >= 0.0;
            const double basePosition = continuePreview ? m_osdSeekPreviewPosition : m_currentPosition;
            double previewPosition = basePosition + delta;
            if (m_totalDuration > 0.0)
            {
                previewPosition = qBound(0.0, previewPosition, m_totalDuration);
            }
            else
            {
                previewPosition = qMax(0.0, previewPosition);
            }

            m_osdSeekPreviewPosition = previewPosition;
            m_osdLayer->showSeek(previewPosition, m_totalDuration, formatTime(previewPosition, m_totalDuration));
        }
    }
}

void PlayerView::cycleVideoScale()
{
    m_videoScaleMode = (m_videoScaleMode + 1) % 4;
    if (m_nativeDanmakuOverlay) {
        m_nativeDanmakuOverlay->setVideoScaleMode(m_videoScaleMode);
    }
    auto *ctrl = m_mpvWidget->controller();
    QString modeStr;

    switch (m_videoScaleMode)
    {
    case 0:
        ctrl->setProperty("keepaspect", true);
        ctrl->setProperty("panscan", 0.0);
        ctrl->setProperty("video-unscaled", false);
        modeStr = tr("Scale: Fit");
        break;
    case 1:
        ctrl->setProperty("keepaspect", true);
        ctrl->setProperty("panscan", 1.0);
        ctrl->setProperty("video-unscaled", false);
        modeStr = tr("Scale: Crop");
        break;
    case 2:
        ctrl->setProperty("keepaspect", false);
        ctrl->setProperty("panscan", 0.0);
        ctrl->setProperty("video-unscaled", false);
        modeStr = tr("Scale: Stretch");
        break;
    case 3:
        ctrl->setProperty("keepaspect", true);
        ctrl->setProperty("panscan", 0.0);
        ctrl->setProperty("video-unscaled", true);
        modeStr = tr("Scale: Original");
        break;
    }
    setScaleIcon();
    showToast(modeStr);

    
    ConfigStore::instance()->set(ConfigKeys::PlayerDefaultScale, m_videoScaleMode);
}

void PlayerView::setScaleIcon()
{
    switch (m_videoScaleMode)
    {
    case 0:
        m_scaleBtn->setIcon(QIcon(":/svg/player/scale-fit.svg"));
        break;
    case 1:
        m_scaleBtn->setIcon(QIcon(":/svg/player/scale-crop.svg"));
        break;
    case 2:
        m_scaleBtn->setIcon(QIcon(":/svg/player/scale-stretch.svg"));
        break;
    case 3:
        m_scaleBtn->setIcon(QIcon(":/svg/player/scale-original.svg"));
        break;
    }
}

void PlayerView::showToast(const QString &msg)
{
    if (m_isViewTearingDown)
    {
        return;
    }

    m_toastLabel->setText(msg);
    m_toastLabel->show();
    m_toastTimer->start(2500);
}

void PlayerView::updateStatisticsDisplay()
{
    if (!m_statisticsOverlay || !m_mpvWidget || !m_mpvWidget->controller())
    {
        return;
    }

    auto *ctrl = m_mpvWidget->controller();
    const QVariantList tracks = ctrl->getProperty("track-list").toList();
    const QVariantMap selectedVideoTrack = findSelectedTrackByType(tracks, QStringLiteral("video"));
    const QVariantMap selectedAudioTrack = findSelectedTrackByType(tracks, QStringLiteral("audio"));
    const QVariantMap selectedSubtitleTrack = findSelectedTrackByType(tracks, QStringLiteral("sub"));

    const MediaSourceInfo &source = m_currentMediaSourceInfo;
    const MediaStreamInfo *videoStream = findFirstStreamByType(source, QStringLiteral("Video"));
    const MediaStreamInfo *audioStream = findFirstStreamByType(source, QStringLiteral("Audio"));

    auto addLine = [](QStringList &lines, const QString &label, const QString &value)
    { lines << (label + QStringLiteral(": ") + trimOrDash(value)); };

    QStringList lines;

    QString sourcePath = firstNonEmpty({source.path, m_currentMediaItem.path});
    sourcePath = QUrl::fromPercentEncoding(sourcePath.toUtf8()).trimmed();

    QString fileDisplay = sourcePath;
    if (!fileDisplay.isEmpty())
    {
        const QFileInfo fileInfo(fileDisplay);
        if (!fileInfo.fileName().isEmpty())
        {
            fileDisplay = fileInfo.fileName();
        }
    }
    if (fileDisplay.isEmpty())
    {
        fileDisplay = firstNonEmpty({source.name, m_currentMediaItem.name, m_fullTitle});
    }
    addLine(lines, tr("File"), fileDisplay);

    QStringList sessionParts;
    if (!m_currentMediaId.isEmpty())
    {
        sessionParts << tr("Item ID") + QStringLiteral("=") + m_currentMediaId;
    }
    if (!m_currentMediaSourceId.isEmpty())
    {
        sessionParts << tr("Media Source ID") + QStringLiteral("=") + m_currentMediaSourceId;
    }
    if (!m_currentPlaySessionId.isEmpty())
    {
        sessionParts << tr("Play Session ID") + QStringLiteral("=") + m_currentPlaySessionId;
    }
    if (!sessionParts.isEmpty())
    {
        addLine(lines, tr("Session"), sessionParts.join(QStringLiteral("  ")));
    }

    const QVariantList chapterList = ctrl->getProperty("chapter-list").toList();
    const int chapterIndex = ctrl->getProperty("chapter").toInt();
    if (!chapterList.isEmpty() && chapterIndex >= 0 && chapterIndex < chapterList.size())
    {
        const QVariantMap chapterInfo = chapterList.at(chapterIndex).toMap();
        QString chapterTitle = chapterInfo.value(QStringLiteral("title")).toString().trimmed();
        if (chapterTitle.isEmpty())
        {
            chapterTitle = tr("Chapter") + QStringLiteral(" ") + QString::number(chapterIndex + 1);
        }
        chapterTitle += QStringLiteral(" (%1 / %2)").arg(chapterIndex + 1).arg(chapterList.size());
        addLine(lines, tr("Chapter"), chapterTitle);
    }

    const qint64 fileSize = source.size > 0 ? source.size : m_currentMediaItem.size;
    const QString container = firstNonEmpty({source.container.toUpper(), m_currentMediaItem.container.toUpper()});
    addLine(
        lines, tr("Size"),
        joinNonEmpty({fileSize > 0 ? FileUtils::formatSize(fileSize) : QString(), container}, QStringLiteral("  ")));

    const QVariantMap cacheState = ctrl->getProperty("demuxer-cache-state").toMap();
    // fw-bytes = 从当前解码位置起在内存里缓冲的字节数（粗略估计）。
    // 注意不要把它和 file-cache-bytes 取 max 混在一起显示：两者量纲不同
    // （一个是在内存里的包，一个是磁盘文件里的字节），混在一起会掩盖
    // "磁盘缓存到底有没有在工作"这个用户最关心的问题。
    const qint64 cacheBytes = variantToLongLong(cacheState.value(QStringLiteral("fw-bytes")));
    double cacheDuration = ctrl->getProperty("demuxer-cache-duration").toDouble();
    if (cacheDuration <= 0.0)
    {
        cacheDuration = variantToDouble(cacheState.value(QStringLiteral("cache-duration")));
    }
    const qint64 cacheSpeed =
        m_useRelayNetworkSpeed ? m_effectiveNetworkSpeed : ctrl->getProperty("cache-speed").toLongLong();
    addLine(lines, tr("Cache"),
            joinNonEmpty({cacheBytes > 0 ? FileUtils::formatSize(cacheBytes) : QString(),
                          formatDurationValue(cacheDuration), formatDataRateValue(cacheSpeed)},
                         QStringLiteral("  ")));

    // 磁盘缓存单独一行。mpv 的 demuxer-cache-state 只有在 --cache-on-disk=yes
    // 时才会给出 file-cache-bytes 这个成员（见 DOCS/man/input.rst），所以
    // "字段在不在" 本身就是开关是否生效的判据 —— 这也是唯一能从外部观察到
    // 磁盘缓存的方式，因为 mpv 建完临时文件后立刻 unlink 了它
    // （demux_cache_create()，demuxer-cache-unlink-files 默认 immediate），
    // 缓存目录在资源管理器里永远是空的。
    if (cacheState.contains(QStringLiteral("file-cache-bytes")))
    {
        const qint64 diskCacheBytes = variantToLongLong(cacheState.value(QStringLiteral("file-cache-bytes")));
        addLine(lines, tr("Disk Cache"),
                diskCacheBytes > 0 ? FileUtils::formatSize(diskCacheBytes) : tr("Not writing"));
    }

    const QString voName = ctrl->getProperty("current-vo").toString().trimmed();
    QString decoderName = ctrl->getProperty("hwdec-current").toString().trimmed();
    if (decoderName.isEmpty() || decoderName.compare(QStringLiteral("no"), Qt::CaseInsensitive) == 0)
    {
        decoderName = tr("Software (CPU)");
    }
    else
    {
        decoderName = decoderName.toUpper();
    }

    QStringList playbackParts;
    if (!voName.isEmpty())
    {
        playbackParts << voName;
    }
    if (!decoderName.isEmpty())
    {
        playbackParts << decoderName;
    }
    playbackParts << tr("Speed") + QStringLiteral(" ") + stripTrailingZeros(QString::number(m_currentSpeed, 'f', 2)) +
                         QStringLiteral("X");

    const double avsync = ctrl->getProperty("avsync").toDouble();
    const QString avsyncText = formatAvSyncValue(avsync);
    if (!avsyncText.isEmpty())
    {
        playbackParts << tr("A-V") + QStringLiteral(" ") + avsyncText;
    }
    addLine(lines, tr("Playback"), playbackParts.join(QStringLiteral("  ")));

    double displayFps = ctrl->getProperty("display-fps").toDouble();
    if (displayFps <= 0.0)
    {
        displayFps = ctrl->getProperty("estimated-display-fps").toDouble();
    }
    double videoFps = ctrl->getProperty("container-fps").toDouble();
    if (videoFps <= 0.0)
    {
        videoFps = selectedVideoTrack.value(QStringLiteral("demux-fps")).toDouble();
    }
    if (videoFps <= 0.0 && videoStream)
    {
        videoFps = videoStream->realFrameRate;
    }

    QStringList refreshParts;
    if (displayFps > 0.0)
    {
        refreshParts << QStringLiteral("%1 Hz").arg(stripTrailingZeros(QString::number(displayFps, 'f', 2)));
    }
    const QString frameRateText = formatFrameRateValue(videoFps);
    if (!frameRateText.isEmpty())
    {
        refreshParts << frameRateText;
    }
    addLine(lines, tr("Refresh Rate"), refreshParts.join(QStringLiteral("  ")));

    const QString outputDrops = ctrl->getProperty("frame-drop-count").toString().trimmed();
    const QString decoderDrops = ctrl->getProperty("decoder-frame-drop-count").toString().trimmed();
    QStringList dropParts;
    if (!outputDrops.isEmpty())
    {
        dropParts << tr("Output") + QStringLiteral(" ") + outputDrops;
    }
    if (!decoderDrops.isEmpty())
    {
        dropParts << tr("Decoder") + QStringLiteral(" ") + decoderDrops;
    }
    addLine(lines, tr("Dropped Frames"), dropParts.join(QStringLiteral("  ")));

    lines << QString();

    const QString videoCodec = firstNonEmpty({ctrl->getProperty("video-codec").toString().toUpper(),
                                              selectedVideoTrack.value(QStringLiteral("codec")).toString().toUpper(),
                                              videoStream ? videoStream->codec.toUpper() : QString()});
    const QString videoFormat = ctrl->getProperty("video-format").toString().toUpper().trimmed();
    const QString videoProfile = videoStream ? videoStream->profile.trimmed() : QString();
    const QString videoLevel = videoStream ? formatLevelValue(videoStream->level) : QString();

    QStringList videoCodecParts;
    if (!videoCodec.isEmpty())
    {
        videoCodecParts << videoCodec;
    }
    if (!videoFormat.isEmpty())
    {
        videoCodecParts << videoFormat;
    }
    if (!videoProfile.isEmpty())
    {
        videoCodecParts << videoProfile;
    }
    if (!videoLevel.isEmpty())
    {
        videoCodecParts << QStringLiteral("L%1").arg(videoLevel);
    }
    addLine(lines, tr("Video"), videoCodecParts.join(QStringLiteral(" / ")));

    addLine(lines, tr("Frame Rate"), frameRateText);

    int sourceWidth = ctrl->getProperty("width").toInt();
    int sourceHeight = ctrl->getProperty("height").toInt();
    if ((sourceWidth <= 0 || sourceHeight <= 0) && videoStream)
    {
        sourceWidth = videoStream->width;
        sourceHeight = videoStream->height;
    }
    const double displayWidthRaw = ctrl->getProperty("video-params/dw").toDouble();
    const double displayHeightRaw = ctrl->getProperty("video-params/dh").toDouble();
    const int displayWidth = displayWidthRaw > 0.0 ? qRound(displayWidthRaw) : sourceWidth;
    const int displayHeight = displayHeightRaw > 0.0 ? qRound(displayHeightRaw) : sourceHeight;

    QString resolutionText = formatDimensionValue(sourceWidth, sourceHeight);
    const QString displayDimensions = formatDimensionValue(displayWidth, displayHeight);
    if (resolutionText.isEmpty())
    {
        resolutionText = displayDimensions;
    }
    else if (!displayDimensions.isEmpty() && displayDimensions != resolutionText)
    {
        resolutionText += QStringLiteral(" -> ") + displayDimensions;
    }
    const QString aspectText =
        formatAspectValue(displayWidth, displayHeight, videoStream ? videoStream->aspectRatio : QString());
    if (!aspectText.isEmpty())
    {
        resolutionText += QStringLiteral(" (%1)").arg(aspectText);
    }
    addLine(lines, tr("Resolution"), resolutionText);

    const QString pixelFormat = firstNonEmpty(
        {ctrl->getProperty("video-params/pixelformat").toString(), videoStream ? videoStream->pixelFormat : QString()});
    const QString colorLevels = ctrl->getProperty("video-params/colorlevels").toString().trimmed();
    const int bitDepth = videoStream ? videoStream->bitDepth : 0;
    addLine(lines, tr("Format"),
            joinNonEmpty({pixelFormat, colorLevels, bitDepth > 0 ? tr("%1-bit").arg(bitDepth) : QString()},
                         QStringLiteral("  ")));

    const QString colorMatrix = ctrl->getProperty("video-params/colormatrix").toString().trimmed();
    const QString colorPrimaries = ctrl->getProperty("video-params/primaries").toString().trimmed();
    const QString colorTransfer = ctrl->getProperty("video-params/gamma").toString().trimmed();
    addLine(lines, tr("Color"), joinNonEmpty({colorMatrix, colorPrimaries, colorTransfer}, QStringLiteral("  ")));

    qint64 videoBitrate = ctrl->getProperty("video-bitrate").toLongLong();
    if (videoBitrate <= 0 && videoStream)
    {
        videoBitrate = videoStream->bitRate;
    }
    if (videoBitrate <= 0)
    {
        videoBitrate = m_currentMediaItem.bitrate;
    }
    addLine(lines, tr("Bitrate"), formatBitrateValue(videoBitrate));

    lines << QString();

    const QString audioCodec = firstNonEmpty({ctrl->getProperty("audio-codec").toString().toUpper(),
                                              selectedAudioTrack.value(QStringLiteral("codec")).toString().toUpper(),
                                              audioStream ? audioStream->codec.toUpper() : QString()});
    const QString currentAo = ctrl->getProperty("current-ao").toString().trimmed();
    addLine(lines, tr("Audio"),
            joinNonEmpty({audioCodec, !currentAo.isEmpty() ? tr("AO %1").arg(currentAo) : QString()},
                         QStringLiteral("  ")));

    addLine(lines, tr("Device"), ctrl->getProperty("audio-device").toString());

    const int channelCount =
        qMax(ctrl->getProperty("audio-params/channel-count").toInt(), audioStream ? audioStream->channels : 0);
    const QString channelLayout = firstNonEmpty({ctrl->getProperty("audio-params/channels").toString(),
                                                 selectedAudioTrack.value(QStringLiteral("demux-channels")).toString(),
                                                 audioStream ? audioStream->channelLayout : QString()});
    addLine(lines, tr("Channels"),
            joinNonEmpty({channelCount > 0 ? tr("%1 ch").arg(channelCount) : QString(), channelLayout},
                         QStringLiteral("  ")));

    int sampleRate = ctrl->getProperty("audio-params/samplerate").toInt();
    if (sampleRate <= 0)
    {
        sampleRate = selectedAudioTrack.value(QStringLiteral("demux-samplerate")).toInt();
    }
    if (sampleRate <= 0 && audioStream)
    {
        sampleRate = audioStream->sampleRate;
    }
    addLine(lines, tr("Sample Rate"), sampleRate > 0 ? QStringLiteral("%1 Hz").arg(sampleRate) : QString());

    qint64 audioBitrate = ctrl->getProperty("audio-bitrate").toLongLong();
    if (audioBitrate <= 0 && audioStream)
    {
        audioBitrate = audioStream->bitRate;
    }
    addLine(lines, tr("Bitrate"), formatBitrateValue(audioBitrate));

    if (!selectedSubtitleTrack.isEmpty())
    {
        const QString subtitleLabel = firstNonEmpty({selectedSubtitleTrack.value(QStringLiteral("title")).toString(),
                                                     selectedSubtitleTrack.value(QStringLiteral("lang")).toString()});
        if (!subtitleLabel.isEmpty())
        {
            addLine(lines, tr("Subtitle"), subtitleLabel);
        }
    }

    m_statisticsOverlay->setLines(lines);
    if (m_showStatisticsOverlay)
    {
        updateOverlayLayout();
    }
}

void PlayerView::toggleFullscreenWindow()
{
    if (window()->isFullScreen())
    {
        window()->showNormal();
    }
    else
    {
        window()->showFullScreen();
    }
}


QCoro::Task<void> PlayerView::executeFetchLogo(QPointer<PlayerView> safeThis, QEmbyCore *core, QString mediaId, QString serverId)
{
    try
    {
        
        MediaItem detail = co_await core->mediaService()->getItemDetail(mediaId, serverId);

        
        if (!safeThis || safeThis->m_currentMediaId != mediaId)
        {
            co_return;
        }

        if (!detail.images.logoTag.isEmpty())
        {
            QPixmap pix = co_await core->mediaService()->fetchImage(mediaId, "Logo", detail.images.logoTag, 400);

            if (safeThis && safeThis->m_currentMediaId == mediaId && !pix.isNull())
            {
                
                QPixmap scaledPix = pix.scaled(140, 55, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                safeThis->m_logoLabel->setPixmap(scaledPix);
                safeThis->m_logoLabel->adjustSize();

                
                
                safeThis->m_logoOpacity->setOpacity(safeThis->m_topOpacity->opacity());
                safeThis->m_logoLabel->show();
                safeThis->updateOverlayLayout();
            }
        }
    }
    catch (...)
    {
        
    }
}

QCoro::Task<void> PlayerView::resolveDanmakuPlaybackContext(
    QPointer<PlayerView> safeThis,
    QPointer<QEmbyCore> core,
    QString mediaId,
    QString fallbackTitle,
    MediaSourceInfo sourceInfo,
    QString serverId)
{
    if (!safeThis || !core || !core->mediaService() || mediaId.isEmpty()) {
        co_return;
    }

    try {
        MediaItem detail =
            co_await core->mediaService()->getItemDetail(mediaId, serverId);
        if (!safeThis || safeThis->m_currentMediaId != mediaId ||
            !safeThis->m_danmakuController) {
            co_return;
        }
        if (detail.id.isEmpty()) {
            detail.id = mediaId;
        }
        if (detail.name.isEmpty()) {
            detail.name = fallbackTitle;
        }
        if (sourceInfo.id.isEmpty() && !detail.mediaSources.isEmpty()) {
            const auto selectedIt = std::find_if(
                detail.mediaSources.cbegin(), detail.mediaSources.cend(),
                [safeThis](const MediaSourceInfo &candidate) {
                    return safeThis &&
                           candidate.id == safeThis->m_currentMediaSourceId;
                });
            sourceInfo = selectedIt == detail.mediaSources.cend()
                             ? detail.mediaSources.first()
                             : *selectedIt;
        }

        PlayerLaunchContext context;
        context.mediaItem = detail;
        context.selectedSource = sourceInfo;
        qDebug().noquote()
            << "[Danmaku][PlayerView] Resolved complete playback context"
            << "| mediaId:" << detail.id
            << "| sourceId:" << sourceInfo.id
            << "| itemType:" << detail.type
            << "| title:" << detail.name
            << "| providerIds:" << detail.providerIds.keys().join(QStringLiteral(","));
        safeThis->m_danmakuController->setPlaybackContext(context);
    } catch (const std::exception &e) {
        if (safeThis && safeThis->m_currentMediaId == mediaId) {
            qWarning().noquote()
                << "[Danmaku][PlayerView] Failed to resolve complete playback context"
                << "| mediaId:" << mediaId
                << "| error:" << e.what();
        }
    }
}

QCoro::Task<void> PlayerView::ensureMediaSourcesThenPlay(QString mediaId,
                                                         QString title,
                                                         QString streamUrl,
                                                         long long startPositionTicks,
                                                         MediaSourceInfo currentSource,
                                                         QString serverId)
{
    QPointer<PlayerView> safeThis(this);
    QPointer<MediaService> mediaService(m_core ? m_core->mediaService() : nullptr);
    if (!mediaService)
    {
        if (safeThis)
            safeThis->playMedia(mediaId, title, streamUrl, startPositionTicks, QVariant(), false);
        co_return;
    }

    MediaSourceInfo source = currentSource;
    MediaItem detailItem;
    try
    {
        // Stage 0: background-prefetched source (continuous play) is richer
        // than the item-detail payload — it already carries the negotiated
        // DirectStreamUrl — so prefer it whenever the current source lacks
        // a negotiated URL.
        const auto cached = m_prefetchedSources.constFind(mediaId);
        if (cached != m_prefetchedSources.constEnd()
            && (source.id.isEmpty() || source.directStreamUrl.isEmpty()))
        {
            source = cached.value();
        }

        // Stage 1: no source at all -> lightweight item-detail API.
        if (source.id.isEmpty())
        {
            MediaItem detail = co_await mediaService->getItemDetail(mediaId, serverId);
            if (!safeThis || !mediaService)
                co_return;
            if (!detail.mediaSources.isEmpty())
            {
                source = detail.mediaSources.first();
            }
            // Keep the fetched detail so playMedia can see this item's
            // serverId — without it m_currentMediaItem.serverId is empty and
            // every downstream URL join falls back to the active server.
            detailItem = detail;
        }

        // Stage 2: source present but DirectStreamUrl missing and the path
        // cannot be direct-played from this client. DirectStreamUrl is a
        // *negotiated* field only filled by the PlaybackInfo endpoint, and
        // servers behind emby2Alist-style proxies disable the /stream
        // fallback endpoint, so without the negotiated URL playback cannot
        // start at all. Same playability rule as getStreamUrl(): loopback
        // http paths (alist 127.0.0.1:5244) do not count as playable.
        if (!source.id.isEmpty() && source.directStreamUrl.isEmpty()
            && !MediaService::isDirectPlayablePath(source.path))
        {
            PlaybackInfo pb = co_await mediaService->getPlaybackInfo(mediaId, serverId);
            if (!safeThis || !mediaService)
                co_return;
            if (!pb.mediaSources.isEmpty())
            {
                const MediaSourceInfo &negotiated = pb.mediaSources.first();
                // Adopt the negotiated source when it carries any playable
                // URL: DirectStreamUrl (direct stream) or TranscodingUrl
                // (server-mandated transcode).
                if (!negotiated.directStreamUrl.isEmpty()
                    || !negotiated.transcodingUrl.isEmpty())
                {
                    source = negotiated;
                }
            }
        }
    }
    catch (const std::exception &e)
    {
        qWarning() << "[PlayerView] playback source negotiation failed:"
                   << e.what();
    }

    if (safeThis)
    {
        // 跨服路由：回调必须带 serverId。只传 MediaSourceInfo 时 playMedia
        // 解析不出 mediaItem，m_currentMediaItem.serverId 为空，导致
        // getStreamUrl / 播放上报 / 弹幕上下文全部 fallback 到 active server
        // （PlaybackInfo 协商到了正确服务器的相对 DirectStreamUrl，却被拼上
        // active server 的 host 播放）。
        PlayerLaunchContext context;
        context.mediaItem = detailItem;
        if (context.mediaItem.serverId.isEmpty())
        {
            context.mediaItem.serverId = serverId;
        }
        context.selectedSource = source;
        safeThis->playMedia(mediaId, title, streamUrl, startPositionTicks,
                            QVariant::fromValue(context), false);
    }
}

QCoro::Task<void> PlayerView::prefetchNextEpisodeSource()
{
    QPointer<PlayerView> safeThis(this);
    QPointer<MediaService> mediaService(m_core ? m_core->mediaService() : nullptr);
    if (!mediaService)
    {
        co_return;
    }

    // Only worthwhile when continuous playback is enabled.
    if (!ConfigStore::instance()->get<bool>(ConfigKeys::PlayerContinuousPlay, true))
    {
        co_return;
    }

    // Need the switcher cache to know what the next episode is.
    if (!m_switcherCacheReady)
    {
        co_await ensureMediaSwitcherDataLoaded();
        if (!safeThis || !mediaService)
        {
            co_return;
        }
    }

    QString nextId;
    QString nextTitle;
    long long startTicks = 0;
    if (!findAdjacentMediaFromCache(1, nextId, nextTitle, startTicks))
    {
        co_return;
    }
    if (nextId.isEmpty() || m_prefetchedSources.contains(nextId))
    {
        co_return;
    }

    // Keep the cache tiny: episodes are consumed in order, so dropping old
    // entries when it grows is safe.
    if (m_prefetchedSources.size() > 4)
    {
        m_prefetchedSources.clear();
    }

    try
    {
        PlaybackInfo pb = co_await mediaService->getPlaybackInfo(
            nextId, m_currentMediaItem.serverId);
        if (!safeThis || !mediaService)
        {
            co_return;
        }
        if (!pb.mediaSources.isEmpty())
        {
            safeThis->m_prefetchedSources.insert(nextId, pb.mediaSources.first());
            qDebug().noquote() << "[PlayerView] prefetched playback source for next episode"
                               << "| mediaId:" << nextId;

            // 顺手预热下一集的流连接。用 launchTask 独立跑（不 co_await），
            // 避免把本协程挂在预热请求上；预热失败也不影响任何正常流程。
            launchTask(safeThis->prewarmStreamConnection(pb.mediaSources.first()),
                       safeThis.data());
        }
    }
    catch (const std::exception &e)
    {
        // Best-effort prefetch: silent failure, the normal negotiation path
        // still runs when the next episode actually starts.
        qDebug() << "[PlayerView] next-episode source prefetch failed:" << e.what();
    }
}

QCoro::Task<void> PlayerView::prewarmStreamConnection(MediaSourceInfo source)
{
    if (!m_core)
    {
        co_return;
    }

    NetworkManager *network = m_core->networkManager();
    if (!network)
    {
        co_return;
    }

    // 与实际播放走同一套 URL 解析：绝对地址直接用，相对地址拼服务器前缀。
    QString url = source.directStreamUrl;
    if (url.isEmpty())
    {
        url = source.transcodingUrl;
    }
    if (url.isEmpty())
    {
        co_return;
    }

    const ServerProfile profile = m_core->serverManager()->activeProfile();
    if (!url.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
        && !url.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive))
    {
        if (profile.url.isEmpty())
        {
            co_return;
        }
        url = profile.url + url;
    }

    // 服务器可能对 UA 有白名单，预热请求要用与播放一致的身份，
    // 否则可能直接被拒，等于没预热。
    QMap<QString, QString> headers;
    const QString userAgent = profile.effectiveUserAgent();
    if (!userAgent.isEmpty())
    {
        headers.insert(QStringLiteral("User-Agent"), userAgent);
    }

    NetworkRequestOptions options;
    options.timeoutMs = 5000;

    try
    {
        // maximumBytes = 1 ⇒ getBytesLimited 内部发 Range: bytes=0-0，
        // 只要首字节：把 DNS / TCP / TLS / CDN 首包这些一次性开销付掉即可。
        const QByteArray head = co_await network->getBytesLimited(url, headers, 1, options);
        qDebug().noquote() << "[PlayerView] stream prewarm finished"
                           << "| bytes:" << head.size()
                           << "| host:" << QUrl(url).host();
    }
    catch (const std::exception &e)
    {
        // 尽力而为：预热失败不改变任何行为，切集时照常走原来的协商路径。
        qDebug() << "[PlayerView] stream prewarm failed (ignored):" << e.what();
    }
}

void PlayerView::playMedia(const QString &mediaId, const QString &title, const QString &streamUrl,
                           long long startPositionTicks, const QVariant &sourceInfoVar,
                           bool allowSourceFetch)
{
    PlayerLaunchContext launchContext;
    MediaSourceInfo resolvedSourceInfo;
    MediaItem resolvedItem;
    if (sourceInfoVar.isValid() && sourceInfoVar.canConvert<PlayerLaunchContext>())
    {
        launchContext = sourceInfoVar.value<PlayerLaunchContext>();
        resolvedItem = launchContext.mediaItem;
        resolvedSourceInfo = launchContext.selectedSource;
    }
    else if (sourceInfoVar.isValid() && sourceInfoVar.canConvert<MediaSourceInfo>())
    {
        resolvedSourceInfo = sourceInfoVar.value<MediaSourceInfo>();
    }

    // Playback source needs preparation when either:
    //  a) sourceInfo is missing entirely (strm item still probing etc.) ->
    //     fetch via the lightweight item-detail API; or
    //  b) sourceInfo exists but has no DirectStreamUrl while the path cannot
    //     be direct-played from this client -> negotiate via PlaybackInfo
    //     (POST + DeviceProfile); servers behind emby2Alist-style proxies
    //     only serve the negotiated URL and disable the /stream fallback.
    //     Path "direct playability" must use the same rule as getStreamUrl()
    //     (loopback http paths are NOT playable), otherwise negotiation is
    //     skipped for paths that getStreamUrl will refuse to use.
    if (allowSourceFetch && !mediaId.isEmpty() && m_core)
    {
        const bool needSourceFetch = resolvedSourceInfo.id.isEmpty();
        const bool pathIsDirectPlayable =
            MediaService::isDirectPlayablePath(resolvedSourceInfo.path);
        const bool needPlaybackNegotiation =
            !needSourceFetch
            && resolvedSourceInfo.directStreamUrl.isEmpty()
            && !pathIsDirectPlayable;
        if (needSourceFetch || needPlaybackNegotiation)
        {
            launchTask(ensureMediaSourcesThenPlay(mediaId, title, streamUrl,
                                                      startPositionTicks, resolvedSourceInfo,
                                                      resolvedItem.serverId), this);
            return;
        }
    }

    connect(m_mpvWidget, &MpvWidget::positionChanged, this, &PlayerView::onPositionChanged, Qt::UniqueConnection);

    m_hasReportedStop = false;
    m_isPlaybackFinished = false;
    m_autoPlayAdvanceInProgress = false;
    m_currentMediaId = mediaId;
    m_currentMediaSourceId = mediaId;
    m_currentPlaySessionId.clear();
    m_currentMediaItem = resolvedItem;
    m_currentMediaSourceInfo = resolvedSourceInfo;
    hideRightSidebar(true);
    hideHudMediaSwitcher();
    if (m_activePopup)
    {
        m_activePopup->deleteLater();
        m_activePopup = nullptr;
    }
    closeSubtitleSubmenu();
    closeActivePlayerDialog();

    
    m_logoLabel->clear();
    m_logoLabel->hide();

    
    m_originalStreamUrl = streamUrl;
    m_currentSourceInfoVar = sourceInfoVar;

    QString actualStreamUrl = streamUrl;
    if (!resolvedSourceInfo.id.isEmpty())
    {
        // 跨服路由：直接 stream URL 必须按 m_currentMediaItem.serverId 拼，
        // 否则 getStreamUrl(mediaId, sourceInfo) 默认走 active server 拼接，
        // 覆盖原本正确的 streamUrl，导致聚合 item 在多个 server id 撞库时
        // 播放到 active server 的同名 id 资源。
        QString directUrl = m_core->mediaService()->getStreamUrl(
            mediaId, resolvedSourceInfo, m_currentMediaItem.serverId);
        if (!directUrl.isEmpty())
        {
            actualStreamUrl = directUrl;
        }
    }

    QUrl streamQUrl(actualStreamUrl);
    QUrl origQUrl(streamUrl); 

    QUrlQuery query(streamQUrl);
    if (!query.hasQueryItem("mediaSourceId") && QUrlQuery(origQUrl).hasQueryItem("mediaSourceId"))
    {
        query = QUrlQuery(origQUrl); 
    }

    if (query.hasQueryItem("mediaSourceId"))
    {
        m_currentMediaSourceId = query.queryItemValue("mediaSourceId");
    }
    else if (query.hasQueryItem("MediaSourceId"))
    {
        m_currentMediaSourceId = query.queryItemValue("MediaSourceId");
    }

    if (m_currentMediaSourceInfo.id.isEmpty() && !m_currentMediaItem.mediaSources.isEmpty())
    {
        for (const MediaSourceInfo &candidate : m_currentMediaItem.mediaSources)
        {
            if (!m_currentMediaSourceId.isEmpty() &&
                candidate.id.compare(m_currentMediaSourceId, Qt::CaseInsensitive) == 0)
            {
                m_currentMediaSourceInfo = candidate;
                break;
            }
        }

        if (m_currentMediaSourceInfo.id.isEmpty())
        {
            m_currentMediaSourceInfo = m_currentMediaItem.mediaSources.first();
        }
    }

    m_pendingSeekSeconds = startPositionTicks / 10000000.0;

    QString displayTitle = title.trimmed();
    if (!m_currentMediaItem.id.isEmpty())
    {
        const QString resolvedTitle = MediaItemUtils::playbackTitle(m_currentMediaItem, displayTitle).trimmed();
        if (!resolvedTitle.isEmpty())
        {
            displayTitle = resolvedTitle;
        }
    }
    if (displayTitle.isEmpty())
    {
        displayTitle = title;
    }

    m_fullTitle = displayTitle;
    Q_EMIT playbackTitleChanged(m_fullTitle);
    updateTitleElision();

    m_currentPosition = m_pendingSeekSeconds;
    m_totalDuration = 0.0;

    if (m_statisticsOverlay)
    {
        if (m_showStatisticsOverlay)
        {
            updateStatisticsDisplay();
            ensureStatisticsWindow();
            if (m_statisticsWindow)
            {
                m_statisticsWindow->show();
                syncStatisticsWindow();
            }
            else
            {
                m_statisticsOverlay->show();
            }
        }
        else
        {
            // 只需要隐藏宿主窗口；面板本体留在窗口里（隐藏窗口即不可见），
            // 避免下次显示时「窗口在、内容空」。
            if (m_statisticsWindow)
            {
                m_statisticsWindow->hide();
            }
            else
            {
                m_statisticsOverlay->hide();
            }
        }
    }

    QWidget *win = window();
    if (win)
    {
        m_wasMaximized = win->isMaximized();
        m_originalGeometry = win->geometry();
        m_hasSetVideoSize = false;

        
        if (m_wasMaximized)
        {
            win->showFullScreen();
        }
    }


    
    
    
    m_isSeriesMode = false;
    m_seriesId.clear();
    m_seriesName.clear();
    m_episodeSegments = {};
    m_introSkipped = false;
    m_outroSkipped = false;
    m_segmentsRequested = false;
    m_manualIntroSec = 0;
    m_manualOutroSec = 0;
    clearMediaSwitcherCache();
    updateMediaSwitcherButton();
    refreshManualSkipSettings();

    if (resolvedItem.type == "Episode" && !resolvedItem.seriesId.isEmpty())
    {
        m_isSeriesMode = true;
        m_seriesId = resolvedItem.seriesId;
        m_seriesName = resolvedItem.seriesName;
        updateMediaSwitcherButton();
        refreshManualSkipSettings();
        requestIntroDBSegments();
        
        ensureMediaSwitcherDataLoaded();
    }
    else
    {
        
        auto detectSeriesMode = [](QPointer<PlayerView> safeThis, QEmbyCore *core,
                                   QString mId, QString serverId) -> QCoro::Task<void>
        {
            try
            {
                MediaItem detail =
                    co_await core->mediaService()->getItemDetail(mId, serverId);
                if (!safeThis || safeThis->m_currentMediaId != mId)
                    co_return;
                if (detail.type == "Episode" && !detail.seriesId.isEmpty())
                {
                    safeThis->m_isSeriesMode = true;
                    safeThis->m_seriesId = detail.seriesId;
                    safeThis->m_seriesName = detail.seriesName;
                    safeThis->m_currentMediaItem = detail;
                    safeThis->clearMediaSwitcherCache();
                    safeThis->updateMediaSwitcherButton();
                    safeThis->refreshManualSkipSettings();
                    safeThis->requestIntroDBSegments();
                    
                    safeThis->ensureMediaSwitcherDataLoaded();
                    if (safeThis->useHudMediaSwitcher() && safeThis->m_mediaSwitchDrawer &&
                        safeThis->m_mediaSwitchDrawer->isVisible())
                    {
                        safeThis->showHudMediaSwitcher();
                    }
                    else if (safeThis->m_isRightSidebarVisible)
                    {
                        safeThis->showRightSidebar();
                    }
                }
            }
            catch (...)
            {
            }
        };
        detectSeriesMode(QPointer<PlayerView>(this), m_core, mediaId,
                         resolvedItem.serverId);

        ensureMediaSwitcherDataLoaded();
    }

    // Next-episode source prefetch is now driven by playback progress (see
    // onPositionChanged); arm the one-shot trigger for this media.
    m_prefetchTriggered = false;
    m_prefetchThreshold =
        ConfigStore::instance()->get<int>(ConfigKeys::PlayerPrefetchThreshold, 90);

    
    
    
    executeFetchLogo(QPointer<PlayerView>(this), m_core, mediaId,
                     resolvedItem.serverId);

    
    
    const ServerProfile activeProfile = m_core->serverManager()->activeProfile();
    const QString activeServerId = activeProfile.id;
    // Strict-UA servers reject the default libmpv UA on stream requests;
    // push the resolved UA (per-server > global > none) before loading.
    m_mpvWidget->setCustomUserAgent(activeProfile.effectiveUserAgent());
    // 新媒体起播时重置 sticky 软解状态，再按本片数据重新决策。
    m_swDecodeForCurrentMedia = false;
    applyDecodeDecision(resolvedSourceInfo);
    m_dvRecheckPending.remove(mediaId);
    // 列表/继续观看路径的 sourceInfo 不带 MediaStreams，初始判定无数据。
    // 先正常硬解起播，异步拉 detail 复查；确认为纯 DV 后软解重载当前进度
    //（detail 的 MediaStreams 完整，qemby.log 已证实此类路径判定恒 false）。
    if (!m_swDecodeForCurrentMedia && !hasVideoStreamData(resolvedSourceInfo)
        && !mediaId.isEmpty() && m_core)
    {
        m_dvRecheckPending.insert(mediaId);
        const QString serverId = resolvedItem.serverId;
        auto dvRecheck = [](QPointer<PlayerView> safeThis, QEmbyCore *core,
                            QString mediaId, QString streamUrl,
                            QString serverId) -> QCoro::Task<void>
        {
            try
            {
                MediaItem detail = co_await core->mediaService()->getItemDetail(mediaId, serverId);
                if (!safeThis || safeThis->m_currentMediaId != mediaId)
                    co_return;
                MediaSourceInfo source;
                for (const MediaSourceInfo &s : detail.mediaSources)
                {
                    if (s.id == mediaId || source.id.isEmpty())
                        source = s;
                    if (s.id == mediaId)
                        break;
                }
                const bool rechecked = sourceNeedsForcedSoftwareDecode(source);
                if (!rechecked)
                    co_return;
                // DV 自动独立窗口（列表/继续观看路径）：内嵌配置下确认纯 DV →
                // 带当前进度切到独立窗口播放（内嵌的 render API 不处理 DV 的
                // IPT 色彩，独立窗口 wid + gpu-next 正常）。先启动独立窗口，
                // 再复用返回流程退出内嵌——内嵌的 stop 上报与独立窗口的 start
                // 使用同一位置，Emby 端视角连续。
                if (!safeThis->m_standalone &&
                    ConfigStore::instance()->get<bool>(
                        ConfigKeys::PlayerDvAutoIndependentWindow, true))
                {
                    const long long switchTicks = static_cast<long long>(
                        qMax(0.0, safeThis->m_currentPosition) * 10000000.0);
                    PlayerLaunchContext switchContext;
                    switchContext.mediaItem = safeThis->m_currentMediaItem;
                    switchContext.selectedSource = source;
                    qInfo().noquote()
                        << "[PlayerView] DV recheck → switching to independent window"
                        << "| mediaId:" << mediaId
                        << "| position:" << safeThis->m_currentPosition;
                    PlaybackManager::instance()->relaunchInIndependentWindow(
                        mediaId, safeThis->m_fullTitle, streamUrl, switchTicks,
                        QVariant::fromValue(switchContext));
                    safeThis->onBackClicked();
                    co_return;
                }
                qInfo().noquote() << "[PlayerView] DV recheck confirmed pure DV, reloading"
                                  << "| mediaId:" << mediaId
                                  << "| position:" << safeThis->m_currentPosition;
                safeThis->m_swDecodeForCurrentMedia = true;
                safeThis->m_mpvWidget->setForceSoftwareDecode(true);
                safeThis->m_pendingSeekSeconds = qMax(0.0, safeThis->m_currentPosition);
                safeThis->m_windowRestorePending = true;
                safeThis->m_windowRestoreShouldPlay = safeThis->m_isPlaying;
                safeThis->m_isBuffering = true;
                safeThis->updateLoadingState();
                if (safeThis->m_danmakuController)
                    safeThis->m_danmakuController->prepareForMediaReload();
                safeThis->m_mpvWidget->loadMedia(streamUrl, serverId);
            }
            catch (...)
            {
            }
        };
        launchTask(dvRecheck(QPointer<PlayerView>(this), m_core, mediaId,
                             actualStreamUrl, serverId), this);
    }
    m_mpvWidget->loadMedia(actualStreamUrl, activeServerId);

    const bool hasCompleteDanmakuContext =
        !resolvedItem.id.isEmpty() && !resolvedItem.name.trimmed().isEmpty() &&
        !resolvedItem.type.trimmed().isEmpty();
    if (hasCompleteDanmakuContext)
    {
        PlayerLaunchContext danmakuContext;
        danmakuContext.mediaItem = resolvedItem;
        danmakuContext.selectedSource = resolvedSourceInfo;
        qDebug().noquote() << "[Danmaku][PlayerView] Prepared playback context"
                           << "| mediaId:" << danmakuContext.mediaItem.id
                           << "| sourceId:" << danmakuContext.selectedSource.id
                           << "| title:" << danmakuContext.mediaItem.name;
        m_danmakuController->setPlaybackContext(danmakuContext);
    }
    else if (!mediaId.isEmpty())
    {
        m_danmakuController->clearPlaybackContext();
        qDebug().noquote()
            << "[Danmaku][PlayerView] Playback metadata incomplete, resolving before danmaku search"
            << "| mediaId:" << mediaId
            << "| sourceId:" << resolvedSourceInfo.id
            << "| sourceInfoValid:" << sourceInfoVar.isValid();
        launchTask(resolveDanmakuPlaybackContext(
                           QPointer<PlayerView>(this), QPointer<QEmbyCore>(m_core),
                           mediaId, title, resolvedSourceInfo, resolvedItem.serverId), this);
    }
    else
    {
        m_danmakuController->clearPlaybackContext();
    }

    
    
    
    m_isBuffering = true;
    m_isSeeking = false;
    updateLoadingState();

    
    
    
    m_targetAudioStreamIndex = -2; 
    m_targetSubStreamIndex = -2;

    QVariantList pendingSubtitles;

    // 统一收口：无论 source 来自详情页 UI 选择、PlaybackInfo 协商还是
    // item-detail 兜底，都在此应用「记忆选择 / 偏好规则」重标 isDefault，
    // 再进入下方的 target 流扫描。修复：协商路径（ensureMediaSourcesThenPlay）
    // 会用服务器原始 flags 整体覆盖详情页标记过的 source 且不再应用规则，
    // 而 mpv 初始化为 sid=no，无 default 时字幕保持关闭——表现为详情页
    // 显示选中了字幕、实际播放却是「关闭字幕」。规则应用是纯内存计算，
    // 对起播速度无可感知影响。
    if (!resolvedSourceInfo.id.isEmpty() && m_core && m_core->serverManager())
    {
        PlayerPreferenceUtils::applyRememberedOrPreferredStreamRules(
            resolvedSourceInfo, m_core->serverManager()->activeProfile().id,
            mediaId,
            ConfigStore::instance()->get<QString>(ConfigKeys::PlayerAudioLang, "auto"),
            ConfigStore::instance()->get<QString>(ConfigKeys::PlayerSubLang, "auto"));
    }

    if (!resolvedSourceInfo.id.isEmpty())
    {
        MediaSourceInfo sourceInfo = resolvedSourceInfo;

        m_targetAudioStreamIndex = -2; 
        m_targetSubStreamIndex = -2;
        bool hasExternalDefault = false;
        bool foundDefaultAudio = false;
        PlayerPreferenceUtils::RememberedStreamSelection remembered;

        for (const auto &stream : sourceInfo.mediaStreams)
        {
            if (stream.type == "Audio" && stream.isDefault)
            {
                m_targetAudioStreamIndex = stream.index;
                foundDefaultAudio = true;
            }
            else if (stream.type == "Subtitle" && stream.isDefault)
            {
                if (stream.isExternal)
                {
                    hasExternalDefault = true;
                }
                else
                {
                    m_targetSubStreamIndex = stream.index;
                }
            }
        }

        if (m_core && m_core->serverManager())
        {
            remembered =
                PlayerPreferenceUtils::validatedRememberedStreamSelection(
                    m_core->serverManager()->activeProfile().id, mediaId,
                    sourceInfo);
            if (remembered.audioIndex.has_value())
            {
                m_targetAudioStreamIndex = *remembered.audioIndex;
            }
            if (remembered.subtitleIndex.has_value())
            {
                
                m_targetSubStreamIndex = *remembered.subtitleIndex;
            }
        }

        
        

        
        if (hasExternalDefault && !remembered.subtitleIndex.has_value())
        {
            m_targetSubStreamIndex = -2;
        }

        QString tokenQuery;
        if (query.hasQueryItem("api_key"))
        {
            tokenQuery = "api_key=" + query.queryItemValue("api_key");
        }
        else if (query.hasQueryItem("X-Emby-Token"))
        {
            tokenQuery = "X-Emby-Token=" + query.queryItemValue("X-Emby-Token");
        }
        else if (query.hasQueryItem("api_token"))
        {
            tokenQuery = "api_token=" + query.queryItemValue("api_token");
        }

        
        
        QString baseUrl;
        int videosIdx = streamUrl.indexOf("/videos/", 0, Qt::CaseInsensitive);
        if (videosIdx != -1)
        {
            baseUrl = streamUrl.left(videosIdx);
        }
        else
        {
            baseUrl = origQUrl.scheme() + "://" + origQUrl.authority();
        }

        for (const auto &stream : sourceInfo.mediaStreams)
        {
            
            
            if (stream.type == "Subtitle" && stream.isExternal)
            {
                QString subUrl = stream.deliveryUrl;

                
                if (subUrl.isEmpty())
                {
                    QString codec = stream.codec.toLower();
                    if (codec.isEmpty() || codec == "subrip")
                    {
                        codec = "srt"; 
                    }

                    
                    subUrl = QString("/Videos/%1/%2/Subtitles/%3/Stream.%4")
                                 .arg(mediaId)
                                 .arg(sourceInfo.id)
                                 .arg(stream.index)
                                 .arg(codec);
                }

                
                if (!subUrl.startsWith("http"))
                {
                    if (!subUrl.startsWith("/"))
                    {
                        subUrl = "/" + subUrl;
                    }
                    subUrl = baseUrl + subUrl;

                    
                    if (!tokenQuery.isEmpty())
                    {
                        subUrl += (subUrl.contains("?") ? "&" : "?") + tokenQuery;
                    }
                }

                QString subTitle = stream.displayTitle.isEmpty() ? stream.language : stream.displayTitle;
                if (subTitle.isEmpty())
                {
                    subTitle = tr("External Sub %1").arg(stream.index);
                }

                
                
                const bool selectExternal =
                    remembered.subtitleIndex.has_value()
                        ? *remembered.subtitleIndex == stream.index
                        : stream.isDefault;
                QString flag = selectExternal ? "select" : "auto";

                
                QVariantMap subMap;
                subMap["url"] = subUrl;
                subMap["flag"] = flag;
                subMap["title"] = subTitle;
                subMap["lang"] = stream.language;
                pendingSubtitles.append(subMap);
            }
        }
    }

    
    setProperty("pendingSubtitles", pendingSubtitles);

    m_mpvWidget->play();

    m_isPlaying = true;
    updatePowerInhibition();
    m_playPauseBtn->setIcon(QIcon(":/svg/player/pause.svg"));

    
    m_currentVolume = ConfigStore::instance()->get<double>(ConfigKeys::PlayerVolumeLevel, 100.0);
    m_isMuted = ConfigStore::instance()->get<bool>(ConfigKeys::PlayerVolumeMuted, false);

    m_mpvWidget->controller()->setProperty("volume", m_currentVolume);
    m_mpvWidget->controller()->setProperty("mute", m_isMuted);

    
    
    m_volumeSlider->blockSignals(true);
    m_volumeSlider->setValue(m_isMuted ? 0 : static_cast<int>(m_currentVolume));
    m_volumeSlider->blockSignals(false);
    m_volumeBtn->setIcon(QIcon(m_isMuted ? ":/svg/player/volume-mute.svg" : ":/svg/player/volume.svg"));

    
    // 默认「适应屏幕」（0）：播放区随窗口自适应、比例不符处留黑边（与 PotPlayer
    // 等播放器一致）。旧的默认值是 1（铺满裁剪），窗口比例与片源不一致时会裁掉
    // 画面上下/左右——容易被误判成"比例算错了"。
    m_videoScaleMode = ConfigStore::instance()->get<int>(ConfigKeys::PlayerDefaultScale, 0);
    if (m_nativeDanmakuOverlay) {
        m_nativeDanmakuOverlay->setVideoScaleMode(m_videoScaleMode);
    }
    setScaleIcon();

    switch (m_videoScaleMode)
    {
    case 0:
        m_mpvWidget->controller()->setProperty("keepaspect", true);
        m_mpvWidget->controller()->setProperty("panscan", 0.0);
        m_mpvWidget->controller()->setProperty("video-unscaled", false);
        break;
    case 1:
        m_mpvWidget->controller()->setProperty("keepaspect", true);
        m_mpvWidget->controller()->setProperty("panscan", 1.0);
        m_mpvWidget->controller()->setProperty("video-unscaled", false);
        break;
    case 2:
        m_mpvWidget->controller()->setProperty("keepaspect", false);
        m_mpvWidget->controller()->setProperty("panscan", 0.0);
        m_mpvWidget->controller()->setProperty("video-unscaled", false);
        break;
    case 3:
        m_mpvWidget->controller()->setProperty("keepaspect", true);
        m_mpvWidget->controller()->setProperty("panscan", 0.0);
        m_mpvWidget->controller()->setProperty("video-unscaled", true);
        break;
    }

    // 诊断：记录本次播放实际生效的画幅模式与播放区尺寸——"播放区没跟着窗口
    // 自适应"这类问题的第一现场证据（0=适应屏幕 1=铺满裁剪 2=拉伸 3=原始比例）。
    qInfo().noquote() << "[PlayerView] Video scale mode applied"
                      << "| mode:" << m_videoScaleMode
                      << "| standalone:" << m_standalone
                      << "| videoWidget:"
                      << QStringLiteral("%1x%2").arg(m_mpvWidget->width()).arg(m_mpvWidget->height());

    
    auto startSessionTask = [](QPointer<PlayerView> safeThis, MediaService *s, QString mId, QString sId,
                               long long ticks) -> QCoro::Task<void>
    {
        QString sessionId = co_await s->reportPlaybackStart(mId, sId, ticks);
        if (safeThis && safeThis->m_currentMediaId == mId)
        {
            safeThis->m_currentPlaySessionId = sessionId;
        }
    };
    startSessionTask(QPointer<PlayerView>(this), m_core->mediaService(), m_currentMediaId, m_currentMediaSourceId,
                     startPositionTicks);

    // Trakt: fresh scrobble session for the new media.
    m_traktIds = {};
    m_traktResolvedMediaId.clear();
    m_traktResolveInFlight = false;
    m_traktResumeChecked = false;
    m_traktStopped = false;
    m_traktLastScrobbleMs = 0;
    if (traktScrobbleActive())
    {
        launchTask(traktScrobbleAt(QStringLiteral("start")), this);
    }

    m_reportTimer->start();
    m_mousePollTimer->start();
    m_bufferTimer->start();
    showControls();
}

void PlayerView::reportProgressToServer()
{
    if (m_isViewTearingDown || m_currentMediaId.isEmpty() || m_hasReportedStop || m_currentPlaySessionId.isEmpty())
    {
        return;
    }
    long long currentTicks = static_cast<long long>(m_currentPosition * 10000000.0);

    m_core->mediaService()->reportPlaybackProgress(m_currentMediaId, m_currentMediaSourceId, currentTicks, !m_isPlaying,
                                                   m_currentPlaySessionId);

    // Trakt scrobble update: Trakt recommends ~1 call per minute; piggyback
    // on this 10s Emby report timer with its own throttle.
    traktOnProgressTick();

    if (m_showStatisticsOverlay)
    {
        updateStatisticsDisplay();
    }
}

bool PlayerView::traktScrobbleActive() const
{
    if (m_traktStopped || m_currentMediaId.isEmpty())
    {
        return false;
    }
    const QString type = m_currentMediaItem.type;
    if (type != QLatin1String("Movie") && type != QLatin1String("Episode"))
    {
        return false;
    }
    if (!ConfigStore::instance()->get<bool>(ConfigKeys::TraktScrobbleEnabled, false))
    {
        return false;
    }
    TraktService *service = TraktService::instance();
    return service->isLoggedIn() && !service->clientId().isEmpty();
}

QCoro::Task<TraktMediaIds> PlayerView::traktEnsureIdsResolved()
{
    if (m_traktResolvedMediaId != m_currentMediaId)
    {
        m_traktIds = {};
        m_traktResolveInFlight = false;
    }
    if (!m_traktIds.valid && !m_traktResolveInFlight)
    {
        m_traktResolveInFlight = true;
        MediaItem item = m_currentMediaItem;
        try
        {
            m_traktIds = co_await TraktService::instance()->resolveIds(item);
        }
        catch (const std::exception &e)
        {
            qDebug().noquote() << "[PlayerView][Trakt] Ids resolve failed"
                               << "| mediaId:" << m_currentMediaId
                               << "| error:" << e.what();
        }
        m_traktResolveInFlight = false;
        m_traktResolvedMediaId = m_currentMediaId;
    }
    co_return m_traktIds;
}

QCoro::Task<void> PlayerView::traktScrobbleAt(QString action)
{
    if (!traktScrobbleActive())
    {
        co_return;
    }
    const TraktMediaIds ids = co_await traktEnsureIdsResolved();
    if (!ids.valid || m_currentMediaId.isEmpty())
    {
        co_return;
    }
    const double percent = m_totalDuration > 0.0
                               ? qBound(0.0, m_currentPosition * 100.0 / m_totalDuration, 100.0)
                               : 0.0;
    co_await TraktService::instance()->scrobble(action, ids, percent);
    qDebug().noquote() << "[PlayerView][Trakt] Scrobble sent"
                       << "| action:" << action
                       << "| position:" << m_currentPosition
                       << "| percent:" << percent;
}

void PlayerView::traktOnProgressTick()
{
    if (!traktScrobbleActive())
    {
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_traktLastScrobbleMs != 0 && now - m_traktLastScrobbleMs < 60000)
    {
        return;
    }
    m_traktLastScrobbleMs = now;
    launchTask(traktScrobbleAt(QStringLiteral("start")), this);
}

void PlayerView::traktOnPauseStateChanged(bool isPaused)
{
    if (!traktScrobbleActive())
    {
        return;
    }
    // Pause keeps the position stored on Trakt (feeds cross-device resume);
    // resume just continues the same scrobble session.
    m_traktLastScrobbleMs = QDateTime::currentMSecsSinceEpoch();
    launchTask(traktScrobbleAt(isPaused ? QStringLiteral("pause")
                                        : QStringLiteral("start")),
               this);
}

void PlayerView::traktOnPlaybackStopped()
{
    if (!traktScrobbleActive())
    {
        return;
    }
    // "stop" marks the item watched when progress > 80%, otherwise Trakt
    // keeps the position for its own resume list.
    m_traktStopped = true;
    m_traktLastScrobbleMs = 0;
    launchTask(traktScrobbleAt(QStringLiteral("stop")), this);
}

QCoro::Task<void> PlayerView::traktCheckResumeProgress()
{
    if (!ConfigStore::instance()->get<bool>(ConfigKeys::TraktResumeCheckEnabled, false))
    {
        co_return;
    }
    TraktService *service = TraktService::instance();
    if (!service->isLoggedIn() || service->clientId().isEmpty())
    {
        co_return;
    }
    const TraktMediaIds ids = co_await traktEnsureIdsResolved();
    if (!ids.valid || m_totalDuration <= 0.0)
    {
        co_return;
    }
    double percent = -1.0;
    try
    {
        percent = co_await service->fetchStoredProgressPercent(ids);
    }
    catch (const std::exception &e)
    {
        qDebug().noquote() << "[PlayerView][Trakt] Resume check failed"
                           << "| error:" << e.what();
    }
    if (percent <= 0.0 || m_isViewTearingDown || m_totalDuration <= 0.0)
    {
        co_return;
    }
    const double traktPosition = m_totalDuration * percent / 100.0;
    // Only offer a jump that is meaningfully ahead of the Emby resume point.
    if (traktPosition < 60.0 || traktPosition <= m_currentPosition + 120.0)
    {
        co_return;
    }
    qDebug().noquote() << "[PlayerView][Trakt] Resume candidate"
                       << "| traktPosition:" << traktPosition
                       << "| embyPosition:" << m_currentPosition;

    QPointer<PlayerView> safeThis(this);
    QMessageBox *box = new QMessageBox(QMessageBox::Question, tr("Trakt Resume"),
                                       tr("Trakt progress: %1 (%2)\nContinue from there?")
                                           .arg(formatTime(traktPosition, m_totalDuration),
                                                QString::number(static_cast<int>(percent)) +QLatin1String("%")),
                                       QMessageBox::Yes | QMessageBox::No, this);
    box->setModal(false);
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QMessageBox::finished, this, [safeThis, traktPosition](int result)
            {
        if (!safeThis || result != QMessageBox::Yes)
        {
            return;
        }
        safeThis->m_pendingSeekSeconds = 0.0;
        safeThis->m_mpvWidget->seek(traktPosition);
        safeThis->m_currentPosition = traktPosition; });
    box->open();
}

void PlayerView::onBackClicked()
{
    if (m_isViewTearingDown)
    {
        return;
    }

    
    
    prepareForStackLeave();
    Q_EMIT navigateBack();

    
    
    QWidget *win = window();
    bool isEmbedded = qobject_cast<QMainWindow *>(win) != nullptr;
    bool wasMaximized = m_wasMaximized;
    QRect originalGeo = m_originalGeometry;
    if (win)
    {
        QPointer<QWidget> safeWin(win);
        QTimer::singleShot(0, this,
                           [safeWin, isEmbedded, wasMaximized, originalGeo]()
                           {
                               if (!safeWin)
                                   return;

                               if (isEmbedded)
                               {
                                   
                                   
                                   if (safeWin->isFullScreen())
                                   {
                                       safeWin->showMaximized();
                                   }
                                   
                               }
                               else
                               {
                                   
                                   if (safeWin->isFullScreen())
                                   {
                                       safeWin->showNormal();
                                   }
                                   if (wasMaximized)
                                   {
                                       safeWin->showMaximized();
                                   }
                                   else if (originalGeo.isValid())
                                   {
                                       safeWin->setGeometry(originalGeo);
                                   }
                               }
                           });
    }
}

void PlayerView::onPositionChanged(double position)
{
    if (m_isViewTearingDown)
    {
        return;
    }

    if (std::isnan(position) || std::isinf(position) || position < 0)
    {
        position = 0.0;
    }
    m_currentPosition = position;

    // Next-episode playback-source prefetch: one-shot trigger once progress
    // passes the configured percentage (0 = disabled). The prefetch coroutine
    // itself guards against re-fetching and against non-series media.
    if (m_prefetchThreshold > 0 && !m_prefetchTriggered
        && m_totalDuration > 0.0
        && position >= m_totalDuration * m_prefetchThreshold / 100.0)
    {
        m_prefetchTriggered = true;
        qDebug().noquote() << "[PlayerView] progress threshold reached, prefetching next episode"
                           << "| position:" << position
                           << "| threshold:" << m_prefetchThreshold << "%";
        launchTask(prefetchNextEpisodeSource(), this);
    }

    checkAndSkipSegment(position);

    if (!m_progressSlider->isSliderDown())
    {
        m_progressSlider->blockSignals(true);
        m_progressSlider->setValue(static_cast<int>(position));
        m_progressSlider->blockSignals(false);
    }

    m_currentTimeLabel->setText(formatTime(position, m_totalDuration));

    
    if (m_osdLayer && m_osdLayer->isSeekLineVisible())
    {
        if (m_osdSeekPreviewPosition < 0.0 || std::abs(position - m_osdSeekPreviewPosition) <= 1.0)
        {
            m_osdSeekPreviewPosition = position;
            m_osdLayer->updateSeekPosition(static_cast<int>(position), formatTime(position, m_totalDuration));
        }
    }
}

void PlayerView::onDurationChanged(double duration)
{
    if (m_isViewTearingDown)
    {
        return;
    }

    if (std::isnan(duration) || std::isinf(duration) || duration < 0)
    {
        duration = 0.0;
    }
    m_totalDuration = duration;
    m_progressSlider->setMaximum(static_cast<int>(duration));
    m_totalTimeLabel->setText(formatTime(duration, duration));

    

    if (!m_hasSetVideoSize && duration > 0)
    {
        m_hasSetVideoSize = true;

        
        double dw = m_mpvWidget->controller()->getProperty("video-params/dw").toDouble();
        double dh = m_mpvWidget->controller()->getProperty("video-params/dh").toDouble();
        if (dw <= 0 || dh <= 0)
        {
            dw = m_mpvWidget->controller()->getProperty("width").toDouble();
            dh = m_mpvWidget->controller()->getProperty("height").toDouble();
        }

        QWidget *win = window();
        
        
        if (win && !m_wasMaximized && !win->isFullScreen() && !qobject_cast<QMainWindow *>(win))
        {
            double aspect = (dw > 0 && dh > 0) ? (dw / dh) : (16.0 / 9.0);

            
            int playerW = this->width();
            if (playerW < 100)
            {
                playerW = win->width(); 
            }

            
            int targetPlayerH = qRound(playerW / aspect);

            
            if (playerW % 2 != 0)
            {
                playerW++;
            }
            if (targetPlayerH % 2 != 0)
            {
                targetPlayerH++;
            }

            
            int deltaH = targetPlayerH - this->height();

            
            int targetWinW = win->width() + (playerW - this->width());
            int targetWinH = win->height() + deltaH;

            QRect currentGeo = win->geometry();
            win->setGeometry(currentGeo.center().x() - targetWinW / 2, currentGeo.center().y() - targetWinH / 2,
                             targetWinW, targetWinH);
        }

        
        
        
        if (m_targetAudioStreamIndex != -2 || m_targetSubStreamIndex != -2)
        {
            QVariantList tracks = m_mpvWidget->controller()->getProperty("track-list").toList();

            
            if (m_targetAudioStreamIndex == -1)
            {
                m_mpvWidget->controller()->setProperty("aid", "no");
            }
            if (m_targetSubStreamIndex == -1)
            {
                m_mpvWidget->controller()->setProperty("sid", "no");
            }

            for (const QVariant &v : tracks)
            {
                QVariantMap map = v.toMap();
                QString type = map["type"].toString();
                int ffIndex = map["ff-index"].toInt();
                int mpvId = map["id"].toInt();

                if (type == "audio" && m_targetAudioStreamIndex >= 0 && ffIndex == m_targetAudioStreamIndex)
                {
                    m_mpvWidget->controller()->setProperty("aid", mpvId);
                }
                if (type == "sub" && m_targetSubStreamIndex >= 0 && ffIndex == m_targetSubStreamIndex)
                {
                    m_mpvWidget->controller()->setProperty("sid", mpvId);
                }
            }
        }

        
        QVariantList pendingSubs = property("pendingSubtitles").toList();
        for (const QVariant &v : pendingSubs)
        {
            QVariantMap map = v.toMap();
            m_mpvWidget->controller()->command(QVariantList{"sub-add", map["url"].toString(), map["flag"].toString(),
                                                            map["title"].toString(), map["lang"].toString()});
        }
        setProperty("pendingSubtitles", QVariantList()); 

        
        
        applyPersistedExternalSubtitleIfAny();
    }

    
    
    if (!m_windowRestorePending && m_pendingSeekSeconds > 0 && duration > 0)
    {
        m_mpvWidget->seek(m_pendingSeekSeconds);
        m_pendingSeekSeconds = 0.0;
    }

    // Trakt resume check: once per media, only when duration is known so the
    // stored percentage can be converted into a seek target.
    if (!m_traktResumeChecked && duration > 0)
    {
        m_traktResumeChecked = true;
        launchTask(traktCheckResumeProgress(), this);
    }
}

void PlayerView::onPlaybackStateChanged(bool isPaused)
{
    if (m_isViewTearingDown)
    {
        return;
    }

    m_isPlaying = !isPaused;
    updatePowerInhibition();
    m_playPauseBtn->setIcon(QIcon(m_isPlaying ? ":/svg/player/pause.svg" : ":/svg/player/play.svg"));

    
    showControls();

    if (!m_hasReportedStop && !m_currentMediaId.isEmpty() && !m_currentPlaySessionId.isEmpty())
    {
        long long currentTicks = static_cast<long long>(m_currentPosition * 10000000.0);
        m_core->mediaService()->reportPlaybackProgress(m_currentMediaId, m_currentMediaSourceId, currentTicks, isPaused,
                                                       m_currentPlaySessionId);
    }

    traktOnPauseStateChanged(isPaused);
}

void PlayerView::togglePlayPause()
{
    if (m_isPlaying)
    {
        m_mpvWidget->pause();
    }
    else if (m_isPlaybackFinished)
    {
        m_currentPosition = 0.0;
        m_mpvWidget->seek(0.0);
        if (m_progressSlider)
        {
            m_progressSlider->blockSignals(true);
            m_progressSlider->setValue(0);
            m_progressSlider->blockSignals(false);
        }
        m_currentTimeLabel->setText(formatTime(m_currentPosition, m_totalDuration));
        resumePlaybackAfterFinishedSeek();
    }
    else
    {
        m_mpvWidget->play();
    }

    showControls();
}

void PlayerView::onSliderMoved(int value)
{
    const bool shouldResumeFinishedPlayback =
        m_isPlaybackFinished && (!m_progressSlider || value < m_progressSlider->maximum());

    m_mpvWidget->seek(static_cast<double>(value));
    m_currentPosition = static_cast<double>(value);
    m_currentTimeLabel->setText(formatTime(m_currentPosition, m_totalDuration));

    if (shouldResumeFinishedPlayback)
    {
        resumePlaybackAfterFinishedSeek();
    }

    if (!m_hasReportedStop && !m_currentMediaId.isEmpty() && !m_currentPlaySessionId.isEmpty())
    {
        long long currentTicks = static_cast<long long>(value * 10000000.0);
        m_core->mediaService()->reportPlaybackProgress(m_currentMediaId, m_currentMediaSourceId, currentTicks,
                                                       !m_isPlaying, m_currentPlaySessionId);
    }

    showControls();
}

QString PlayerView::formatTime(double seconds, double totalSeconds) const
{
    QTime t(0, 0, 0);
    t = t.addSecs(static_cast<int>(seconds));
    QTime totalT(0, 0, 0);
    totalT = totalT.addSecs(static_cast<int>(totalSeconds));

    if (totalT.hour() > 0)
    {
        return t.toString("hh:mm:ss");
    }
    else
    {
        return t.toString("mm:ss");
    }
}
