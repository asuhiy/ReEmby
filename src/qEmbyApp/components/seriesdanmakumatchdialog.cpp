#include "seriesdanmakumatchdialog.h"

#include "../managers/thememanager.h"
#include "loadingoverlay.h"
#include "moderntoast.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSet>
#include <QShowEvent>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>
#include <config/configstore.h>
#include <qembycore.h>
#include <services/danmaku/danmakuservice.h>
#include <qcorotimer.h>
#include <stdexcept>

namespace {

constexpr int kSeriesCandidateRole = Qt::UserRole + 1;
constexpr int kSeriesProviderRole = Qt::UserRole + 100;

// Draws a QListWidgetItem with the main text on the left and the
// candidate's provider name (BiliBili / dandanplay / danmu_api) right-
// aligned and dimmed on the same row. Used in the series picker so the
// user can tell at a glance which danmaku source each row comes from
// when several providers have entries for the same work.
class SeriesProviderItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter,
               const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QString text = index.data(Qt::DisplayRole).toString();
        const QString provider =
            index.data(kSeriesProviderRole).toString();

        painter->save();

        // 1. Draw the default item chrome (background, focus rect, etc.)
        //    with the text field cleared so we can lay out the two
        //    columns ourselves.
        opt.text.clear();
        QStyle *style = opt.widget ? opt.widget->style()
                                   : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

        // 2. Foreground colour: highlighted when selected, normal
        //    otherwise. We dim the provider label so the main title
        //    stays the visual anchor.
        QColor mainColor = opt.palette.text().color();
        if (opt.state.testFlag(QStyle::State_Selected)) {
            mainColor = opt.palette.highlightedText().color();
        }
        QColor dimColor = mainColor;
        dimColor.setAlpha(170);

        // 3. Lay out the two columns inside the item rect with sensible
        //    padding; reserve room on the right for the provider label.
        const int hPad = 8;
        const int vPad = 2;
        QRect itemRect = opt.rect.adjusted(hPad, vPad, -hPad, -vPad);

        QFont providerFont = opt.font;
        // QFont::pointSizeF() returns -1 when only the pixel size is set
        // (the common case for Qt widgets, which use the platform's DPI-
        // aware point size). Passing -0.88 to setPointSizeF triggers a
        // "Point size <= 0" warning every paint — squelch that by only
        // scaling when the font has a real point size; otherwise derive a
        // smaller pixel size directly.
        const qreal basePoint = opt.font.pointSizeF();
        if (basePoint > 0.0) {
            providerFont.setPointSizeF(basePoint * 0.88);
        } else if (opt.font.pixelSize() > 0) {
            providerFont.setPixelSize(qMax(1, qRound(opt.font.pixelSize() * 0.88)));
        }
        QFontMetrics providerFm(providerFont);
        const int providerWidth = provider.isEmpty()
                                      ? 0
                                      : providerFm.horizontalAdvance(provider) + 16;

        QRect mainRect = itemRect;
        if (providerWidth > 0 && mainRect.width() > providerWidth + 32) {
            mainRect.setRight(mainRect.right() - providerWidth);
        }
        QRect providerRect = itemRect;
        providerRect.setLeft(providerRect.right() - providerWidth + 8);

        // 4. Main title — elide if the row is too narrow.
        painter->setPen(mainColor);
        painter->setFont(opt.font);
        const QString elidedText =
            opt.fontMetrics.elidedText(text, Qt::ElideRight, mainRect.width());
        painter->drawText(mainRect,
                          Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
                          elidedText);

        // 5. Provider label — smaller and dimmer, right-aligned.
        if (!provider.isEmpty()) {
            painter->setPen(dimColor);
            painter->setFont(providerFont);
            painter->drawText(providerRect,
                              Qt::AlignVCenter | Qt::AlignRight,
                              provider);
        }

        painter->restore();
    }
};

constexpr char kEpisodeOffsetKey[] = "danmaku/episode_offset";

QString offsetConfigKey(const QString &provider, const QString &seriesId)
{
    return QString::fromLatin1("%1/%2/%3")
        .arg(QLatin1String(kEpisodeOffsetKey), provider, seriesId);
}

// Returns the user-visible label for the right-aligned "where this row
// came from" badge. Prefer the per-endpoint display name (e.g. "ziji",
// the name the user gave their LogVar/danmu_api instance) over the
// generic provider type ("danmu_api") so the user can tell at a glance
// which configured instance produced the row — matches the
// PlayerDanmakuIdentifyDialog logic.
QString providerBadgeLabel(const DanmakuMatchCandidate &candidate)
{
    const QString endpointName = candidate.endpointName.trimmed();
    if (!endpointName.isEmpty()) {
        return endpointName;
    }
    const QString provider = candidate.provider.trimmed();
    if (provider == QLatin1String("bilibili")) {
        return QCoreApplication::translate("SeriesDanmakuMatchDialog", "BiliBili");
    }    if (provider == QLatin1String("dandanplay")) {
        return QLatin1String("dandanplay");
    }
    if (provider == QLatin1String("danmu_api")) {
        return QLatin1String("danmu_api");
    }
    return provider;
}

// Heuristic: is this episode part of the main episode sequence?
// Providers without a section_type field (ziji / danmu_api, dandanplay
// public API) return every episode of a work including trailers, PVs and
// extras — e.g. "凡人修仙传" from bilibili carries 189 main episodes plus
// 106 "星海飞驳第N集预告 / 重逆天南第N集预告 / ...". We pick the main
// sequence by excluding titles carrying trailer/extra keywords. 重制版 /
// 重置版 are treated as main episodes (a re-release of the same episode,
// not a preview).
bool isMainEpisode(const DanmakuEpisode &ep)
{
    const QString text = ep.longTitle.isEmpty() ? ep.title : ep.longTitle;
    static const QRegularExpression trailerPattern(
        QStringLiteral(
            R"(预告|PV|番外|特别篇|OVA|OAD|特典|先行版|CM|OP|ED)"),
        QRegularExpression::CaseInsensitiveOption);
    return !trailerPattern.match(text).hasMatch();
}

} // namespace

SeriesDanmakuMatchDialog::SeriesDanmakuMatchDialog(
    QEmbyCore *core, Mode mode, QList<DanmakuMatchCandidate> seriesCandidates,
    DanmakuMediaContext context, QString initialKeyword, QString activeTargetId,
    QString activeEndpointId, QList<int> preSelectedEpisodes, QWidget *parent)
    : PlayerOverlayDialog(parent),
      m_core(core),
      m_mode(mode),
      m_context(std::move(context)),
      m_initialKeyword(std::move(initialKeyword)),
      m_activeTargetId(std::move(activeTargetId)),
      m_activeEndpointId(std::move(activeEndpointId)),
      m_seriesResults(std::move(seriesCandidates)),
      m_preSelectedEpisodes(std::move(preSelectedEpisodes))
{
    setSurfaceObjectName("seriesDanmakuMatchDialog");
    setSurfacePreferredSize(QSize(720, 520));
    setTitle(m_mode == Mode::Multi ? tr("Match Danmaku for Series")
                                   : tr("Search Danmaku"));

    // ---- Stage 1: series list -------------------------------------------
    m_promptLabel = new QLabel(tr("Choose the series to match danmaku for."), this);
    m_promptLabel->setObjectName("dialog-text");
    m_promptLabel->setWordWrap(true);
    contentLayout()->addWidget(m_promptLabel);
    contentLayout()->addSpacing(8);

    auto *searchRow = new QHBoxLayout();
    searchRow->setSpacing(10);
    m_seriesSearchEdit = new QLineEdit(this);
    m_seriesSearchEdit->setObjectName("PlaylistSearchEdit");
    m_seriesSearchEdit->setPlaceholderText(tr("Enter series title or keyword"));
    m_seriesSearchEdit->setClearButtonEnabled(true);
    m_seriesSearchEdit->addAction(
        ThemeManager::getAdaptiveIcon(QStringLiteral(":/svg/light/search.svg")),
        QLineEdit::LeadingPosition);
    QString defaultKeyword = m_initialKeyword.trimmed();
    if (defaultKeyword.isEmpty()) {
        defaultKeyword = m_context.isEpisode()
                             ? m_context.seriesName.trimmed()
                             : m_context.title.trimmed();
    }
    m_seriesSearchEdit->setText(defaultKeyword);
    searchRow->addWidget(m_seriesSearchEdit, 1);

    m_seriesSearchButton = new QPushButton(tr("Search"), this);
    m_seriesSearchButton->setObjectName("dialog-btn-primary");
    m_seriesSearchButton->setCursor(Qt::PointingHandCursor);
    searchRow->addWidget(m_seriesSearchButton);
    contentLayout()->addLayout(searchRow);
    contentLayout()->addSpacing(6);

    m_statusLabel = new QLabel(tr("Searching danmaku servers..."), this);
    m_statusLabel->setObjectName("dialog-text");
    m_statusLabel->setWordWrap(true);
    contentLayout()->addWidget(m_statusLabel);
    contentLayout()->addSpacing(8);

    m_seriesListContainer = new QWidget(this);
    auto *listLayout = new QVBoxLayout(m_seriesListContainer);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(6);

    m_seriesFilterEdit = new QLineEdit(m_seriesListContainer);
    m_seriesFilterEdit->setObjectName("PlaylistSearchEdit");
    m_seriesFilterEdit->setPlaceholderText(tr("Filter results"));
    m_seriesFilterEdit->setClearButtonEnabled(true);
    m_seriesFilterEdit->setEnabled(false);
    listLayout->addWidget(m_seriesFilterEdit);

    m_seriesList = new QListWidget(m_seriesListContainer);
    m_seriesList->setObjectName("ManageLibPathList");
    m_seriesList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_seriesList->setWordWrap(true);
    m_seriesList->setMinimumHeight(240);
    // Custom delegate draws the provider name (BiliBili / dandanplay /
    // danmu_api) right-aligned in each row so the user can tell at a
    // glance which danmaku source each series row came from.
    m_seriesList->setItemDelegate(new SeriesProviderItemDelegate(m_seriesList));
    listLayout->addWidget(m_seriesList);

    m_loadingOverlay = new LoadingOverlay(m_seriesListContainer);
    m_loadingOverlay->setHudPanelVisible(false);
    m_loadingOverlay->setSubtleOverlay(true);

    contentLayout()->addWidget(m_seriesListContainer, 1);
    contentLayout()->addSpacing(12);

    // ---- Stage 2: episode picker (hidden initially) ---------------------
    m_episodePanel = new QWidget(this);
    auto *episodeLayout = new QVBoxLayout(m_episodePanel);
    episodeLayout->setContentsMargins(0, 0, 0, 0);
    episodeLayout->setSpacing(6);

    auto *episodeHeader = new QHBoxLayout();
    m_backButton = new QPushButton(QStringLiteral("\u2190 ") +
                                       tr("Back to Search"),
                                   m_episodePanel);
    m_backButton->setObjectName("dialog-btn-cancel");
    m_backButton->setCursor(Qt::PointingHandCursor);
    episodeHeader->addWidget(m_backButton);

    m_episodeTitleLabel = new QLabel(m_episodePanel);
    m_episodeTitleLabel->setObjectName("dialog-title");
    episodeHeader->addWidget(m_episodeTitleLabel, 1);

    if (m_mode == Mode::Multi) {
        m_offsetEdit = new QLineEdit(m_episodePanel);
        m_offsetEdit->setObjectName("PlaylistSearchEdit");
        m_offsetEdit->setPlaceholderText(tr("Episode offset"));
        m_offsetEdit->setFixedWidth(110);
        m_offsetEdit->setToolTip(
            tr("Add N to every episode number when the danmaku source numbers "
               "differ from your library (e.g. source starts at 3)."));
        episodeHeader->addWidget(m_offsetEdit);
        m_selectAllButton = new QPushButton(tr("Select All"), m_episodePanel);
        m_selectAllButton->setObjectName("dialog-btn-primary");
        m_selectAllButton->setCursor(Qt::PointingHandCursor);
        episodeHeader->addWidget(m_selectAllButton);
    }
    episodeLayout->addLayout(episodeHeader);

    if (m_mode == Mode::Single) {
        // The player-side picker is per-episode (pick exactly one row for
        // the currently playing episode); a filter box helps when a series
        // carries hundreds of episodes. Multi mode intentionally keeps the
        // offset + select-all header instead.
        m_episodeFilterEdit = new QLineEdit(m_episodePanel);
        m_episodeFilterEdit->setObjectName("PlaylistSearchEdit");
        m_episodeFilterEdit->setPlaceholderText(tr("Filter episodes"));
        m_episodeFilterEdit->setClearButtonEnabled(true);
        m_episodeFilterEdit->addAction(
            ThemeManager::getAdaptiveIcon(
                QStringLiteral(":/svg/light/search.svg")),
            QLineEdit::LeadingPosition);
        episodeLayout->addWidget(m_episodeFilterEdit);
    }

    m_episodeList = new QListWidget(m_episodePanel);
    m_episodeList->setObjectName("ManageLibPathList");
    // MultiSelection: each click toggles the row's selection, no modifier
    // needed. ExtendedSelection would replace the previous selection on a
    // fresh click — the user wants individual items to stay selected while
    // they pick others.
    m_episodeList->setSelectionMode(m_mode == Mode::Multi
                                        ? QAbstractItemView::MultiSelection
                                        : QAbstractItemView::SingleSelection);
    m_episodeList->setWordWrap(true);
    m_episodeList->setMinimumHeight(240);
    episodeLayout->addWidget(m_episodeList, 1);

    if (m_mode == Mode::Multi) {
        m_selectionSummaryLabel = new QLabel(m_episodePanel);
        m_selectionSummaryLabel->setObjectName("dialog-text");
        episodeLayout->addWidget(m_selectionSummaryLabel);
    }

    auto *episodeButtons = new QHBoxLayout();
    episodeButtons->addStretch();
    auto *cancelButton = new QPushButton(tr("Cancel"), m_episodePanel);
    cancelButton->setObjectName("dialog-btn-cancel");
    cancelButton->setCursor(Qt::PointingHandCursor);
    connect(cancelButton, &QPushButton::clicked, this,
            &PlayerOverlayDialog::reject);
    episodeButtons->addWidget(cancelButton);
    m_confirmButton = new QPushButton(tr("Confirm"), m_episodePanel);
    m_confirmButton->setObjectName("dialog-btn-primary");
    m_confirmButton->setCursor(Qt::PointingHandCursor);
    m_confirmButton->setEnabled(false);
    connect(m_confirmButton, &QPushButton::clicked, this, [this]() {
        if (m_mode != Mode::Multi) {
            // Single mode: materialise the episode candidate from the
            // current row right before accepting. The item data holds the
            // original episodes[] index — currentRow() is the *visible*
            // row, which drifts when the filter box skips rows.
            const int currentRow =
                m_episodeList ? m_episodeList->currentRow() : -1;
            if (currentRow < 0 || !m_episodeList->item(currentRow)) {
                return;
            }
            bool ok = false;
            const int epIndex =
                m_episodeList->item(currentRow)
                    ->data(kSeriesCandidateRole)
                    .toInt(&ok);
            if (!ok || epIndex < 0 ||
                epIndex >= m_currentSeries.episodes.size()) {
                return;
            }
            const DanmakuEpisode &ep =
                m_currentSeries.episodes.at(epIndex);
            DanmakuMatchCandidate episodeCandidate = m_currentSeries;
            episodeCandidate.episodes.clear();
            episodeCandidate.targetId = ep.cid;
            episodeCandidate.episodeNumber = ep.episodeNumber + m_episodeOffset;
            episodeCandidate.seasonNumber = -1;
            episodeCandidate.durationMs = ep.durationMs;
            episodeCandidate.title = ep.longTitle.isEmpty() ? ep.title : ep.longTitle;
            episodeCandidate.subtitle = m_currentSeries.title;
            m_selectedEpisodes = {episodeCandidate};
        }
        if (!m_selectedEpisodes.isEmpty()) {
            accept();
        }
    });
    episodeButtons->addWidget(m_confirmButton);
    episodeLayout->addLayout(episodeButtons);
    m_episodePanel->hide();

    contentLayout()->addWidget(m_episodePanel);

    // ---- wiring ---------------------------------------------------------
    connect(m_seriesSearchButton, &QPushButton::clicked, this,
            [this]() { triggerSeriesSearch(); });
    connect(m_seriesSearchEdit, &QLineEdit::returnPressed, this,
            [this]() { triggerSeriesSearch(); });
    connect(m_seriesFilterEdit, &QLineEdit::textChanged, this,
            [this](const QString &) { rebuildSeriesList(); });
    connect(m_seriesList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        // Read the original m_seriesResults index from the item data —
        // row() is the *visible* row, which drifts after the filter box
        // skips rows.
        bool ok = false;
        const int idx = item->data(kSeriesCandidateRole).toInt(&ok);
        if (ok && idx >= 0 && idx < m_seriesResults.size()) {
            enterEpisodePicker(idx);
        }
    });
    connect(m_episodeList, &QListWidget::itemSelectionChanged, this,
            [this]() {
                updateSelectionSummary();
                updateSelectAllLabel();
            });
    if (m_selectAllButton) {
        connect(m_selectAllButton, &QPushButton::clicked, this, [this]() {
            if (!m_episodeList || m_episodeList->count() == 0) {
                return;
            }
            if (m_allSelected) {
                // Already all selected: a second click clears the list.
                m_episodeList->clearSelection();
                // clearSelection does not trigger itemSelectionChanged on
                // all Qt backports when going from "all selected" to
                // "nothing"; drive the bookkeeping manually.
                m_allSelected = false;
                updateSelectAllLabel();
                updateSelectionSummary();
            } else {
                m_episodeList->selectAll();
                m_allSelected = true;
                // selectAll() fires itemSelectionChanged but on some Qt
                // builds it does not fire if the list was already fully
                // selected; refresh once to be safe.
                if (m_episodeList->selectedItems().size() !=
                    m_episodeList->count()) {
                    m_episodeList->selectAll();
                }
                updateSelectAllLabel();
                updateSelectionSummary();
            }
        });
    }
    if (m_offsetEdit) {
        connect(m_offsetEdit, &QLineEdit::textChanged, this,
                [this](const QString &) { applyEpisodeOffset(); });
    }
    if (m_episodeFilterEdit) {
        connect(m_episodeFilterEdit, &QLineEdit::textChanged, this,
                [this](const QString &) {
                    // Re-filtering rebuilds the list, which clears the
                    // selection; refresh the confirm-button state too.
                    rebuildEpisodeList();
                    updateSelectionSummary();
                });
    }
    connect(m_backButton, &QPushButton::clicked, this, [this]() {
        m_episodePanel->hide();
        m_seriesListContainer->show();
        m_promptLabel->show();
        m_seriesSearchEdit->setFocus();
    });

    m_episodeOffset = 0;
}

QList<DanmakuMatchCandidate> SeriesDanmakuMatchDialog::selectedEpisodes() const
{
    return m_selectedEpisodes;
}

void SeriesDanmakuMatchDialog::showEvent(QShowEvent *event)
{
    PlayerOverlayDialog::showEvent(event);
    updateLoadingOverlayGeometry();
    if (m_seriesSearchEdit && !m_loaded) {
        m_seriesSearchEdit->setFocus();
        m_seriesSearchEdit->selectAll();
    }
    if (!m_loaded) {
        m_loaded = true;
        triggerSeriesSearch();
    }
}

void SeriesDanmakuMatchDialog::resizeEvent(QResizeEvent *event)
{
    PlayerOverlayDialog::resizeEvent(event);
    updateLoadingOverlayGeometry();
}

QCoro::Task<void> SeriesDanmakuMatchDialog::searchSeries(QString queryText)
{
    QPointer<SeriesDanmakuMatchDialog> safeThis(this);
    QPointer<QEmbyCore> core(m_core);
    const DanmakuMediaContext context = m_context;
    queryText = queryText.trimmed();
    if (queryText.isEmpty()) {
        queryText = context.displayTitle().trimmed();
    }
    if (queryText.isEmpty()) {
        queryText = context.title.trimmed();
    }
    if (queryText.isEmpty() || !safeThis || !core || !core->danmakuService()) {
        co_return;
    }

    m_isLoading = true;
    updateStatusText(tr("Searching danmaku servers..."));
    updateUiState();

    try {
        // Single-server search: only the danmaku sources enabled on the
        // current Emby server are queried. The cross-server aggregator
        // (searchAllCandidatesAcrossServers) previously pulled sources
        // from every configured Emby server, which duplicated the same
        // service when it was registered under multiple servers (e.g.
        // the same ziji endpoint on A服 and B服) and inflated episode
        // counts (风起天南 590 = 295 × 2). The user explicitly wants
        // "search the sources enabled on the server I'm using" — no
        // cross-server traversal, no duplicate queries.
        const QList<DanmakuMatchCandidate> results =
            co_await core->danmakuService()->searchAllCandidates(
                context, queryText);
        if (!safeThis) {
            co_return;
        }
        safeThis->m_seriesResults = results;
        safeThis->m_isLoading = false;
        safeThis->rebuildSeriesList();
        safeThis->updateStatusText(
            results.isEmpty()
                ? tr("No danmaku sources found for \"%1\"").arg(queryText)
                : tr("%1 result(s) found").arg(results.size()));
        safeThis->updateUiState();
    } catch (const std::exception &e) {
        if (!safeThis) {
            co_return;
        }
        safeThis->m_seriesResults.clear();
        safeThis->m_isLoading = false;
        safeThis->rebuildSeriesList();
        safeThis->updateStatusText(tr("Search failed"));
        safeThis->updateUiState();
        qWarning().noquote()
            << "[SeriesDanmaku] search failed"
            << "| error:" << e.what();
        ModernToast::showMessage(
            tr("Failed to search danmaku: %1").arg(QString::fromUtf8(e.what())),
            3000);
    }
}

void SeriesDanmakuMatchDialog::triggerSeriesSearch()
{
    if (!m_seriesSearchEdit || m_isLoading) {
        return;
    }
    m_pendingTask = searchSeries(m_seriesSearchEdit->text());
}

void SeriesDanmakuMatchDialog::rebuildSeriesList()
{
    if (!m_seriesList) {
        return;
    }
    m_seriesList->clear();
    const QString filter = m_seriesFilterEdit->text().trimmed();
    for (int i = 0; i < m_seriesResults.size(); ++i) {
        const DanmakuMatchCandidate &candidate = m_seriesResults.at(i);
        const QString displayText = candidate.displayText();
        if (!filter.isEmpty() &&
            !displayText.contains(filter, Qt::CaseInsensitive) &&
            !candidate.provider.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }
        QString text = displayText;
        if (candidate.isSeries()) {
            text = QStringLiteral("%1 (%2)")
                       .arg(displayText)
                       .arg(candidate.episodes.size());
        }
        auto *item = new QListWidgetItem(text, m_seriesList);
        item->setData(kSeriesCandidateRole, i);
        // Picked up by SeriesProviderItemDelegate to render a right-
        // aligned "BiliBili" / "dandanplay" / "ziji" label (prefers the
        // user-named endpoint over the generic provider type).
        item->setData(kSeriesProviderRole,
                      providerBadgeLabel(candidate));
        item->setToolTip(candidate.isSeries()
                             ? tr("Click to choose episodes")
                             : tr("Click to load danmaku"));
    }
}

void SeriesDanmakuMatchDialog::enterEpisodePicker(int row)
{
    if (row < 0 || row >= m_seriesResults.size()) {
        return;
    }
    m_currentSeries = m_seriesResults.at(row);

    // Episode-level candidate (not a series): nothing to pick — accept it
    // immediately with a single entry.
    if (!m_currentSeries.isSeries()) {
        m_selectedEpisodes = {m_currentSeries};
        accept();
        return;
    }

    // Restore the saved per-series offset (multi mode).
    m_episodeOffset = 0;
    if (m_mode == Mode::Multi && m_offsetEdit) {
        m_episodeOffset = ConfigStore::instance()
                              ->get<int>(offsetConfigKey(m_currentSeries.provider,
                                                         m_currentSeries.targetId),
                                          0);
        m_offsetEdit->setText(QString::number(m_episodeOffset));
    }

    m_selectedEpisodes.clear();
    // Single mode (player-side) labels the stage "Select Episode" — the
    // user is picking exactly one danmaku source for the current episode.
    // Multi mode keeps the plain series title as the heading.
    m_episodeTitleLabel->setText(
        m_mode == Mode::Single
            ? tr("Select Episode: %1").arg(m_currentSeries.displayText())
            : m_currentSeries.displayText());
    if (m_episodeFilterEdit) {
        m_episodeFilterEdit->clear();
    }
    rebuildEpisodeList();
    // Player-side (Single): auto-locate the row for the episode that is
    // currently playing, and center it in the viewport, so the picker
    // opens at the relevant main episode instead of the top of a long
    // list (e.g. 295 rows for 凡人修仙传).
    if (m_mode == Mode::Single && m_episodeList &&
        m_context.episodeNumber > 0) {
        for (int i = 0; i < m_episodeList->count(); ++i) {
            bool ok = false;
            const int epIndex = m_episodeList->item(i)
                                    ->data(kSeriesCandidateRole)
                                    .toInt(&ok);
            if (ok && epIndex >= 0 &&
                epIndex < m_currentSeries.episodes.size() &&
                m_currentSeries.episodes.at(epIndex).episodeNumber ==
                    m_context.episodeNumber) {
                m_episodeList->setCurrentRow(i);
                m_episodeList->scrollToItem(m_episodeList->item(i),
                                            QAbstractItemView::PositionAtCenter);
                break;
            }
        }
    }
    m_seriesListContainer->hide();
    m_promptLabel->hide();
    m_episodePanel->show();
    updateSelectionSummary();
    updateUiState();
}

void SeriesDanmakuMatchDialog::rebuildEpisodeList()
{
    if (!m_episodeList) {
        return;
    }
    const QString filter = m_episodeFilterEdit
                               ? m_episodeFilterEdit->text().trimmed()
                               : QString();
    m_episodeList->clear();
    // Build a quick lookup of pre-ticked adjusted episode numbers so the
    // episode picker can restore the user's prior selections.
    QSet<int> preTicked;
    for (const int ep : m_preSelectedEpisodes) {
        if (ep > 0) {
            preTicked.insert(ep);
        }
    }
    int ticked = 0;
    for (int row = 0; row < m_currentSeries.episodes.size(); ++row) {
        const DanmakuEpisode &ep = m_currentSeries.episodes.at(row);
        const int adjusted = ep.episodeNumber + m_episodeOffset;
        const QString label =
            QStringLiteral("%1. %2")
                .arg(adjusted > 0 ? adjusted : 0, 2, 10, QChar('0'))
                .arg(ep.longTitle.isEmpty() ? ep.title : ep.longTitle);
        if (!filter.isEmpty() &&
            !label.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }
        auto *item = new QListWidgetItem(label, m_episodeList);
        // Original episodes[] index, so confirm / updateSelectionSummary
        // survive the filter box (visible row != episodes index).
        item->setData(kSeriesCandidateRole, row);
        // setSelected on the freshly created item (not item(row) — the
        // row index no longer maps 1:1 to episodes when filtering skips
        // rows).
        if (preTicked.contains(adjusted)) {
            item->setSelected(true);
            ++ticked;
        } else if (m_mode == Mode::Multi && isMainEpisode(ep)) {
            // Detail-page picker: default to selecting the whole main
            // episode sequence (trailers/PVs/extras stay unchecked). The
            // user can still manually adjust individual rows afterwards.
            item->setSelected(true);
            ++ticked;
        }
    }
    m_allSelected = (ticked > 0 && ticked == m_currentSeries.episodes.size());
    updateSelectAllLabel();
}

void SeriesDanmakuMatchDialog::applyEpisodeOffset()
{
    if (!m_offsetEdit) {
        return;
    }
    bool ok = false;
    const int offset = m_offsetEdit->text().trimmed().toInt(&ok);
    m_episodeOffset = ok ? offset : 0;
    ConfigStore::instance()->set(offsetConfigKey(m_currentSeries.provider,
                                                 m_currentSeries.targetId),
                                 m_episodeOffset);
    rebuildEpisodeList();
    updateSelectionSummary();
}

void SeriesDanmakuMatchDialog::updateSelectionSummary()
{
    if (m_mode != Mode::Multi) {
        const bool hasSelection =
            m_episodeList && m_episodeList->currentItem() != nullptr;
        if (m_confirmButton) {
            m_confirmButton->setEnabled(hasSelection);
        }
        if (m_selectionSummaryLabel) {
            m_selectionSummaryLabel->setText(
                hasSelection ? tr("1 episode selected")
                             : tr("Select an episode"));
        }
        return;
    }

    QList<int> selectedIndexes;
    for (int i = 0; i < m_episodeList->count(); ++i) {
        QListWidgetItem *item = m_episodeList->item(i);
        if (!item || !item->isSelected()) {
            continue;
        }
        // item data carries the original episodes[] index (visible row
        // != episodes index once the filter box skips rows).
        bool ok = false;
        const int epIndex = item->data(kSeriesCandidateRole).toInt(&ok);
        if (ok && epIndex >= 0 && epIndex < m_currentSeries.episodes.size()) {
            selectedIndexes.append(epIndex);
        }
    }
    m_selectedEpisodes.clear();
    for (const int idx : selectedIndexes) {
        const DanmakuEpisode &ep = m_currentSeries.episodes.at(idx);
        DanmakuMatchCandidate episodeCandidate = m_currentSeries;
        episodeCandidate.episodes.clear();
        episodeCandidate.targetId = ep.cid;
        episodeCandidate.episodeNumber = ep.episodeNumber + m_episodeOffset;
        episodeCandidate.seasonNumber = -1;
        episodeCandidate.durationMs = ep.durationMs;
        episodeCandidate.title = ep.longTitle.isEmpty() ? ep.title : ep.longTitle;
        episodeCandidate.subtitle = m_currentSeries.title;
        m_selectedEpisodes.append(episodeCandidate);
    }
    if (m_selectionSummaryLabel) {
        m_selectionSummaryLabel->setText(
            tr("%1 / %2 episodes selected")
                .arg(m_selectedEpisodes.size())
                .arg(m_currentSeries.episodes.size()));
    }
    if (m_confirmButton) {
        m_confirmButton->setEnabled(!m_selectedEpisodes.isEmpty());
    }
}

void SeriesDanmakuMatchDialog::updateSelectAllLabel()
{
    if (!m_selectAllButton || !m_episodeList) {
        return;
    }
    // Mirror the user-visible "all selected" state derived from the actual
    // selection. Each user-driven click on the button still toggles between
    // select-all and clear-selection, but the label should also flip back
    // automatically if the user manually deselects items, so the button is
    // never misleading.
    m_allSelected =
        m_episodeList->count() > 0 &&
        m_episodeList->selectedItems().size() == m_episodeList->count();
    m_selectAllButton->setText(
        m_allSelected ? tr("Deselect All") : tr("Select All"));
}

void SeriesDanmakuMatchDialog::updateUiState()
{
    const bool searching = m_isLoading;
    if (m_seriesSearchButton) {
        m_seriesSearchButton->setEnabled(!searching);
    }
    if (m_seriesFilterEdit) {
        m_seriesFilterEdit->setEnabled(!searching);
    }
    if (m_loadingOverlay) {
        m_loadingOverlay->setVisible(searching);
    }
}

void SeriesDanmakuMatchDialog::updateLoadingOverlayGeometry()
{
    if (m_loadingOverlay && m_seriesListContainer) {
        m_loadingOverlay->setGeometry(m_seriesListContainer->rect());
        m_loadingOverlay->raise();
    }
}

void SeriesDanmakuMatchDialog::updateStatusText(const QString &text)
{
    if (m_statusLabel) {
        m_statusLabel->setText(text);
    }
}
