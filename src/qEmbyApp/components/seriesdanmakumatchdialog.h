#ifndef SERIESDANMAKUMATCHDIALOG_H
#define SERIESDANMAKUMATCHDIALOG_H

#include "playeroverlaydialog.h"

#include <models/danmaku/danmakumodels.h>

#include <QList>
#include <QString>
#include <optional>
#include <qcorotask.h>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QResizeEvent;
class QShowEvent;
class QEmbyCore;
class QWidget;
class LoadingOverlay;

// Two-stage danmaku match dialog.
//
// Stage 1 (series list): the provider returns one row per series
// (e.g. "凡人修仙传" -> one candidate carrying its full episode list in
// candidate.episodes). Clicking a row opens stage 2.
//
// Stage 2 (episode picker): shows candidate.episodes as "N. longTitle" rows.
//   - single mode: one row selected, confirm returns that single episode
//     (used from the player, where a specific episode is already playing).
//   - multi mode: checkboxes + select-all + episode-offset input (used from
//     the series detail page to pre-bind the whole show). The offset lets
//     users reconcile numbering when Bilibili's long_title numbering differs
//     from the server's episode numbers (e.g. B站 S01 starts at 3).
class SeriesDanmakuMatchDialog : public PlayerOverlayDialog
{
    Q_OBJECT

public:
    enum class Mode
    {
        Single, // player: pick one episode
        Multi   // detail page: pre-bind several episodes
    };

    explicit SeriesDanmakuMatchDialog(QEmbyCore *core,
                                      Mode mode,
                                      QList<DanmakuMatchCandidate> seriesCandidates,
                                      DanmakuMediaContext context,
                                      QString initialKeyword,
                                      QString activeTargetId,
                                      QString activeEndpointId,
                                      QList<int> preSelectedEpisodes = {},
                                      QWidget *parent = nullptr);

    // Episode-level candidates selected at stage 2 (one per picked episode).
    // Each candidate has targetId = concrete cid and episodeNumber resolved
    // against the server-side episode number.
    QList<DanmakuMatchCandidate> selectedEpisodes() const;

protected:
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QCoro::Task<void> searchSeries(QString queryText);
    void triggerSeriesSearch();
    void rebuildSeriesList();
    void enterEpisodePicker(int row);
    void rebuildEpisodeList();
    void applyEpisodeOffset();
    void updateSelectionSummary();
    void updateSelectAllLabel();
    void updateUiState();
    void updateLoadingOverlayGeometry();
    void updateStatusText(const QString &text);

    QEmbyCore *m_core = nullptr;
    Mode m_mode = Mode::Single;
    DanmakuMediaContext m_context;
    QString m_initialKeyword;
    QString m_activeTargetId;
    QString m_activeEndpointId;

    // Stage 1 widgets
    QLabel *m_promptLabel = nullptr;
    QLineEdit *m_seriesSearchEdit = nullptr;
    QPushButton *m_seriesSearchButton = nullptr;
    QLabel *m_statusLabel = nullptr;
    QWidget *m_seriesListContainer = nullptr;
    QLineEdit *m_seriesFilterEdit = nullptr;
    QListWidget *m_seriesList = nullptr;
    LoadingOverlay *m_loadingOverlay = nullptr;
    QList<DanmakuMatchCandidate> m_seriesResults;

    // Stage 2 widgets
    QWidget *m_episodePanel = nullptr;
    QLabel *m_episodeTitleLabel = nullptr;
    QPushButton *m_backButton = nullptr;
    QLineEdit *m_offsetEdit = nullptr;   // multi mode only
    QPushButton *m_selectAllButton = nullptr; // multi mode only
    QLineEdit *m_episodeFilterEdit = nullptr; // single mode only
    QListWidget *m_episodeList = nullptr;
    QLabel *m_selectionSummaryLabel = nullptr;
    QPushButton *m_confirmButton = nullptr;

    DanmakuMatchCandidate m_currentSeries;
    int m_episodeOffset = 0;
    QList<DanmakuMatchCandidate> m_selectedEpisodes;
    QList<int> m_preSelectedEpisodes;
    bool m_isLoading = false;
    bool m_loaded = false;
    // Multi-mode helpers: tracks whether the select-all button is currently
    // in the "all selected" state, so a second click on the same button
    // toggles back to "nothing selected".
    bool m_allSelected = false;
    std::optional<QCoro::Task<void>> m_pendingTask;
};

#endif // SERIESDANMAKUMATCHDIALOG_H
