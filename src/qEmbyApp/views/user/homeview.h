#ifndef HOMEVIEW_H
#define HOMEVIEW_H

#include <QStack>
#include <QWidget>
#include <QHBoxLayout>
#include <QPropertyAnimation>
#include <QPointer>
#include <qcorotask.h>
#include <optional>
#include <models/media/mediaitem.h>

class QTimer;
class QEmbyCore;
struct ServerProfile;
class SlidingStackedWidget;
class QLabel;
class QPushButton;
class QAction;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QCompleter;
class QStringListModel;
class ElidedLabel;
class QBoxLayout;
class QVBoxLayout;
class DashboardView;
class FavoritesView;
class AggregatedSearchView;
class AggregatedHistoryView;
class AggregatedFavoritesView;
class ServerScopedView;
class SettingsView;
class SearchHistoryPopup;
class SmoothScrollController;
class SeasonView; 
class PlaybackManager; 
class PlayerView;      
class ManageView;      
class WebdavProfileStore;


struct RouteInfo {
    QPointer<QWidget> widget; 
    bool isDynamic;           
    QString routeType;        
    QString routeId;          
    QString routeTitle;       
    QString routeExtraId;     
};

class HomeView : public QWidget
{
    Q_OBJECT
public:
    explicit HomeView(QEmbyCore* core, QWidget *parent = nullptr);
    ~HomeView() override;
    
    
    QCoro::Task<void> refreshProfile();

    
    void triggerSearch(const QString& query);

    
    bool canNavigateBack() const;

    bool canGoHome() const;

    bool canGoFav() const;

    // Opens the server-switcher dropdown anchored to the given trigger
    // widget (the MainWindow titlebar server pill). Public so MainWindow
    // can invoke it.
    void showServerSwitcher(QWidget *anchorWidget = nullptr);

    
    PlayerView* activePlayerView() const;

public Q_SLOTS:
    
    void navigateBack();
    void goHome();
    void goFav();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

signals:
    void logoutRequested();
    void canNavigateBackChanged(bool canBack);
    void homeContentSwitched();
    
    // 聚合功能入口（阶段3/4 的聚合视图连接这些信号）：
    // 侧边栏「聚合搜索...」回车 / 「聚合历史」「聚合收藏」按钮点击。
    void aggregatedSearchRequested(const QString& query);
    void aggregatedHistoryRequested();
    void aggregatedFavoritesRequested();
    
    void immersiveStateChanged(bool isImmersive);
    void playerChromeVisibilityChanged(bool visible);
    void serverUnreachable(const QString& serverId, const QString& displayName);

private:
    void scheduleProfileRefresh();
    void setupUi();
    void setupSidebar();

    void showSidebar();
    void hideSidebar();
    void applySidebarPosition();
    void applySidebarPinned(bool pinned);
    bool isCurrentViewImmersive() const;
    int sidebarWidthForMode(bool pinned) const;
    void applySidebarMetrics(bool pinned);
    void applySidebarIcons();
    void applySidebarCustomVisibility();
    void syncSidebarVisibility();
    void openCloudSyncDialog();
    void setupSearchHistory();
    void updateSearchCompleter(const QString &text = QString());
    // 阶段6：搜索历史下拉（当前服搜索框 + 聚合搜索框共用 SearchHistoryPopup）。
    void setupSearchHistoryPopups();
    void showHistoryPopupFor(QLineEdit *box, SearchHistoryPopup *popup,
                             const QString &bucket);
    void dismissHistoryPopups();

    
    void pushView(QWidget* view);
    void resetToView(QWidget* view);

    QWidget* createDetailView(const QString& itemId, const QString& itemName = "", const MediaItem& seedItem = {});
    QWidget* createCategoryView(const QString& categoryId, const QString& title = "");
    QWidget* createLibraryView(const QString& libraryId, const QString& title = "");
    QWidget* createPersonView(const QString& personId, const QString& personName = "");
    QWidget* createSearchView(const QString& query);
    QWidget* createFilteredView(const QString& filterType, const QString& filterValue);
    QWidget* createPlayerView(const QString& mediaId, const QString& title, const QString& streamUrl, long long startPositionTicks, const QVariant &extraData);
    QWidget* createSettingsView();
    QWidget* createManageView();
    // 阶段5：单个服务器的全部结果页（从聚合视图点击 section header 进入）。
    QWidget* createServerScopedView(const ServerProfile& profile,
                                    const QList<MediaItem>& items,
                                    const QString& title);


    QWidget* createSeasonView(const QString& seriesId, const QString& seasonId, const QString& seasonName);

    
    QCoro::Task<void> trySwitchToServer(const QString& serverId, const QString& displayName);
    QCoro::Task<void> verifyServerReachability(const ServerProfile& target,
                                                int generation,
                                                const QString& displayName);
    QListWidgetItem* m_serverSwitcherHoverItem = nullptr;
    QPointer<QWidget> m_serverSwitcherViewport;

    
    void launchPlayer(const QString& mediaId, const QString& title, const QString& streamUrl, long long startPositionTicks, const QVariant& extraData);

    QEmbyCore* m_core;

    SlidingStackedWidget* m_contentSwitcher = nullptr;
    bool m_isDestroying = false;
    QStack<RouteInfo> m_navStack; 

    DashboardView* m_dashboardView = nullptr;
    FavoritesView* m_favoritesView = nullptr;
    AggregatedSearchView* m_aggregatedSearchView = nullptr;
    AggregatedHistoryView* m_aggregatedHistoryView = nullptr;
    AggregatedFavoritesView* m_aggregatedFavoritesView = nullptr;

    QWidget* m_sidebar = nullptr;
    QWidget* m_edgeTrigger = nullptr;
    QPropertyAnimation* m_sidebarAnim = nullptr;
    QTimer* m_sidebarAutoHideTimer = nullptr;   
    bool m_sidebarOnRight = false;
    bool m_sidebarPinned = false;
    bool m_sidebarPinnedApplied = false;
    QHBoxLayout* m_contentLayout = nullptr;
    QVBoxLayout* m_sidebarFooterActionsLayout = nullptr;
    QLineEdit* m_searchBox = nullptr;
    QAction* m_searchAction = nullptr;
    QCompleter* m_searchCompleter = nullptr;    qint64 m_historyTermActivatedMs = 0;  // 历史条目点击时间（穿透保护窗口起点）

    QStringListModel* m_searchHistoryModel = nullptr;

    // 阶段6：搜索历史下拉（chip 面板，复用 SearchHistoryPopup 组件）。
    SearchHistoryPopup* m_searchHistoryPopup = nullptr;          // 当前服搜索框
    SearchHistoryPopup* m_aggregatedSearchHistoryPopup = nullptr; // 聚合搜索框

    QPushButton* m_btnHome = nullptr;
    QPushButton* m_btnFavorites = nullptr;
    QWidget* m_navArea = nullptr;
    QWidget* m_searchSpacer = nullptr;

    // —— 聚合分组（侧边栏：服务器信息 → 聚合分组 → 当前服分组 → 媒体库）——
    QLabel* m_aggregateGroupTitle = nullptr;      // "AGGREGATE" 分组标题
    QLineEdit* m_aggregatedSearchBox = nullptr;   // 聚合搜索输入框
    QPushButton* m_btnAggregatedHistory = nullptr;// 聚合历史按钮
    QPushButton* m_btnAggregatedFavorites = nullptr; // 聚合收藏按钮
    QLabel* m_currentServerLabel = nullptr;       // "当前服 · 服名" 分组标题

    // Read-only server info card pinned to the top of the sidebar (icon +
    // name + address). The switching entry point lives in the mainwindow
    // titlebar server pill; this card is display-only.
    QWidget* m_serverInfoWidget = nullptr;
    QLabel* m_serverIconLabel = nullptr;
    QBoxLayout* m_serverInfoLayout = nullptr;
    QVBoxLayout* m_serverNameLayout = nullptr;
    ElidedLabel* m_serverNameLabel = nullptr;
    ElidedLabel* m_serverAddressLabel = nullptr;

    QListWidget* m_libraryList = nullptr;
    SmoothScrollController* m_sidebarLibraryScrollController = nullptr;

    QLabel* m_userAvatarLabel = nullptr;
    QHBoxLayout* m_userInfoLayout = nullptr;
    ElidedLabel* m_userNameLabel = nullptr;
    QPushButton* m_btnCloudSync = nullptr;
    QPushButton* m_btnSettings = nullptr;
    QPushButton* m_btnManage = nullptr;
    QPushButton* m_btnDownloads = nullptr;
    QPushButton* m_btnLogout = nullptr;
    WebdavProfileStore* m_webdavStore = nullptr;

    std::optional<QCoro::Task<void>> m_pendingProfileRefreshTask;
    int m_profileRefreshGeneration = 0;
    // Independent generation counter for server-reachability probes.
    // Must NOT share m_profileRefreshGeneration: trySwitchToServer starts
    // the probe right after setActiveServer, and a shared counter would
    // invalidate the sidebar refresh (generation bumped mid-flight) so the
    // new server's libraries never get applied.
    int m_serverProbeGeneration = 0;
    QString m_sidebarLibraryServerId;
    QString m_sidebarLibraryUserId;
    QString m_lastRouteType;
};

#endif 
