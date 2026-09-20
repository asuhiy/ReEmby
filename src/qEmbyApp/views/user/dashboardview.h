#ifndef DASHBOARDVIEW_H
#define DASHBOARDVIEW_H

#include "../baseview.h"
#include <QStringList>
#include <qcorotask.h>

class QListView;
class MediaListModel;
class MediaCardDelegate;
class QLabel;
class QWidget;
class QVBoxLayout;
class QScrollArea;
class HorizontalListViewGallery;
class MediaSectionWidget;
class SmoothScrollController;

class DashboardView : public BaseView
{
    Q_OBJECT
public:
    explicit DashboardView(QEmbyCore* core, QWidget *parent = nullptr);


    QCoro::Task<void> loadDashboardData();

    // Surface a transient empty/error state when the active server is
    // unreachable after a server-switch. Hides the section galleries and
    // shows a retry CTA. Auto-clears when activeServerChanged fires with a
    // different serverId (see onActiveServerChanged below).
    void showServerUnreachableState();

public Q_SLOTS:
    void scrollToTop() override;

Q_SIGNALS:
    
    void navigateToLibrary(const QString& libraryId, const QString& libraryName);

protected:
    
    void onMediaItemUpdated(const MediaItem& item) override;
    
    
    void onMediaItemRemoved(const QString& itemId) override;
    CardContextMenuRequest showCardContextMenu(const MediaItem& item,
                                               const QPoint& globalPos) override;
    void dispatchCardContextMenuRequest(
        const MediaItem& item, const CardContextMenuRequest& request) override;

    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void setupUi();
    void applyDashboardSectionOrder();
    QStringList dashboardSectionOrder() const;
    QString currentServerId() const;
    QString currentDashboardContextKey() const;
    QWidget* sectionWidgetForId(const QString& sectionId) const;
    void clearLibraryGallerySections();
    void clearDashboardState(bool resetScrollPositions);
    void resetDashboardScrollPositions();
    void clearDashboardGallery(HorizontalListViewGallery* gallery);
    void resetDashboardGalleryScrollPosition(
        HorizontalListViewGallery* gallery) const;
    void resetListViewScrollPosition(QListView* listView) const;
    bool isManageableDashboardLibraryCard(const MediaItem& item) const;
    void openDashboardLibraryImageEditor(const MediaItem& item);

    
    QWidget* createSectionHeader(const QString& title, const QString& type);
    
    QListView* createGridListView(MediaListModel** outModel);
    void adjustLibraryGridHeight();

    
    QCoro::Task<void> loadResumeSection(bool show, int generation);
    QCoro::Task<void> loadLatestSection(bool show, int generation);
    QCoro::Task<void> loadRecommendedSection(bool show, int generation);
    QCoro::Task<void> loadCompletedSection(bool show, int generation);
    QCoro::Task<void> loadLibrarySections(bool showLibraries, bool showEachLibrary, int generation);
    QCoro::Task<void> executeDashboardLibraryRefresh(
        MediaItem item, bool replaceAllMetadata, bool replaceAllImages,
        bool isMetadataRefresh);

    QScrollArea* m_mainScrollArea = nullptr;

    SmoothScrollController* m_vScrollController = nullptr;

    // Empty/error overlay shown on dashboard when an active server is
    // unreachable. Built lazily by showServerUnreachableState().
    QWidget* m_unreachableOverlay = nullptr;
    QLabel* m_unreachableDetailLabel = nullptr;

    
    QWidget* m_resumeSection = nullptr;
    QWidget* m_resumeHeader = nullptr;
    HorizontalListViewGallery* m_resumeGallery = nullptr;

    
    QWidget* m_latestSection = nullptr;
    QWidget* m_latestHeader = nullptr;
    HorizontalListViewGallery* m_latestGallery = nullptr;

    
    QWidget* m_recommendSection = nullptr;
    QWidget* m_recommendHeader = nullptr;
    HorizontalListViewGallery* m_recommendGallery = nullptr;

    
    QWidget* m_completedSection = nullptr;
    QWidget* m_completedHeader = nullptr;
    HorizontalListViewGallery* m_completedGallery = nullptr;

    
    QWidget* m_libraryGridSection = nullptr;
    QWidget* m_librarySectionsContainer = nullptr;
    QLabel* m_libraryTitle = nullptr;
    QListView* m_libraryListView = nullptr;
    MediaListModel* m_libraryModel = nullptr;

    
    QList<MediaSectionWidget*> m_libraryGalleries;
    QVBoxLayout* m_containerLayout = nullptr;
    QVBoxLayout* m_librarySectionsLayout = nullptr;

    
    MediaCardDelegate* m_libraryDelegate = nullptr;

    
    int m_loadGeneration = 0;
    QString m_dashboardContextKey;
};

#endif 
