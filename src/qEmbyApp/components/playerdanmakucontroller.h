#ifndef PLAYERDANMAKUCONTROLLER_H
#define PLAYERDANMAKUCONTROLLER_H

#include <QObject>
#include <QPointer>
#include <QVariantMap>

#include <models/danmaku/danmakumodels.h>
#include <models/media/playerlaunchcontext.h>
#include <qcorotask.h>

class QEmbyCore;
class MpvWidget;
class NativeDanmakuOverlay;

class PlayerDanmakuController : public QObject
{
    Q_OBJECT
public:
    explicit PlayerDanmakuController(QEmbyCore *core,
                                     MpvWidget *mpvWidget,
                                     NativeDanmakuOverlay *nativeDanmakuOverlay,
                                     QObject *parent = nullptr);

    void setPlaybackContext(const PlayerLaunchContext &context);
    void clearPlaybackContext();
    void prepareForMediaReload();

    bool isDanmakuEnabled() const;
    bool isDanmakuVisible() const;
    bool hasDanmakuTrack() const;
    bool hasPreparedDanmaku() const;
    bool isLoading() const;
    bool hasPlaybackContext() const;
    QString sourceTitle() const;
    QString sourceProvider() const;
    QString sourceServerId() const;
    QString sourceServerName() const;
    int commentCount() const;
    DanmakuMediaContext mediaContext() const;
    QString activeTargetId() const;
    QString activeEndpointId() const;

    // forSecondary = true 时按副字幕选中状态标记 selected（供副字幕菜单使用）。
    QList<QVariantMap> contentSubtitleTracks(bool forSecondary = false) const;
    void selectSubtitleTrack(const QVariant &data);
    // 副字幕（第二条内容字幕）：需 PlayerSubtitleSecondaryEnabled 全局开关开启。
    // 与主字幕共用同一组轨道列表、独立选择；会记住所选语言，切集后自动恢复。
    void selectSecondarySubtitleTrack(const QVariant &data);
    int secondarySubtitleTrackId() const { return m_secondarySubtitleTrackId; }
    // 内容主字幕当前选中的轨道 id（-1 = 未选择）。
    int selectedSubtitleTrackId() const { return m_selectedSubtitleTrackId; }
    // 副字幕是否被当前弹幕渲染方式挤占（弹幕占满 sid + secondary-sid 两条轨）。
    bool secondarySubtitleBlockedByDanmaku() const;
    // 供外部（如副字幕开关变化）触发一次轨道重分配（幂等状态同步）。
    void refreshTrackSelection();

    void setDanmakuEnabled(bool enabled);
    void setDanmakuVisible(bool visible);
    void reload(const QString &manualKeyword = QString());
    void loadFromCandidate(DanmakuMatchCandidate candidate,
                           bool saveAsManualMatch = true);
    void loadLocalFile(QString filePath);

signals:
    void stateChanged();
    void toastRequested(const QString &message);

private:
    DanmakuMediaContext buildMediaContext(const PlayerLaunchContext &context) const;
    bool isDanmakuTrackMap(const QVariantMap &trackMap) const;
    void onTrackListChanged();
    // 读 mpv 原始 track-list 里的内容字幕轨（保留原生 selected 字段）。
    // 菜单展示路径（contentSubtitleTracks）会剥离该字段并重插"用户选择"，
    // 因此任何需要判断"实际在播的轨"的逻辑必须用这个原始列表。
    QList<QVariantMap> rawSubtitleTracks() const;
    void syncSubtitleSelectionFromTrackList();
    void syncSecondarySubtitleSelectionFromTrackList();
    void attachDanmakuTrack();
    void refreshDanmakuTrackId(int remainingRetries = 5);
    void removeDanmakuTrack();
    bool prefersNativeRenderer() const;
    bool shouldUseNativeRenderer() const;
    void updateDanmakuPresentation();
    void applyTrackSelection();
    void applyDanmakuMotionStabilityProfile();
    void clearDanmakuMotionStabilityProfile();

    QCoro::Task<void> loadDanmakuTask(quint64 requestId,
                                      QString manualKeyword = QString());
    QCoro::Task<void> loadDanmakuCandidateTask(
        quint64 requestId,
        DanmakuMatchCandidate candidate,
        bool saveAsManualMatch);

    QPointer<QEmbyCore> m_core;
    QPointer<MpvWidget> m_mpvWidget;
    QPointer<NativeDanmakuOverlay> m_nativeDanmakuOverlay;
    PlayerLaunchContext m_launchContext;
    DanmakuMediaContext m_mediaContext;
    QString m_assFilePath;
    QString m_sourceTitle;
    QString m_sourceProvider;
    QString m_sourceServerId;
    QString m_sourceServerName;
    QString m_activeTargetId;
    QString m_activeEndpointId;
    QList<DanmakuComment> m_commentPayload;
    int m_commentCount = 0;
    int m_danmakuTrackId = -1;
    int m_selectedSubtitleTrackId = -1;
    // 副字幕选中轨；m_secondarySubtitleLang 为会话内语言记忆（切集后按语言
    // 重新匹配恢复副字幕，因为 track id 在切集后会变化）。
    int m_secondarySubtitleTrackId = -1;
    QString m_secondarySubtitleLang;
    bool m_fileLoaded = false;
    bool m_visible = true;
    bool m_loading = false;
    bool m_motionStabilityProfileApplied = false;
    bool m_nativePayloadDirty = false;
    quint64 m_requestSerial = 0;
};

#endif 
