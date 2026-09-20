#include "playerdanmakucontroller.h"

#include "../utils/qcoroutil.h"
#include "mpvwidget.h"
#include "nativedanmakuoverlay.h"
#include "../utils/danmakurendererutils.h"
#include "../utils/subtitlestyleutils.h"
#include <config/config_keys.h>
#include <config/configstore.h>
#include <qembycore.h>
#include <services/danmaku/danmakuservice.h>
#include <services/manager/servermanager.h>
#include <services/media/mediaservice.h>

#include <QCoreApplication>
#include <QDir>
#include <QDebug>
#include <QFileInfo>
#include <QTimer>
#include <QUrl>
#include <exception>

namespace {

constexpr auto kDanmakuTrackTitle = "[ReEmby] Danmaku";

QString contextTitle(const DanmakuMediaContext &context)
{
    const QString displayTitle = context.displayTitle().trimmed();
    return displayTitle.isEmpty() ? context.title.trimmed() : displayTitle;
}

QString dandanCredentialErrorMessage()
{
    return QCoreApplication::translate(
        "DandanplayProvider",
        "DandanPlay Open API now requires App ID and App Secret. Configure them in Danmaku Server settings.");
}

bool isDandanCredentialError(const QString &errorMessage)
{
    const QString trimmed = errorMessage.trimmed();
    return trimmed == dandanCredentialErrorMessage() ||
           trimmed == QStringLiteral(
               "DandanPlay Open API now requires App ID and App Secret. Configure them in Danmaku Server settings.");
}

QString danmakuEnabledConfigKey(const DanmakuMediaContext &context)
{
    const QString serverId = context.serverId.trimmed();
    if (serverId.isEmpty()) {
        return QString::fromLatin1(ConfigKeys::PlayerDanmakuEnabled);
    }
    return ConfigKeys::forServer(serverId, ConfigKeys::PlayerDanmakuEnabled);
}

bool readDanmakuEnabled(const DanmakuMediaContext &context)
{
    auto *store = ConfigStore::instance();
    const bool globalEnabled = store->get<bool>(ConfigKeys::PlayerDanmakuEnabled,
                                                false);
    return store->get<bool>(danmakuEnabledConfigKey(context), globalEnabled);
}

QString configuredVideoSyncMode()
{
    const QString mode = ConfigStore::instance()
                             ->get<QString>(ConfigKeys::PlayerVideoSync,
                                            QStringLiteral("display-resample"))
                             .trimmed();
    return mode.isEmpty() ? QStringLiteral("display-resample") : mode;
}

bool shouldInterpolationBeEnabled()
{
    const QString vsyncMode = configuredVideoSyncMode();
    return vsyncMode == QLatin1String("display-resample");
}

} 

PlayerDanmakuController::PlayerDanmakuController(QEmbyCore *core,
                                                 MpvWidget *mpvWidget,
                                                 NativeDanmakuOverlay *nativeDanmakuOverlay,
                                                 QObject *parent)
    : QObject(parent),
      m_core(core),
      m_mpvWidget(mpvWidget),
      m_nativeDanmakuOverlay(nativeDanmakuOverlay)
{
    if (!m_mpvWidget || !m_mpvWidget->controller()) {
        return;
    }

    if (m_nativeDanmakuOverlay && m_core && m_core->danmakuService()) {
        m_nativeDanmakuOverlay->setRenderOptions(
            m_core->danmakuService()->renderOptions());
        m_nativeDanmakuOverlay->setDanmakuVisible(false);
    }

    connect(m_mpvWidget->controller(), &MpvController::fileLoaded, this,
            [this]() {
                m_fileLoaded = true;
                syncSubtitleSelectionFromTrackList();
                updateDanmakuPresentation();
            });

    connect(m_mpvWidget->controller(), &MpvController::propertyChanged, this,
            [this](const QString &property, const QVariant &) {
                if (property == QLatin1String("track-list")) {
                    onTrackListChanged();
                }
            });

    connect(ConfigStore::instance(), &ConfigStore::valueChanged, this,
            [this](const QString &key, const QVariant &) {
                if (key == QLatin1String(ConfigKeys::PlayerDanmakuRenderer)) {
                    qDebug().noquote()
                        << "[Danmaku][Player] Renderer changed"
                        << "| renderer:" << DanmakuRendererUtils::normalizeRendererId(
                               ConfigStore::instance()->get<QString>(
                                   ConfigKeys::PlayerDanmakuRenderer,
                                   DanmakuRendererUtils::defaultRendererId()));
                    updateDanmakuPresentation();
                    emit stateChanged();
                } else if (key ==
                           QLatin1String(ConfigKeys::PlayerDanmakuDualSubtitle)) {
                    applyTrackSelection();
                    emit stateChanged();
                }
            });
}

void PlayerDanmakuController::setPlaybackContext(const PlayerLaunchContext &context)
{
    ++m_requestSerial;
    removeDanmakuTrack();
    m_launchContext = context;
    m_mediaContext = buildMediaContext(context);
    m_assFilePath.clear();
    m_commentPayload.clear();
    m_sourceTitle.clear();
    m_sourceProvider.clear();
    m_sourceServerId.clear();
    m_sourceServerName.clear();
    m_activeTargetId.clear();
    m_activeEndpointId.clear();
    m_commentCount = 0;
    m_selectedSubtitleTrackId = -1;
    // 副字幕选中轨一并复位；语言记忆 m_secondarySubtitleLang 刻意保留，
    // 切集后由 syncSecondarySubtitleSelectionFromTrackList 按语言自动恢复。
    m_secondarySubtitleTrackId = -1;
    m_fileLoaded = false;
    m_visible = true;
    m_loading = false;
    m_nativePayloadDirty = false;
    if (m_nativeDanmakuOverlay) {
        m_nativeDanmakuOverlay->clearDanmaku();
    }

    qDebug().noquote()
        << "[Danmaku][Player] Set playback context"
        << "| media:" << contextTitle(m_mediaContext)
        << "| mediaId:" << m_mediaContext.mediaId
        << "| sourceId:" << m_mediaContext.mediaSourceId
        << "| enabled:" << isDanmakuEnabled();

    if (!isDanmakuEnabled()) {
        emit stateChanged();
        return;
    }

    const bool autoLoad = ConfigStore::instance()->get<bool>(
        ConfigKeys::forServer(m_mediaContext.serverId, ConfigKeys::DanmakuAutoLoad),
        true);
    if (!autoLoad) {
        qDebug().noquote()
            << "[Danmaku][Player] Auto load disabled for current server"
            << "| serverId:" << m_mediaContext.serverId;
        emit stateChanged();
        return;
    }

    launchTask(loadDanmakuTask(m_requestSerial), this);
}

void PlayerDanmakuController::clearPlaybackContext()
{
    ++m_requestSerial;
    removeDanmakuTrack();
    m_launchContext = {};
    m_mediaContext = {};
    m_assFilePath.clear();
    m_commentPayload.clear();
    m_sourceTitle.clear();
    m_sourceProvider.clear();
    m_sourceServerId.clear();
    m_sourceServerName.clear();
    m_activeTargetId.clear();
    m_activeEndpointId.clear();
    m_commentCount = 0;
    m_selectedSubtitleTrackId = -1;
    // 副字幕选中轨一并复位；语言记忆 m_secondarySubtitleLang 刻意保留，
    // 切集后由 syncSecondarySubtitleSelectionFromTrackList 按语言自动恢复。
    m_secondarySubtitleTrackId = -1;
    m_fileLoaded = false;
    m_visible = true;
    m_loading = false;
    m_nativePayloadDirty = false;
    if (m_nativeDanmakuOverlay) {
        m_nativeDanmakuOverlay->clearDanmaku();
    }
    qDebug() << "[Danmaku][Player] Cleared playback context";
    emit stateChanged();
}

void PlayerDanmakuController::prepareForMediaReload()
{
    if (!hasPlaybackContext()) {
        return;
    }

    const bool hadTrack = m_danmakuTrackId > 0;
    const bool hasPreparedContent = hasPreparedDanmaku();

    
    
    
    removeDanmakuTrack();
    m_fileLoaded = false;
    m_selectedSubtitleTrackId = -1;
    // 副字幕轨复位（语言记忆保留，切集后按语言恢复）。
    m_secondarySubtitleTrackId = -1;
    if (m_nativeDanmakuOverlay) {
        m_nativeDanmakuOverlay->setDanmakuVisible(false);
    }

    qDebug().noquote()
        << "[Danmaku][Player] Prepared for media reload"
        << "| mediaId:" << m_mediaContext.mediaId
        << "| hadTrack:" << hadTrack
        << "| hasPreparedContent:" << hasPreparedContent
        << "| nativeRenderer:" << shouldUseNativeRenderer();
    emit stateChanged();
}

bool PlayerDanmakuController::isDanmakuEnabled() const
{
    return readDanmakuEnabled(m_mediaContext);
}

bool PlayerDanmakuController::isDanmakuVisible() const
{
    return isDanmakuEnabled() && m_visible;
}

bool PlayerDanmakuController::hasDanmakuTrack() const
{
    return m_danmakuTrackId > 0;
}

bool PlayerDanmakuController::hasPreparedDanmaku() const
{
    return !m_assFilePath.isEmpty() || !m_commentPayload.isEmpty();
}

bool PlayerDanmakuController::isLoading() const
{
    return m_loading;
}

bool PlayerDanmakuController::hasPlaybackContext() const
{
    return !m_mediaContext.mediaId.isEmpty();
}

QString PlayerDanmakuController::sourceTitle() const
{
    return m_sourceTitle;
}

QString PlayerDanmakuController::sourceProvider() const
{
    return m_sourceProvider;
}

QString PlayerDanmakuController::sourceServerId() const
{
    return m_sourceServerId;
}

QString PlayerDanmakuController::sourceServerName() const
{
    return m_sourceServerName;
}

int PlayerDanmakuController::commentCount() const
{
    return m_commentCount;
}

DanmakuMediaContext PlayerDanmakuController::mediaContext() const
{
    return m_mediaContext;
}

QString PlayerDanmakuController::activeTargetId() const
{
    return m_activeTargetId;
}

QString PlayerDanmakuController::activeEndpointId() const
{
    return m_activeEndpointId;
}

QList<QVariantMap> PlayerDanmakuController::rawSubtitleTracks() const
{
    // 原始读取：保留 mpv track-list 自带的所有字段（含 selected）。
    QList<QVariantMap> tracks;
    if (!m_mpvWidget || !m_mpvWidget->controller()) {
        return tracks;
    }

    const QVariantList trackList =
        m_mpvWidget->controller()->getProperty(QStringLiteral("track-list")).toList();
    for (const QVariant &value : trackList) {
        QVariantMap trackMap = value.toMap();
        if (trackMap.value(QStringLiteral("type")).toString() !=
            QLatin1String("sub")) {
            continue;
        }
        if (isDanmakuTrackMap(trackMap)) {
            continue;
        }
        tracks.append(trackMap);
    }
    return tracks;
}

QList<QVariantMap> PlayerDanmakuController::contentSubtitleTracks(
    bool forSecondary) const
{
    const int selectedTrackId =
        forSecondary ? m_secondarySubtitleTrackId : m_selectedSubtitleTrackId;
    QList<QVariantMap> tracks = rawSubtitleTracks();
    for (QVariantMap &trackMap : tracks) {
        // mpv 的 track-list 原始条目自带 selected 字段（当前播放中的轨），与
        // 菜单里的"用户选择"标记不是一回事——先清除，避免主/副字幕菜单的
        // 勾选互相混入（例如副字幕菜单里同时出现主字幕轨的勾、或多勾）。
        // 注意：需要读"实际在播的轨"的逻辑（syncSubtitleSelectionFromTrackList）
        // 必须走 rawSubtitleTracks()，不要用这个剥离过的列表。
        trackMap.remove(QStringLiteral("selected"));
        if (selectedTrackId > 0 &&
            trackMap.value(QStringLiteral("id")).toInt() == selectedTrackId) {
            trackMap.insert(QStringLiteral("selected"), true);
        }
    }
    return tracks;
}

void PlayerDanmakuController::selectSubtitleTrack(const QVariant &data)
{
    if (!m_mpvWidget || !m_mpvWidget->controller()) {
        return;
    }

    const QString valStr = data.toString();
    if (valStr == QLatin1String("no")) {
        m_selectedSubtitleTrackId = -1;
    } else {
        m_selectedSubtitleTrackId = data.toInt();
    }
    qDebug().noquote()
        << "[Danmaku][Player] Select subtitle track"
        << "| trackData:" << valStr
        << "| resolvedTrackId:" << m_selectedSubtitleTrackId;
    applyTrackSelection();
    emit stateChanged();
}

void PlayerDanmakuController::selectSecondarySubtitleTrack(const QVariant &data)
{
    if (!m_mpvWidget || !m_mpvWidget->controller()) {
        return;
    }

    const QString valStr = data.toString();
    if (valStr == QLatin1String("no")) {
        m_secondarySubtitleTrackId = -1;
        m_secondarySubtitleLang.clear();
    } else {
        m_secondarySubtitleTrackId = data.toInt();
        // 记住所选轨的语言：切集后 track id 会变化，靠语言重新匹配恢复。
        m_secondarySubtitleLang.clear();
        for (const QVariantMap &trackMap : contentSubtitleTracks(true)) {
            if (trackMap.value(QStringLiteral("id")).toInt() ==
                m_secondarySubtitleTrackId) {
                m_secondarySubtitleLang =
                    trackMap.value(QStringLiteral("lang")).toString();
                break;
            }
        }
    }
    qDebug().noquote()
        << "[Danmaku][Player] Select secondary subtitle track"
        << "| trackData:" << valStr
        << "| resolvedTrackId:" << m_secondarySubtitleTrackId
        << "| lang:" << m_secondarySubtitleLang;
    applyTrackSelection();
    emit stateChanged();
}

bool PlayerDanmakuController::secondarySubtitleBlockedByDanmaku() const
{
    // ass-track 弹幕占用 sid，secondary-sid 又要留给主字幕（dualSubtitle 开启
    // 时），两条字幕轨已满 → 副字幕无法再分配。native-smooth 弹幕走 Qt 层渲染、
    // 不占 mpv 字幕轨，因此不冲突。
    return isDanmakuVisible() && m_danmakuTrackId > 0 &&
           !shouldUseNativeRenderer();
}

void PlayerDanmakuController::refreshTrackSelection()
{
    applyTrackSelection();
}

void PlayerDanmakuController::setDanmakuEnabled(bool enabled)
{
    const QString enabledKey = danmakuEnabledConfigKey(m_mediaContext);
    ConfigStore::instance()->set(enabledKey, enabled);
    
    m_visible = enabled;
    qDebug().noquote()
        << "[Danmaku][Player] Toggle danmaku"
        << "| configKey:" << enabledKey
        << "| serverId:" << m_mediaContext.serverId
        << "| enabled:" << enabled
        << "| hasAssFile:" << !m_assFilePath.isEmpty()
        << "| commentPayload:" << m_commentPayload.size()
        << "| fileLoaded:" << m_fileLoaded;
    if (!enabled) {
        updateDanmakuPresentation();
        emit toastRequested(tr("Danmaku Disabled"));
        emit stateChanged();
        return;
    }

    if (!hasPreparedDanmaku()) {
        reload();
        return;
    }

    updateDanmakuPresentation();
    emit toastRequested(tr("Danmaku Enabled"));
    emit stateChanged();
}

void PlayerDanmakuController::setDanmakuVisible(bool visible)
{
    if (m_visible == visible) {
        return;
    }
    m_visible = visible;
    qDebug().noquote()
        << "[Danmaku][Player] Toggle danmaku visibility"
        << "| visible:" << visible
        << "| hasTrack:" << (m_danmakuTrackId > 0)
        << "| nativeRenderer:" << shouldUseNativeRenderer();
    updateDanmakuPresentation();
    emit toastRequested(visible ? tr("Danmaku Shown") : tr("Danmaku Hidden"));
    emit stateChanged();
}

void PlayerDanmakuController::reload(const QString &manualKeyword)
{
    if (m_mediaContext.mediaId.isEmpty()) {
        return;
    }

    const QString trimmedKeyword = manualKeyword.trimmed();
    if (!trimmedKeyword.isEmpty() && !isDanmakuEnabled()) {
        ConfigStore::instance()->set(danmakuEnabledConfigKey(m_mediaContext), true);
    }

    ++m_requestSerial;
    removeDanmakuTrack();
    m_assFilePath.clear();
    m_commentPayload.clear();
    m_sourceTitle.clear();
    m_sourceProvider.clear();
    m_sourceServerId.clear();
    m_sourceServerName.clear();
    m_activeTargetId.clear();
    m_activeEndpointId.clear();
    m_commentCount = 0;
    m_loading = false;
    m_nativePayloadDirty = false;
    if (m_nativeDanmakuOverlay) {
        m_nativeDanmakuOverlay->clearDanmaku();
    }
    qDebug().noquote()
        << "[Danmaku][Player] Reload requested"
        << "| mediaId:" << m_mediaContext.mediaId
        << "| manualKeyword:" << trimmedKeyword
        << "| enabled:" << isDanmakuEnabled();
    emit stateChanged();
    launchTask(loadDanmakuTask(m_requestSerial, trimmedKeyword), this);
}

void PlayerDanmakuController::loadFromCandidate(
    DanmakuMatchCandidate candidate, bool saveAsManualMatch)
{
    if (m_mediaContext.mediaId.isEmpty() || !candidate.isValid()) {
        return;
    }

    if (!isDanmakuEnabled()) {
        ConfigStore::instance()->set(danmakuEnabledConfigKey(m_mediaContext), true);
    }

    ++m_requestSerial;
    removeDanmakuTrack();
    m_assFilePath.clear();
    m_commentPayload.clear();
    m_sourceTitle.clear();
    m_sourceProvider.clear();
    m_sourceServerId.clear();
    m_sourceServerName.clear();
    m_activeTargetId.clear();
    m_activeEndpointId.clear();
    m_commentCount = 0;
    m_loading = false;
    m_nativePayloadDirty = false;
    if (m_nativeDanmakuOverlay) {
        m_nativeDanmakuOverlay->clearDanmaku();
    }
    qDebug().noquote()
        << "[Danmaku][Player] Load candidate requested"
        << "| mediaId:" << m_mediaContext.mediaId
        << "| provider:" << candidate.provider
        << "| endpointId:" << candidate.endpointId
        << "| endpointName:" << candidate.endpointName
        << "| targetId:" << candidate.targetId
        << "| saveManualMatch:" << saveAsManualMatch;
    emit stateChanged();
    launchTask(loadDanmakuCandidateTask(m_requestSerial, candidate,
                                        saveAsManualMatch), this);
}

void PlayerDanmakuController::loadLocalFile(QString filePath)
{
    if (!m_core || !m_core->danmakuService()) {
        return;
    }

    const DanmakuMatchCandidate candidate =
        m_core->danmakuService()->createLocalFileCandidate(filePath.trimmed());
    if (!candidate.isValid()) {
        emit toastRequested(tr("Unsupported danmaku file"));
        return;
    }

    loadFromCandidate(candidate, true);
}

DanmakuMediaContext PlayerDanmakuController::buildMediaContext(
    const PlayerLaunchContext &context) const
{
    DanmakuMediaContext mediaContext;
    if (!m_core || !m_core->serverManager()) {
        return mediaContext;
    }

    const MediaItem &item = context.mediaItem;
    const MediaSourceInfo &source = context.selectedSource;
    mediaContext.serverId = m_core->serverManager()->activeProfile().id;
    mediaContext.mediaId = item.id;
    mediaContext.mediaSourceId = source.id;
    mediaContext.itemType = item.type;
    mediaContext.title = item.name;
    mediaContext.originalTitle = item.originalTitle;
    mediaContext.seriesName = item.seriesName;
    mediaContext.parentSeriesId = item.seriesId;
    mediaContext.productionYear = item.productionYear;
    mediaContext.seasonNumber = item.parentIndexNumber;
    mediaContext.episodeNumber = item.indexNumber;
    mediaContext.durationMs =
        (source.runTimeTicks > 0 ? source.runTimeTicks : item.runTimeTicks) / 10000;
    mediaContext.path = !source.path.isEmpty() ? source.path : item.path;
    QString normalizedMediaPath = mediaContext.path;
    const QUrl mediaPathUrl = QUrl::fromUserInput(normalizedMediaPath);
    if (mediaPathUrl.scheme().compare(QStringLiteral("http"),
                                      Qt::CaseInsensitive) == 0 ||
        mediaPathUrl.scheme().compare(QStringLiteral("https"),
                                      Qt::CaseInsensitive) == 0) {
        normalizedMediaPath = mediaPathUrl.path();
    }
    normalizedMediaPath.replace(QLatin1Char('\\'), QLatin1Char('/'));
    mediaContext.fileName =
        QFileInfo(normalizedMediaPath).completeBaseName().trimmed();
    if (mediaContext.fileName.isEmpty() && !item.name.trimmed().isEmpty()) {
        mediaContext.fileName = item.name.trimmed();
    }
    mediaContext.fileSize = source.size > 0 ? source.size : item.size;
    mediaContext.mediaModifiedAt = source.dateModified;
    mediaContext.genres = item.genres;
    if (m_core->mediaService() && !item.id.isEmpty() && !source.id.isEmpty()) {
        mediaContext.mediaUrl =
            m_core->mediaService()->getStreamUrl(item.id, source, item.serverId);
    }
    mediaContext.providerIds = item.providerIds;
    return mediaContext;
}

bool PlayerDanmakuController::isDanmakuTrackMap(const QVariantMap &trackMap) const
{
    const QString title = trackMap.value(QStringLiteral("title")).toString();
    if (title == QLatin1String(kDanmakuTrackTitle)) {
        return true;
    }

    const QString externalFilename =
        trackMap.value(QStringLiteral("external-filename")).toString();
    return !m_assFilePath.isEmpty() &&
           QDir::fromNativeSeparators(externalFilename) ==
               QDir::fromNativeSeparators(m_assFilePath);
}

void PlayerDanmakuController::onTrackListChanged()
{
    syncSubtitleSelectionFromTrackList();
    syncSecondarySubtitleSelectionFromTrackList();
    refreshDanmakuTrackId(0);
    // 副字幕启用且已选中时也要重分配——无弹幕场景不满足原来两个条件，
    // 否则切集后 secondary-sid 不会被重新写入。
    const bool secondaryActive =
        m_secondarySubtitleTrackId > 0 &&
        ConfigStore::instance()->get<bool>(
            ConfigKeys::PlayerSubtitleSecondaryEnabled, false);
    if ((isDanmakuVisible() && m_danmakuTrackId > 0) ||
        shouldUseNativeRenderer() || secondaryActive) {
        applyTrackSelection();
    }
}

void PlayerDanmakuController::syncSubtitleSelectionFromTrackList()
{
    if (!m_mpvWidget || !m_mpvWidget->controller()) {
        return;
    }
    const int previousTrackId = m_selectedSubtitleTrackId;

    // 从 mpv 的字幕通道（sid / secondary-sid）读取"实际在播的内容主字幕"。
    // 不要用 track-list 的 selected 字段：菜单展示路径
    // （contentSubtitleTracks）会剥离它并重插"用户选择"，用它会导致同步
    // 读到自己刚写回的值（永远同步不到 mpv 的真实状态）。
    //
    // 通道语义：ass-track 弹幕模式下弹幕占用 sid，内容主字幕物理上位于
    // secondary-sid；native-smooth 双字幕场景下主字幕在 sid、副字幕在
    // secondary-sid——优先取 sid 命中的内容字幕轨，其次取 secondary-sid。
    const auto readChannelTrackId = [this](const char *name) -> int {
        const QVariant value =
            m_mpvWidget->controller()->getProperty(QString::fromLatin1(name));
        bool ok = false;
        const int id = value.toInt(&ok);
        return ok ? id : -1;
    };
    const int sidTrackId = readChannelTrackId("sid");
    const int secondaryTrackId = readChannelTrackId("secondary-sid");
    const QList<QVariantMap> tracks = rawSubtitleTracks();

    int resolved = -1;
    if (sidTrackId > 0) {
        for (const QVariantMap &trackMap : tracks) {
            if (trackMap.value(QStringLiteral("id")).toInt() == sidTrackId) {
                resolved = sidTrackId;
                break;
            }
        }
    }
    if (resolved <= 0 && secondaryTrackId > 0) {
        for (const QVariantMap &trackMap : tracks) {
            if (trackMap.value(QStringLiteral("id")).toInt() == secondaryTrackId) {
                resolved = secondaryTrackId;
                break;
            }
        }
    }
    // 两个通道都没有内容字幕（显式关闭字幕、或切集瞬间 track-list 尚未
    // 重建）时，保留仍然存在的上一次选择。
    if (resolved <= 0 && previousTrackId > 0) {
        for (const QVariantMap &trackMap : tracks) {
            if (trackMap.value(QStringLiteral("id")).toInt() == previousTrackId) {
                resolved = previousTrackId;
                break;
            }
        }
    }
    m_selectedSubtitleTrackId = resolved;
}

void PlayerDanmakuController::syncSecondarySubtitleSelectionFromTrackList()
{
    if (m_secondarySubtitleLang.isEmpty()) {
        return;
    }

    const QList<QVariantMap> tracks = contentSubtitleTracks(true);

    // 当前选择仍然有效（track id 还在列表里）→ 保持不动，避免覆盖用户的手动选择。
    for (const QVariantMap &trackMap : tracks) {
        if (trackMap.value(QStringLiteral("id")).toInt() ==
            m_secondarySubtitleTrackId) {
            return;
        }
    }

    // 切集后 track id 已失效：按记住的语言重新匹配。
    m_secondarySubtitleTrackId = -1;
    for (const QVariantMap &trackMap : tracks) {
        if (trackMap.value(QStringLiteral("lang")).toString().compare(
                m_secondarySubtitleLang, Qt::CaseInsensitive) == 0) {
            m_secondarySubtitleTrackId =
                trackMap.value(QStringLiteral("id")).toInt();
            qDebug().noquote()
                << "[Danmaku][Player] Restore secondary subtitle by lang"
                << "| lang:" << m_secondarySubtitleLang
                << "| trackId:" << m_secondarySubtitleTrackId;
            break;
        }
    }
}

void PlayerDanmakuController::attachDanmakuTrack()
{
    if (shouldUseNativeRenderer()) {
        applyTrackSelection();
        return;
    }

    if (!m_fileLoaded || m_assFilePath.isEmpty() || !m_mpvWidget ||
        !m_mpvWidget->controller()) {
        return;
    }

    removeDanmakuTrack();
    qDebug().noquote()
        << "[Danmaku][Player] Attach danmaku track"
        << "| path:" << m_assFilePath;
    m_mpvWidget->controller()->command(QVariantList{
        QStringLiteral("sub-add"),
        QDir::toNativeSeparators(m_assFilePath),
        QStringLiteral("auto"),
        QString::fromLatin1(kDanmakuTrackTitle),
        QStringLiteral("qdm")});
    refreshDanmakuTrackId(5);
}

void PlayerDanmakuController::refreshDanmakuTrackId(int remainingRetries)
{
    if (!m_mpvWidget || !m_mpvWidget->controller()) {
        return;
    }

    const QVariantList trackList =
        m_mpvWidget->controller()->getProperty(QStringLiteral("track-list")).toList();
    for (const QVariant &value : trackList) {
        const QVariantMap trackMap = value.toMap();
        if (trackMap.value(QStringLiteral("type")).toString() !=
            QLatin1String("sub")) {
            continue;
        }
        if (!isDanmakuTrackMap(trackMap)) {
            continue;
        }

        bool ok = false;
        const int trackId =
            trackMap.value(QStringLiteral("id")).toInt(&ok);
        m_danmakuTrackId = ok ? trackId : -1;
        qDebug().noquote()
            << "[Danmaku][Player] Danmaku track discovered"
            << "| trackId:" << m_danmakuTrackId
            << "| title:" << trackMap.value(QStringLiteral("title")).toString();
        applyTrackSelection();
        emit stateChanged();
        return;
    }

    if (remainingRetries > 0) {
        QTimer::singleShot(80, this, [this, remainingRetries]() {
            refreshDanmakuTrackId(remainingRetries - 1);
        });
    }
}

void PlayerDanmakuController::removeDanmakuTrack()
{
    if (!m_mpvWidget || !m_mpvWidget->controller()) {
        m_danmakuTrackId = -1;
        m_motionStabilityProfileApplied = false;
        return;
    }

    if (m_danmakuTrackId > 0) {
        qDebug().noquote()
            << "[Danmaku][Player] Remove danmaku track"
            << "| trackId:" << m_danmakuTrackId;
        m_mpvWidget->controller()->command(
            QVariantList{QStringLiteral("sub-remove"), m_danmakuTrackId});
    }
    m_danmakuTrackId = -1;
    clearDanmakuMotionStabilityProfile();
}

bool PlayerDanmakuController::prefersNativeRenderer() const
{
    return m_nativeDanmakuOverlay &&
           DanmakuRendererUtils::isNativeRenderer(
               ConfigStore::instance()->get<QString>(
                   ConfigKeys::PlayerDanmakuRenderer,
                   DanmakuRendererUtils::defaultRendererId()));
}

bool PlayerDanmakuController::shouldUseNativeRenderer() const
{
    return prefersNativeRenderer() && !m_commentPayload.isEmpty();
}

void PlayerDanmakuController::updateDanmakuPresentation()
{
    const bool nativeRendererPreferred = prefersNativeRenderer();
    if (nativeRendererPreferred && m_nativeDanmakuOverlay && m_core &&
        m_core->danmakuService()) {
        m_nativeDanmakuOverlay->setRenderOptions(
            m_core->danmakuService()->renderOptions());
        if (m_nativePayloadDirty) {
            if (!m_commentPayload.isEmpty()) {
                m_nativeDanmakuOverlay->setComments(m_commentPayload);
            } else {
                m_nativeDanmakuOverlay->clearDanmaku();
            }
            m_nativePayloadDirty = false;
        }
    } else if (m_nativeDanmakuOverlay && m_nativePayloadDirty &&
               m_commentPayload.isEmpty()) {
        m_nativeDanmakuOverlay->clearDanmaku();
        m_nativePayloadDirty = false;
    }

    if (shouldUseNativeRenderer()) {
        if (m_danmakuTrackId > 0) {
            removeDanmakuTrack();
        } else {
            clearDanmakuMotionStabilityProfile();
        }
        applyTrackSelection();
        return;
    }

    if (m_nativeDanmakuOverlay) {
        m_nativeDanmakuOverlay->setDanmakuVisible(false);
    }

    if (isDanmakuVisible() && m_fileLoaded && !m_assFilePath.isEmpty() &&
        m_danmakuTrackId <= 0) {
        attachDanmakuTrack();
        return;
    }

    applyTrackSelection();
}

void PlayerDanmakuController::applyDanmakuMotionStabilityProfile()
{
    if (!m_mpvWidget || !m_mpvWidget->controller()) {
        return;
    }

    auto *controller = m_mpvWidget->controller();
    auto setOptionWithLog = [controller](const QString &name,
                                         const QVariant &value) {
        const int err = controller->setProperty(name, value);
        if (err < 0) {
            qWarning().noquote()
                << "[Danmaku][Player] Failed to set MPV option"
                << "| option:" << name
                << "| value:" << value
                << "| error:" << err;
        }
    };
    setOptionWithLog(QStringLiteral("video-sync"), QStringLiteral("audio"));
    setOptionWithLog(QStringLiteral("interpolation"), false);
    setOptionWithLog(QStringLiteral("blend-subtitles"), QStringLiteral("no"));
    setOptionWithLog(QStringLiteral("sub-hinting"), QStringLiteral("none"));
    setOptionWithLog(QStringLiteral("sub-ass-hinting"), QStringLiteral("none"));
    setOptionWithLog(QStringLiteral("sub-blur"), 0.0);
    setOptionWithLog(QStringLiteral("sub-gauss"), 0.0);

    if (!m_motionStabilityProfileApplied) {
        qDebug().noquote()
            << "[Danmaku][Player] Applied motion stability profile"
            << "| video-sync: audio"
            << "| interpolation: off"
            << "| blend-subtitles: no"
            << "| sub-hinting: none"
            << "| sub-ass-hinting: none"
            << "| sub-blur: 0"
            << "| sub-gauss: 0";
    }
    m_motionStabilityProfileApplied = true;
}

void PlayerDanmakuController::clearDanmakuMotionStabilityProfile()
{
    if (!m_mpvWidget || !m_mpvWidget->controller()) {
        m_motionStabilityProfileApplied = false;
        return;
    }

    auto *controller = m_mpvWidget->controller();
    auto setOptionWithLog = [controller](const QString &name,
                                         const QVariant &value) {
        const int err = controller->setProperty(name, value);
        if (err < 0) {
            qWarning().noquote()
                << "[Danmaku][Player] Failed to restore MPV option"
                << "| option:" << name
                << "| value:" << value
                << "| error:" << err;
        }
    };
    const QString vsyncMode = configuredVideoSyncMode();
    const bool interpolationEnabled = shouldInterpolationBeEnabled();
    setOptionWithLog(QStringLiteral("video-sync"), vsyncMode);
    setOptionWithLog(QStringLiteral("interpolation"), interpolationEnabled);
    // mpv 默认值即 no；blend-subtitles=video 在 libmpv render API + 硬解 copy
    // + 10-bit 组合下会静默吞掉字幕，恢复路径同样不能设回 video
    setOptionWithLog(QStringLiteral("blend-subtitles"), QStringLiteral("no"));
    setOptionWithLog(QStringLiteral("sub-hinting"), QStringLiteral("none"));
    setOptionWithLog(QStringLiteral("sub-ass-hinting"), QStringLiteral("none"));
    setOptionWithLog(QStringLiteral("sub-blur"), 0.0);
    setOptionWithLog(QStringLiteral("sub-gauss"), 0.0);

    if (m_motionStabilityProfileApplied) {
        qDebug().noquote()
            << "[Danmaku][Player] Restored default motion profile"
            << "| video-sync:" << vsyncMode
            << "| interpolation:" << interpolationEnabled
            << "| blend-subtitles: no"
            << "| sub-hinting: none"
            << "| sub-ass-hinting: none"
            << "| sub-blur: 0"
            << "| sub-gauss: 0";
    }
    m_motionStabilityProfileApplied = false;
}

void PlayerDanmakuController::applyTrackSelection()
{
    if (!m_mpvWidget || !m_mpvWidget->controller()) {
        return;
    }

    const bool dualSubtitle = ConfigStore::instance()->get<bool>(
        ConfigKeys::PlayerDanmakuDualSubtitle, true);
    const bool useNativeRenderer =
        shouldUseNativeRenderer() && isDanmakuVisible();

    // 副字幕（第二条内容字幕）：需全局开关开启且已选中轨道才参与分配。
    const bool secondaryEnabled = ConfigStore::instance()->get<bool>(
        ConfigKeys::PlayerSubtitleSecondaryEnabled, false);
    const int secondaryTrackId =
        (secondaryEnabled && m_secondarySubtitleTrackId > 0)
            ? m_secondarySubtitleTrackId
            : -1;

    if (m_nativeDanmakuOverlay) {
        m_nativeDanmakuOverlay->setBottomSubtitleProtected(
            useNativeRenderer && dualSubtitle && m_selectedSubtitleTrackId > 0);
        m_nativeDanmakuOverlay->setDanmakuVisible(useNativeRenderer);
    }

    if (useNativeRenderer) {
        // native-smooth 弹幕走 Qt overlay、不占 mpv 字幕轨 → sid 给主字幕、
        // secondary-sid 给副字幕，实现「主副双字幕 + 弹幕」共存。
        qDebug().noquote()
            << "[Danmaku][Player] Apply native renderer selection"
            << "| contentSubtitleTrackId:" << m_selectedSubtitleTrackId
            << "| secondarySubtitleTrackId:" << secondaryTrackId
            << "| dualSubtitle:" << dualSubtitle
            << "| commentCount:" << m_commentPayload.size();
        if (secondaryTrackId > 0) {
            m_mpvWidget->controller()->setProperty(QStringLiteral("secondary-sid"),
                                                   secondaryTrackId);
        } else {
            m_mpvWidget->controller()->setProperty(QStringLiteral("secondary-sid"),
                                                   QStringLiteral("no"));
        }
        if (dualSubtitle && m_selectedSubtitleTrackId > 0) {
            m_mpvWidget->controller()->setProperty(QStringLiteral("sid"),
                                                   m_selectedSubtitleTrackId);
        } else {
            m_mpvWidget->controller()->setProperty(QStringLiteral("sid"),
                                                   QStringLiteral("no"));
        }
        clearDanmakuMotionStabilityProfile();
        SubtitleStyleUtils::applyToController(m_mpvWidget->controller(), false);
        return;
    }

    if (isDanmakuVisible() && m_danmakuTrackId > 0) {
        // ass-track 弹幕占用 sid（弹幕本身就是一条 ASS 字幕轨），secondary-sid
        // 留给主字幕；此场景下副字幕无法分配（2 条字幕轨已满），菜单侧在选择
        // 副字幕时会提示改用 native-smooth 弹幕渲染。
        qDebug().noquote()
            << "[Danmaku][Player] Apply dual subtitle selection"
            << "| danmakuTrackId:" << m_danmakuTrackId
            << "| contentSubtitleTrackId:" << m_selectedSubtitleTrackId
            << "| secondarySubtitleTrackId:" << secondaryTrackId
            << "| dualSubtitle:" << dualSubtitle
            << "| secondaryBlocked:" << (secondaryTrackId > 0);
        m_mpvWidget->controller()->setProperty(QStringLiteral("sid"),
                                               m_danmakuTrackId);
        if (dualSubtitle && m_selectedSubtitleTrackId > 0) {
            m_mpvWidget->controller()->setProperty(QStringLiteral("secondary-sub-pos"),
                                                   92);
            m_mpvWidget->controller()->setProperty(QStringLiteral("secondary-sid"),
                                                   m_selectedSubtitleTrackId);
        } else {
            m_mpvWidget->controller()->setProperty(QStringLiteral("secondary-sid"),
                                                   QStringLiteral("no"));
        }
        applyDanmakuMotionStabilityProfile();
        SubtitleStyleUtils::applyToController(m_mpvWidget->controller(), true);
        return;
    }

    // 无弹幕：sid = 主字幕，secondary-sid = 副字幕。
    qDebug().noquote()
        << "[Danmaku][Player] Apply regular subtitle selection"
        << "| contentSubtitleTrackId:" << m_selectedSubtitleTrackId
        << "| secondarySubtitleTrackId:" << secondaryTrackId;
    if (secondaryTrackId > 0) {
        m_mpvWidget->controller()->setProperty(QStringLiteral("secondary-sid"),
                                               secondaryTrackId);
    } else {
        m_mpvWidget->controller()->setProperty(QStringLiteral("secondary-sid"),
                                               QStringLiteral("no"));
    }
    if (m_selectedSubtitleTrackId > 0) {
        m_mpvWidget->controller()->setProperty(QStringLiteral("sid"),
                                               m_selectedSubtitleTrackId);
    } else {
        m_mpvWidget->controller()->setProperty(QStringLiteral("sid"),
                                               QStringLiteral("no"));
    }
    clearDanmakuMotionStabilityProfile();
    SubtitleStyleUtils::applyToController(m_mpvWidget->controller(), false);
}

QCoro::Task<void> PlayerDanmakuController::loadDanmakuTask(quint64 requestId,
                                                           QString manualKeyword)
{
    QPointer<PlayerDanmakuController> safeThis(this);
    QPointer<QEmbyCore> core(m_core);
    const DanmakuMediaContext mediaContext = m_mediaContext;
    const QString trimmedKeyword = manualKeyword.trimmed();
    if (!core || !core->danmakuService() || mediaContext.mediaId.isEmpty()) {
        co_return;
    }

    m_loading = true;
    emit stateChanged();
    qDebug().noquote()
        << "[Danmaku][Player] Load task start"
        << "| media:" << contextTitle(mediaContext)
        << "| mediaId:" << mediaContext.mediaId
        << "| manualKeyword:" << trimmedKeyword;

    try {
        DanmakuLoadResult result =
            co_await core->danmakuService()->prepareDanmaku(mediaContext,
                                                            trimmedKeyword);
        if (!safeThis || !safeThis->m_core || requestId != safeThis->m_requestSerial) {
            co_return;
        }

        safeThis->m_loading = false;
        safeThis->m_assFilePath = result.assFilePath;
        safeThis->m_commentPayload = result.comments;
        safeThis->m_nativePayloadDirty = !result.comments.isEmpty();
        safeThis->m_sourceTitle = result.sourceTitle;
        safeThis->m_sourceProvider = result.provider;
        safeThis->m_sourceServerId = result.sourceServerId;
        safeThis->m_sourceServerName = result.sourceServerName;
        safeThis->m_activeTargetId = result.matchResult.selected.targetId;
        safeThis->m_activeEndpointId =
            result.sourceServerId.trimmed().isEmpty()
                ? result.matchResult.selected.endpointId
                : result.sourceServerId;
        safeThis->m_commentCount = result.commentCount;
        qDebug().noquote()
            << "[Danmaku][Player] Load task finished"
            << "| mediaId:" << mediaContext.mediaId
            << "| success:" << result.success
            << "| commentCount:" << result.commentCount
            << "| endpointId:" << result.sourceServerId
            << "| endpointName:" << result.sourceServerName
            << "| needManualMatch:" << result.needManualMatch
            << "| assPath:" << result.assFilePath
            << "| nativePreferred:" << safeThis->prefersNativeRenderer()
            << "| payloadCount:" << result.comments.size();

        if (result.hasRenderableContent()) {
            const QString sourceServerName = result.sourceServerName.trimmed();
            if (!sourceServerName.isEmpty() && result.commentCount > 0) {
                emit safeThis->toastRequested(
                    tr("Danmaku Loaded from %1 (%2)")
                        .arg(sourceServerName)
                        .arg(result.commentCount));
            } else if (!sourceServerName.isEmpty()) {
                emit safeThis->toastRequested(
                    tr("Danmaku Loaded from %1").arg(sourceServerName));
            } else if (result.commentCount > 0) {
                emit safeThis->toastRequested(
                    tr("Danmaku Loaded (%1)").arg(result.commentCount));
            } else {
                emit safeThis->toastRequested(tr("Danmaku Loaded"));
            }
            safeThis->updateDanmakuPresentation();
        } else if (result.needManualMatch) {
            emit safeThis->toastRequested(tr("No matching danmaku found"));
        } else {
            emit safeThis->toastRequested(tr("No danmaku available"));
        }
        emit safeThis->stateChanged();
    } catch (const std::exception &e) {
        if (!safeThis || requestId != safeThis->m_requestSerial) {
            co_return;
        }
        safeThis->m_loading = false;
        const QString errorMessage = QString::fromUtf8(e.what()).trimmed();
        safeThis->m_sourceProvider.clear();
        safeThis->m_sourceServerId.clear();
        safeThis->m_sourceServerName.clear();
        safeThis->m_commentCount = 0;
        qWarning().noquote()
            << "[Danmaku][Player] Load task failed"
            << "| mediaId:" << mediaContext.mediaId
            << "| manualKeyword:" << trimmedKeyword
            << "| error:" << e.what();
        emit safeThis->toastRequested(
            isDandanCredentialError(errorMessage)
                ? errorMessage
                : tr("Failed to load danmaku"));
        emit safeThis->stateChanged();
    }
}

QCoro::Task<void> PlayerDanmakuController::loadDanmakuCandidateTask(
    quint64 requestId, DanmakuMatchCandidate candidate, bool saveAsManualMatch)
{
    QPointer<PlayerDanmakuController> safeThis(this);
    QPointer<QEmbyCore> core(m_core);
    const DanmakuMediaContext mediaContext = m_mediaContext;
    if (!core || !core->danmakuService() || mediaContext.mediaId.isEmpty() ||
        !candidate.isValid()) {
        co_return;
    }

    m_loading = true;
    emit stateChanged();
    qDebug().noquote()
        << "[Danmaku][Player] Candidate load task start"
        << "| media:" << contextTitle(mediaContext)
        << "| mediaId:" << mediaContext.mediaId
        << "| provider:" << candidate.provider
        << "| endpointId:" << candidate.endpointId
        << "| endpointName:" << candidate.endpointName
        << "| targetId:" << candidate.targetId
        << "| saveManualMatch:" << saveAsManualMatch;

    try {
        DanmakuLoadResult result =
            co_await core->danmakuService()->prepareDanmakuForCandidate(
                mediaContext, candidate);
        if (!safeThis || !safeThis->m_core ||
            requestId != safeThis->m_requestSerial) {
            co_return;
        }

        safeThis->m_loading = false;
        safeThis->m_assFilePath = result.assFilePath;
        safeThis->m_commentPayload = result.comments;
        safeThis->m_nativePayloadDirty = !result.comments.isEmpty();
        safeThis->m_sourceTitle = result.sourceTitle;
        safeThis->m_sourceProvider = result.provider;
        safeThis->m_sourceServerId = result.sourceServerId;
        safeThis->m_sourceServerName = result.sourceServerName;
        safeThis->m_activeTargetId = candidate.targetId;
        safeThis->m_activeEndpointId =
            result.sourceServerId.trimmed().isEmpty()
                ? candidate.endpointId
                : result.sourceServerId;
        safeThis->m_commentCount = result.commentCount;
        qDebug().noquote()
            << "[Danmaku][Player] Candidate load task finished"
            << "| mediaId:" << mediaContext.mediaId
            << "| success:" << result.success
            << "| commentCount:" << result.commentCount
            << "| provider:" << result.provider
            << "| endpointId:" << result.sourceServerId
            << "| endpointName:" << result.sourceServerName
            << "| assPath:" << result.assFilePath
            << "| nativePreferred:" << safeThis->prefersNativeRenderer()
            << "| payloadCount:" << result.comments.size();

        if (result.hasRenderableContent()) {
            if (saveAsManualMatch) {
                DanmakuMatchCandidate savedCandidate = candidate;
                if (savedCandidate.endpointId.trimmed().isEmpty()) {
                    savedCandidate.endpointId = result.sourceServerId;
                }
                if (savedCandidate.endpointName.trimmed().isEmpty()) {
                    savedCandidate.endpointName = result.sourceServerName;
                }
                core->danmakuService()->saveManualMatch(mediaContext,
                                                        savedCandidate);
            }
            const QString sourceServerName = result.sourceServerName.trimmed();
            if (!sourceServerName.isEmpty() && result.commentCount > 0) {
                emit safeThis->toastRequested(
                    tr("Danmaku Loaded from %1 (%2)")
                        .arg(sourceServerName)
                        .arg(result.commentCount));
            } else if (!sourceServerName.isEmpty()) {
                emit safeThis->toastRequested(
                    tr("Danmaku Loaded from %1").arg(sourceServerName));
            } else {
                emit safeThis->toastRequested(
                    result.commentCount > 0
                        ? tr("Danmaku Loaded (%1)").arg(result.commentCount)
                        : tr("Danmaku Loaded"));
            }
            safeThis->updateDanmakuPresentation();
        } else {
            emit safeThis->toastRequested(tr("No danmaku available"));
        }
        emit safeThis->stateChanged();
    } catch (const std::exception &e) {
        if (!safeThis || requestId != safeThis->m_requestSerial) {
            co_return;
        }

        safeThis->m_loading = false;
        safeThis->m_sourceProvider.clear();
        safeThis->m_sourceServerId.clear();
        safeThis->m_sourceServerName.clear();
        safeThis->m_commentCount = 0;
        const QString errorMessage = QString::fromUtf8(e.what()).trimmed();
        qWarning().noquote()
            << "[Danmaku][Player] Candidate load task failed"
            << "| mediaId:" << mediaContext.mediaId
            << "| provider:" << candidate.provider
            << "| endpointId:" << candidate.endpointId
            << "| endpointName:" << candidate.endpointName
            << "| targetId:" << candidate.targetId
            << "| error:" << e.what();
        emit safeThis->toastRequested(
            isDandanCredentialError(errorMessage)
                ? errorMessage
                : tr("Failed to load danmaku"));
        emit safeThis->stateChanged();
    }
}
