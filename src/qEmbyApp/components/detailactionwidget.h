#ifndef DETAILACTIONWIDGET_H
#define DETAILACTIONWIDGET_H

#include <QWidget>
#include "models/media/mediaitem.h"
#include <optional>

class QPushButton;
class QProgressBar;
class QLabel;
class ModernMenuButton;
class SplitPlayerButton;

class DetailActionWidget : public QWidget {
    Q_OBJECT
public:
    explicit DetailActionWidget(QWidget* parent = nullptr);

    // 行动作按钮用 FlowLayout（按宽度换行），高度必须按实际宽度算。
    // QWidget 的 sizePolicy 默认 hasHeightForWidth()==false，外层布局因此
    // 只会用构造期的 sizeHint().height()（那时拿不到真实宽度，必然算错行数）
    // 而不会再问 heightForWidth → 第二行被裁、整行不可见。
    // 这里显式打开并转发给内部 layout，让外层拿到真实宽度后重新询问高度。
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;

    void setupNormalMode(const MediaItem& item);
    void setupSeriesMode(const MediaItem& nextUpItem, const QString& epTag);
    void setSeriesLoadingMode();

    void setFavoriteState(bool isFavorite);
    void setPlayedState(bool played);
    void setSources(const QList<MediaSourceInfo>& sources, int currentIndex = -1);
    void setStreams(const MediaSourceInfo& source,
                    std::optional<int> rememberedAudioIndex = std::nullopt,
                    std::optional<int> rememberedSubtitleIndex = std::nullopt);
    void clear();

    // Trakt sync button: shown only when enabled, signed in and the item type
    // is something Trakt can address (Movie / Episode / Series).
    void setTraktItemType(const QString& itemType);
    void updateTraktButtonVisibility();

    // Danmaku match button: visible for Movie / Episode / Series. The detail
    // view connects danmakuMatchRequested to the two-stage series matcher.
    void setDanmakuMatchVisible(bool visible);
    // Bound count > 0: switch the button area to a "rematch + clear"
    // pair, since at least one episode is already wired to a danmaku
    // source. count == 0 hides the pair and reverts to a single "match"
    // button. The count also surfaces as a tooltip on the rematch button
    // so the user can see how many episodes are wired.
    void setDanmakuMatchBoundCount(int count);

    int currentSourceIndex() const;
    int currentAudioIndex() const;
    int currentSubtitleIndex() const;

    
    void refreshExtPlayerButton();

signals:
    void playRequested();
    void resumeRequested();
    void favoriteRequested();
    void playedToggleRequested();
    void traktMarkWatchedRequested();
    void traktUnmarkWatchedRequested();
    void danmakuMatchRequested();
    void danmakuRematchRequested();
    void danmakuClearBindingsRequested();
    void sourceVersionChanged(int index);
    void audioStreamChanged(int streamIndex);
    void subtitleStreamChanged(int streamIndex);
    void externalPlayRequested(const QString &playerPath);

private:
    QString formatRunTime(long long ticks);

    QPushButton* m_resumeBtn;
    QPushButton* m_playBtn;
    QPushButton* m_favBtn;
    QPushButton* m_playedBtn;
    QPushButton* m_traktBtn = nullptr;
    QPushButton* m_danmakuMatchBtn = nullptr;
    QPushButton* m_danmakuRematchBtn = nullptr;
    QPushButton* m_danmakuClearBtn = nullptr;
    QString m_traktItemType;
    int m_danmakuBoundCount = 0;

    QWidget* m_progressWidget;
    QProgressBar* m_progressBar;
    QLabel* m_remainingTimeLabel;

    ModernMenuButton* m_versionComboBox;
    ModernMenuButton* m_audioComboBox;
    ModernMenuButton* m_subtitleComboBox;
    QList<int> m_sourceIndexes;

    
    SplitPlayerButton* m_extPlayerBtn;
};

#endif 
