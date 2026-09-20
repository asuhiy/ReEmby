#include "homeview.h"
#include "../../utils/qcoroutil.h"
#include "../../components/searchcompleterpopup.h"
#include "../../components/searchhistorypopup.h"
#include "../../components/downloadmanagerdialog.h"
#include "../../components/elidedlabel.h"
#include "../../components/moderntoast.h"
#include "../../components/slidingstackedwidget.h"
#include "../../components/webdavsyncdialog.h"
#include "../../managers/thememanager.h"
#include "../../managers/searchhistorymanager.h"
#include "../../managers/playbackmanager.h"
#include "../../utils/smoothscrollcontroller.h"
#include "../admin/manageview.h" 
#include "../media/detailview.h"
#include "../media/libraryview.h"
#include "../media/playerview.h"
#include "../media/seasonview.h" 
#include "../search/searchview.h"
#include "../settings/settingsview.h"
#include "categoryview.h"
#include "config/config_keys.h"
#include "config/configstore.h"
#include "config/webdavprofilestore.h"
#include "dashboardview.h"
#include "favoritesview.h"
#include "aggregatedviewbase.h"
#include "aggregatedsearchview.h"
#include "aggregatedhistoryview.h"
#include "aggregatedfavoritesview.h"
#include "serverscopedview.h"
#include <QAction>
#include <QApplication>
#include <QAbstractItemView>
#include <QBoxLayout>
#include <QCompleter>
#include <functional>
#include <QCursor>
#include <QDebug>
#include <QEvent>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMargins>
#include <QMouseEvent>
#include <QPointer> 
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSize>
#include <QShowEvent>
#include <QStringListModel>
#include <QDateTime>
#include <QTimer>
#include <QKeyEvent>
#include <QHoverEvent>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <models/profile/serverprofile.h>
#include <qembycore.h>
#include <qcorotask.h>
#include <api/networkmanager.h>
#include <services/manager/servermanager.h>
#include <services/media/mediaservice.h>

namespace
{
constexpr int kFloatingSidebarWidth = 240;
constexpr int kPinnedSidebarWidth = 136;
constexpr int kSidebarHiddenOffset = 30;
constexpr int kLeftEdgeTriggerWidth = 15;
constexpr int kRightEdgeTriggerWidth = 20;
constexpr int kSidebarLibraryNameRole = Qt::UserRole + 1;

// Captures left-button press anywhere in a server-switcher row (including
// its sub-widgets) and dispatches the click. Needed because QListWidget's
// itemClicked signal is never emitted when a sub-widget inside a
// setItemWidget() row swallows the event.
class RowClickFilter : public QObject
{
public:
    std::function<void()> onClick;
    explicit RowClickFilter(QObject *parent) : QObject(parent) {}
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton && onClick) {
                onClick();
                return true;  // consume the press
            }
        }
        return QObject::eventFilter(watched, event);
    }
};
} 

HomeView::HomeView(QEmbyCore *core, QWidget *parent) : QWidget(parent), m_core(core)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName("home-view");

    
    QString pos = ConfigStore::instance()->get<QString>(ConfigKeys::SidebarPosition, "left");
    m_sidebarOnRight = (pos == "right");
    m_sidebarPinned = ConfigStore::instance()->get<bool>(ConfigKeys::SidebarPinned, false);

    setupUi();
    if (!m_sidebarPinned)
    {
        hideSidebar();
    }

    connect(ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](ThemeManager::Theme) { applySidebarIcons(); });

    
    PlaybackManager::instance()->init(m_core);
    connect(PlaybackManager::instance(), &PlaybackManager::requestEmbeddedPlay, this,
            [this](const QString &id, const QString &title, const QString &url, long long ticks,
                   const QVariant &extraData) { pushView(createPlayerView(id, title, url, ticks, extraData)); });

    
    
    connect(PlaybackManager::instance(), &PlaybackManager::playbackFinished, this,
            [this]()
            {
                QWidget *current = m_contentSwitcher->currentWidget();
                if (current)
                {
                    QShowEvent showEvent;
                    QCoreApplication::sendEvent(current, &showEvent);
                }
            });
}

HomeView::~HomeView()
{
    m_isDestroying = true;

    if (m_contentSwitcher)
    {
        disconnect(m_contentSwitcher, nullptr, this, nullptr);
        const auto playerViews = m_contentSwitcher->findChildren<PlayerView *>();
        for (PlayerView *playerView : playerViews)
        {
            disconnect(playerView, nullptr, this, nullptr);
        }
    }
}


PlayerView *HomeView::activePlayerView() const
{
    if (m_isDestroying || !m_contentSwitcher)
    {
        return nullptr;
    }

    QWidget *current = m_contentSwitcher->currentWidget();
    if (current && current->property("routeType").toString() == "PlayerView")
    {
        return qobject_cast<PlayerView *>(current);
    }
    return nullptr;
}

void HomeView::setupUi()
{
    this->setProperty("showGlobalSearch", true);
    // Drop the global app-name title — the sidebar carries the active server
    // name instead (see setupSidebar); only secondary views (login, etc.)
    // set their own viewTitle from elsewhere.
    this->setProperty("viewTitle", QString());
    this->setProperty("showGlobalBack", true);
    this->setProperty("showGlobalHome", true);
    this->setProperty("showGlobalFav", true);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    
    m_contentLayout = new QHBoxLayout();
    m_contentLayout->setContentsMargins(0, 0, 0, 0);
    m_contentLayout->setSpacing(0);

    m_contentSwitcher = new SlidingStackedWidget(this);
    m_contentSwitcher->setObjectName("home-content");

    
    m_dashboardView = new DashboardView(m_core, this);
    m_favoritesView = new FavoritesView(m_core, this);

    // —— 聚合视图（跨服务器搜索/历史/收藏），常驻 viewstack 保留状态 ——
    m_aggregatedSearchView = new AggregatedSearchView(m_core, this);
    m_aggregatedSearchView->setProperty("routeType", "AggregatedSearchView");
    m_aggregatedSearchView->setProperty("showGlobalBack", true);
    m_aggregatedSearchView->setProperty("showGlobalHome", true);
    m_aggregatedSearchView->setProperty("showGlobalFav", true);

    m_aggregatedHistoryView = new AggregatedHistoryView(m_core, this);
    m_aggregatedHistoryView->setProperty("routeType", "AggregatedHistoryView");
    m_aggregatedHistoryView->setProperty("showGlobalBack", true);
    m_aggregatedHistoryView->setProperty("showGlobalHome", true);
    m_aggregatedHistoryView->setProperty("showGlobalFav", true);

    m_aggregatedFavoritesView = new AggregatedFavoritesView(m_core, this);
    m_aggregatedFavoritesView->setProperty("routeType", "AggregatedFavoritesView");
    m_aggregatedFavoritesView->setProperty("showGlobalBack", true);
    m_aggregatedFavoritesView->setProperty("showGlobalHome", true);
    m_aggregatedFavoritesView->setProperty("showGlobalFav", true);

    connect(m_dashboardView, &DashboardView::navigateToLibrary, this,
            [this](const QString &id, const QString &name)
            {
                m_libraryList->clearSelection();

                
                for (int i = 0; i < m_libraryList->count(); ++i)
                {
                    if (m_libraryList->item(i)->data(Qt::UserRole).toString() == id)
                    {
                        m_libraryList->item(i)->setSelected(true);
                        break;
                    }
                }
                pushView(createLibraryView(id, name));
            });

    connect(m_dashboardView, &DashboardView::navigateToCategory, this,
            [this](const QString &categoryId, const QString &title)
            {
                m_libraryList->clearSelection();
                pushView(createCategoryView(categoryId, title));
            });

    connect(m_favoritesView, &FavoritesView::navigateToCategory, this,
            [this](const QString &categoryId, const QString &title)
            {
                m_libraryList->clearSelection();
                pushView(createCategoryView("Favorite_" + categoryId, title));
            });

    auto navigateToDetailSlot = [this](const QString &itemId, const QString &itemName, const MediaItem &seedItem)
    { pushView(createDetailView(itemId, itemName, seedItem)); };

    connect(m_dashboardView, &DashboardView::navigateToDetail, this, navigateToDetailSlot);
    connect(m_favoritesView, &FavoritesView::navigateToDetail, this, navigateToDetailSlot);
    auto navigateToFolderSlot = [this](const QString &id, const QString &name)
    { pushView(createLibraryView(id, name)); };
    connect(m_favoritesView, &FavoritesView::navigateToFolder, this, navigateToFolderSlot);
    auto navigateToPersonSlot = [this](const QString &id, const QString &name)
    { pushView(createPersonView(id, name)); };
    connect(m_favoritesView, &FavoritesView::navigateToPerson, this, navigateToPersonSlot);

    
    
    
    auto navigateToPlayerSlot = [this](const QString &id, const QString &title, const QString &url, long long ticks,
                                       const QVariant &extraData) { launchPlayer(id, title, url, ticks, extraData); };
    connect(m_dashboardView, &BaseView::navigateToPlayer, this, navigateToPlayerSlot);
    connect(m_favoritesView, &BaseView::navigateToPlayer, this, navigateToPlayerSlot);

    
    auto navigateToSeasonSlot = [this](const QString &seriesId, const QString &seasonId, const QString &seasonName)
    { pushView(createSeasonView(seriesId, seasonId, seasonName)); };
    connect(m_dashboardView, &BaseView::navigateToSeason, this, navigateToSeasonSlot);
    connect(m_favoritesView, &BaseView::navigateToSeason, this, navigateToSeasonSlot);

    // 聚合搜索视图的导航信号复用同一套路由。
    connect(m_aggregatedSearchView, &BaseView::navigateToDetail, this, navigateToDetailSlot);
    connect(m_aggregatedSearchView, &BaseView::navigateToPlayer, this, navigateToPlayerSlot);
    connect(m_aggregatedSearchView, &BaseView::navigateToSeason, this, navigateToSeasonSlot);
    connect(m_aggregatedHistoryView, &BaseView::navigateToDetail, this, navigateToDetailSlot);
    connect(m_aggregatedHistoryView, &BaseView::navigateToPlayer, this, navigateToPlayerSlot);
    connect(m_aggregatedHistoryView, &BaseView::navigateToSeason, this, navigateToSeasonSlot);
    connect(m_aggregatedFavoritesView, &BaseView::navigateToDetail, this, navigateToDetailSlot);
    connect(m_aggregatedFavoritesView, &BaseView::navigateToPlayer, this, navigateToPlayerSlot);
    connect(m_aggregatedFavoritesView, &BaseView::navigateToSeason, this, navigateToSeasonSlot);

    // 阶段5：点击聚合结果中的服务器 section header → 打开该服务器的全部结果页。
    // 数据直接复用聚合视图已加载的结果（不重新请求），面包屑标题由子类提供
    // （搜索：xxx / 继续观看 / 收藏）。
    auto openServerScoped = [this](AggregatedViewBase* src,
                                   const ServerProfile& profile) {
        pushView(createServerScopedView(
            profile, src->itemsForServer(profile),
            src->scopedPageTitle(profile)));
    };
    connect(m_aggregatedSearchView, &AggregatedViewBase::serverScopedRequested, this,
            [this, openServerScoped](const ServerProfile& p) {
                openServerScoped(m_aggregatedSearchView, p);
            });
    connect(m_aggregatedHistoryView, &AggregatedViewBase::serverScopedRequested, this,
            [this, openServerScoped](const ServerProfile& p) {
                openServerScoped(m_aggregatedHistoryView, p);
            });
    connect(m_aggregatedFavoritesView, &AggregatedViewBase::serverScopedRequested, this,
            [this, openServerScoped](const ServerProfile& p) {
                openServerScoped(m_aggregatedFavoritesView, p);
            });

    m_contentSwitcher->addWidget(m_dashboardView);
    m_contentSwitcher->addWidget(m_favoritesView);
    m_contentSwitcher->addWidget(m_aggregatedSearchView);
    m_contentSwitcher->addWidget(m_aggregatedHistoryView);
    m_contentSwitcher->addWidget(m_aggregatedFavoritesView);
    m_lastRouteType = m_contentSwitcher->currentWidget()
                          ? m_contentSwitcher->currentWidget()->property("routeType").toString()
                          : QString();

    connect(m_contentSwitcher, &QStackedWidget::currentChanged, this,
            [this](int )
            {
                QWidget *current = m_contentSwitcher->currentWidget();
                const QString currentRouteType = current ? current->property("routeType").toString() : QString();

                if (m_lastRouteType == "ManageView" && currentRouteType != "ManageView")
                {
                    qDebug() << "[HomeView] Left management view — refreshing sidebar library list";
                    scheduleProfileRefresh();
                }
                m_lastRouteType = currentRouteType;

                Q_EMIT homeContentSwitched();

                if (current)
                {
                    bool isImmersive = current->property("isImmersive").toBool();
                    syncSidebarVisibility();
                    
                    Q_EMIT immersiveStateChanged(isImmersive);
                }
            });

    m_contentLayout->addWidget(m_contentSwitcher, 1);
    mainLayout->addLayout(m_contentLayout);

    
    m_navStack.clear();

    setupSidebar();

    // 聚合侧边栏入口 → 切换到聚合视图。
    // 阶段3：聚合搜索。阶段4：聚合历史/收藏视图（挂到对应信号后实现）。
    connect(this, &HomeView::aggregatedSearchRequested, this,
            [this](const QString& query) {
                if (!m_aggregatedSearchView) return;
                const QString trimmed = query.trimmed();
                if (trimmed.isEmpty()) return;
                // 记录聚合搜索历史（__aggregated__ 桶，所有服务器共享）。
                SearchHistoryManager::instance()->recordSearch(
                    SearchHistoryManager::aggregatedBucket(), trimmed);
                if (m_contentSwitcher->currentWidget() != m_aggregatedSearchView) {
                    // pushView 保留返回栈：聚合结果 → 返回箭头 → 搜索前页面。
                    pushView(m_aggregatedSearchView);
                }
                m_aggregatedSearchView->search(trimmed);
            });
    connect(this, &HomeView::aggregatedHistoryRequested, this,
            [this]() {
                if (!m_aggregatedHistoryView) return;
                if (m_contentSwitcher->currentWidget() != m_aggregatedHistoryView) {
                    pushView(m_aggregatedHistoryView);
                }
            });
    connect(this, &HomeView::aggregatedFavoritesRequested, this,
            [this]() {
                if (!m_aggregatedFavoritesView) return;
                if (m_contentSwitcher->currentWidget() != m_aggregatedFavoritesView) {
                    pushView(m_aggregatedFavoritesView);
                }
            });

    m_edgeTrigger = new QWidget(this);
    m_edgeTrigger->setFixedWidth(15);
    m_edgeTrigger->setCursor(Qt::PointingHandCursor);
    m_edgeTrigger->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    m_edgeTrigger->installEventFilter(this);

    // 全局事件过滤：监听所有应用的 mouse press，搜索历史 popup 打开时
    // 点击 popup 外部 → 自动 dismiss（SearchHistoryPopup 是 QFrame 不是
    // Qt::Popup 窗口，不会自动 deactivate）。
    qApp->installEventFilter(this);

    
    
    
    if (m_sidebarPinned)
    {
        
        m_sidebar->hide();
        m_edgeTrigger->hide();
    }
}


QWidget *HomeView::createDetailView(const QString &itemId, const QString &itemName, const MediaItem &seedItem)
{
    auto *view = new DetailView(m_core, this);

    view->setProperty("isDynamic", true);
    view->setProperty("routeType", "DetailView");
    view->setProperty("routeId", itemId);
    view->setProperty("routeTitle", itemName);

    
    
    view->loadItem(itemId, seedItem);

    connect(view, &DetailView::navigateToDetail, this,
            [this](const QString &id, const QString &name, const MediaItem &seed) { pushView(createDetailView(id, name, seed)); });
    connect(view, &DetailView::navigateToFolder, this,
            [this](const QString &id, const QString &name) { pushView(createLibraryView(id, name)); });
    connect(view, &BaseView::navigateToPerson, this,
            [this](const QString &id, const QString &name)
            {
                pushView(createPersonView(id, name)); 
            });

    connect(view, &BaseView::navigateToPlayer, this,
            [this](const QString &id, const QString &title, const QString &url, long long ticks,
                   const QVariant &extraData) { launchPlayer(id, title, url, ticks, extraData); });

    
    connect(view, &BaseView::navigateToSeason, this,
            [this](const QString &seriesId, const QString &seasonId, const QString &seasonName)
            { pushView(createSeasonView(seriesId, seasonId, seasonName)); });

    connect(view, &DetailView::triggerSearch, this, &HomeView::triggerSearch);
    connect(view, &BaseView::navigateToFilteredView, this,
            [this](const QString &type, const QString &value)
            { pushView(createFilteredView(type, value)); });

    return view;
}

QWidget *HomeView::createServerScopedView(const ServerProfile& profile,
                                          const QList<MediaItem>& items,
                                          const QString& title)
{
    auto *view = new ServerScopedView(m_core, this);
    view->setProperty("isDynamic", true);
    view->setProperty("routeType", "ServerScopedView");
    view->setProperty("routeId", profile.id);
    view->setProperty("routeTitle", title);

    view->setContext(profile, items, title);

    // 详情/播放/剧季/返回复用 BaseView 统一路由（不切服）。
    connect(view, &BaseView::navigateToDetail, this,
            [this](const QString &id, const QString &name, const MediaItem &seed) { pushView(createDetailView(id, name, seed)); });
    connect(view, &BaseView::navigateToFolder, this,
            [this](const QString &id, const QString &name) { pushView(createLibraryView(id, name)); });
    connect(view, &BaseView::navigateToPerson, this,
            [this](const QString &id, const QString &name) { pushView(createPersonView(id, name)); });
    connect(view, &BaseView::navigateToPlayer, this,
            [this](const QString &id, const QString &title, const QString &url, long long ticks,
                   const QVariant &extraData) { launchPlayer(id, title, url, ticks, extraData); });
    connect(view, &BaseView::navigateToSeason, this,
            [this](const QString &seriesId, const QString &seasonId, const QString &seasonName)
            { pushView(createSeasonView(seriesId, seasonId, seasonName)); });
    connect(view, &BaseView::navigateBack, this, &HomeView::navigateBack);

    return view;
}

QWidget *HomeView::createCategoryView(const QString &categoryId, const QString &title)
{
    auto *view = new CategoryView(m_core, this);
    view->setProperty("isDynamic", true);
    view->setProperty("routeType", "CategoryView");
    view->setProperty("routeId", categoryId);
    view->setProperty("routeTitle", title);

    QTimer::singleShot(0, view, [view, categoryId, title]()
                       { launchTask(view->loadCategory(categoryId, title), view); });

    connect(view, &CategoryView::navigateToDetail, this,
            [this](const QString &id, const QString &name, const MediaItem &seed) { pushView(createDetailView(id, name, seed)); });

    connect(view, &CategoryView::navigateToFolder, this,
            [this](const QString &id, const QString &name) { pushView(createLibraryView(id, name)); });

    connect(view, &CategoryView::navigateToPerson, this,
            [this](const QString &id, const QString &name) { pushView(createPersonView(id, name)); });

    
    connect(view, &BaseView::navigateToPlayer, this,
            [this](const QString &id, const QString &title, const QString &url, long long ticks,
                   const QVariant &extraData) { launchPlayer(id, title, url, ticks, extraData); });

    
    connect(view, &BaseView::navigateToSeason, this,
            [this](const QString &seriesId, const QString &seasonId, const QString &seasonName)
            { pushView(createSeasonView(seriesId, seasonId, seasonName)); });

    return view;
}

QWidget *HomeView::createLibraryView(const QString &libraryId, const QString &title)
{
    auto *view = new LibraryView(m_core, this);
    view->setProperty("isDynamic", true);
    view->setProperty("routeType", "LibraryView");
    view->setProperty("routeId", libraryId);
    view->setProperty("routeTitle", title);

    view->loadLibrary(libraryId, title);

    connect(view, &LibraryView::navigateToDetail, this,
            [this](const QString &id, const QString &name, const MediaItem &seed) { pushView(createDetailView(id, name, seed)); });

    connect(view, &LibraryView::navigateToFolder, this,
            [this](const QString &id, const QString &name) { pushView(createLibraryView(id, name)); });

    connect(view, &BaseView::navigateToPerson, this,
            [this](const QString &id, const QString &name) { pushView(createPersonView(id, name)); });

    
    connect(view, &BaseView::navigateToPlayer, this,
            [this](const QString &id, const QString &title, const QString &url, long long ticks,
                   const QVariant &extraData) { launchPlayer(id, title, url, ticks, extraData); });

    
    connect(view, &BaseView::navigateToSeason, this,
            [this](const QString &seriesId, const QString &seasonId, const QString &seasonName)
            { pushView(createSeasonView(seriesId, seasonId, seasonName)); });

    return view;
}


QWidget *HomeView::createPersonView(const QString &personId, const QString &personName)
{
    
    auto *view = new LibraryView(m_core, this);
    view->setProperty("isDynamic", true);
    view->setProperty("routeType", "PersonView"); 
    view->setProperty("routeId", personId);
    view->setProperty("routeTitle", personName);

    
    
    
    connect(view, &BaseView::navigateBack, this, &HomeView::navigateBack);

    
    connect(view, &BaseView::navigateToDetail, this,
            [this](const QString &id, const QString &name, const MediaItem &seed) { pushView(createDetailView(id, name, seed)); });

    
    connect(view, &BaseView::navigateToPerson, this,
            [this](const QString &id, const QString &name) { pushView(createPersonView(id, name)); });

    
    connect(view, &BaseView::navigateToPlayer, this,
            [this](const QString &id, const QString &title, const QString &url, long long ticks,
                   const QVariant &extraData) { launchPlayer(id, title, url, ticks, extraData); });

    
    connect(view, &BaseView::navigateToSeason, this,
            [this](const QString &seriesId, const QString &seasonId, const QString &seasonName)
            { pushView(createSeasonView(seriesId, seasonId, seasonName)); });

    
    connect(view, &BaseView::triggerSearch, this, [this](const QString &query) { triggerSearch(query); });

    
    
    
    view->loadPerson(personId, personName);

    return view;
}

QWidget *HomeView::createSearchView(const QString &query)
{
    auto *view = new SearchView(m_core, this);
    view->setProperty("isDynamic", true);
    view->setProperty("routeType", "SearchView");
    view->setProperty("routeId", query);

    view->performSearch(query);

    connect(view, &SearchView::navigateToDetail, this,
            [this](const QString &id, const QString &name, const MediaItem &seed) { pushView(createDetailView(id, name, seed)); });

    connect(view, &SearchView::navigateToFolder, this,
            [this](const QString &id, const QString &name) { pushView(createLibraryView(id, name)); });

    connect(view, &SearchView::navigateToPerson, this,
            [this](const QString &id, const QString &name) { pushView(createPersonView(id, name)); });

    
    connect(view, &BaseView::navigateToPlayer, this,
            [this](const QString &id, const QString &title, const QString &url, long long ticks,
                   const QVariant &extraData) { launchPlayer(id, title, url, ticks, extraData); });

    
    connect(view, &BaseView::navigateToSeason, this,
            [this](const QString &seriesId, const QString &seasonId, const QString &seasonName)
            { pushView(createSeasonView(seriesId, seasonId, seasonName)); });

    return view;
}




QWidget *HomeView::createFilteredView(const QString &filterType, const QString &filterValue)
{
    auto *view = new LibraryView(m_core, this);
    view->setProperty("isDynamic", true);
    view->setProperty("routeType", "FilteredView");
    view->setProperty("routeId", filterType + ":" + filterValue);
    view->setProperty("routeTitle", filterValue);
    view->setProperty("routeExtraId", filterType);

    connect(view, &BaseView::navigateToDetail, this,
            [this](const QString &id, const QString &name, const MediaItem &seed) { pushView(createDetailView(id, name, seed)); });
    connect(view, &BaseView::navigateToFolder, this,
            [this](const QString &id, const QString &name) { pushView(createLibraryView(id, name)); });
    connect(view, &BaseView::navigateToPerson, this,
            [this](const QString &id, const QString &name) { pushView(createPersonView(id, name)); });
    connect(view, &BaseView::navigateToPlayer, this,
            [this](const QString &id, const QString &title, const QString &url, long long ticks,
                   const QVariant &extraData) { launchPlayer(id, title, url, ticks, extraData); });
    connect(view, &BaseView::navigateToSeason, this,
            [this](const QString &seriesId, const QString &seasonId, const QString &seasonName)
            { pushView(createSeasonView(seriesId, seasonId, seasonName)); });

    view->loadFiltered(filterType, filterValue);

    return view;
}




QWidget *HomeView::createSeasonView(const QString &seriesId, const QString &seasonId, const QString &seasonName)
{
    auto *view = new SeasonView(m_core, this);
    view->setProperty("isDynamic", true);
    view->setProperty("routeType", "SeasonView");
    view->setProperty("routeId", seasonId);
    
    view->setProperty("routeExtraId", seriesId);
    view->setProperty("routeTitle", seasonName);

    
    view->loadSeason(seriesId, seasonId, seasonName);

    connect(view, &BaseView::navigateBack, this, &HomeView::navigateBack);

    
    connect(view, &BaseView::navigateToDetail, this,
            [this](const QString &id, const QString &name, const MediaItem &seed) { pushView(createDetailView(id, name, seed)); });

    
    connect(view, &BaseView::navigateToPlayer, this,
            [this](const QString &id, const QString &title, const QString &url, long long ticks,
                   const QVariant &extraData) { launchPlayer(id, title, url, ticks, extraData); });

    return view;
}

QWidget *HomeView::createPlayerView(const QString &mediaId, const QString &title, const QString &streamUrl,
                                    long long startPositionTicks, const QVariant &extraData)
{
    auto *view = new PlayerView(m_core, this);
    view->setProperty("isDynamic", true);
    view->setProperty("routeType", "PlayerView");
    view->setProperty("routeId", mediaId);
    view->setProperty("routeTitle", title);

    
    connect(view, &BaseView::navigateBack, this, &HomeView::navigateBack);
    connect(view, &PlayerView::playerChromeVisibilityChanged, this,
            [this, view](bool visible)
            {
                if (m_isDestroying || !m_contentSwitcher)
                {
                    return;
                }

                if (activePlayerView() == view)
                {
                    Q_EMIT playerChromeVisibilityChanged(visible);
                }
            });

    
    
    
    
    QPointer<PlayerView> safeView(view);
    auto launchPlay = [safeView, mediaId, title, streamUrl, startPositionTicks, extraData]()
    {
        if (safeView)
        {
            safeView->playMedia(mediaId, title, streamUrl, startPositionTicks, extraData);
        }
    };

    if (m_contentSwitcher)
    {
        
        
        
        connect(m_contentSwitcher, &SlidingStackedWidget::animationFinished, view,
                launchPlay, Qt::SingleShotConnection);
    }
    else
    {
        
        launchPlay();
    }

    return view;
}




void HomeView::launchPlayer(const QString &mediaId, const QString &title, const QString &streamUrl,
                            long long startPositionTicks, const QVariant &extraData)
{
    PlaybackManager::instance()->startPlayback(mediaId, title, streamUrl, startPositionTicks, extraData);
}

QWidget *HomeView::createSettingsView()
{
    auto *view = new SettingsView(m_core, this);
    view->setProperty("isDynamic", true);
    view->setProperty("routeType", "SettingsView");
    view->setProperty("routeId", "settings_global");
    view->setProperty("routeTitle", tr("Settings"));

    
    connect(view, &BaseView::navigateBack, this, &HomeView::navigateBack);

    return view;
}

QWidget *HomeView::createManageView()
{
    auto *view = new ManageView(m_core, this);
    view->setProperty("isDynamic", true);
    view->setProperty("routeType", "ManageView");
    view->setProperty("routeId", "manage_global");
    view->setProperty("routeTitle", tr("Server Management"));

    connect(view, &BaseView::navigateBack, this, &HomeView::navigateBack);

    return view;
}

void HomeView::setupSidebar()
{
    m_sidebar = new QWidget(this);
    m_sidebar->setObjectName("floating-sidebar");
    m_sidebar->setProperty("sidebarSide", m_sidebarOnRight ? "right" : "left");

    
    if (!m_sidebarPinned)
    {
        auto *shadow = new QGraphicsDropShadowEffect(this);
        shadow->setBlurRadius(25);
        shadow->setColor(QColor(0, 0, 0, 30));
        shadow->setOffset(m_sidebarOnRight ? -4 : 4, 0);
        m_sidebar->setGraphicsEffect(shadow);
    }

    auto *layout = new QVBoxLayout(m_sidebar);
    layout->setContentsMargins(16, 20, 0, 20);
    layout->setSpacing(6);

    
    // Read-only server info card at the very top of the sidebar: icon +
    // name + address of the currently active server. The switch entry point
    // is the titlebar server pill in MainWindow; this card is display-only
    // (no cursor / eventFilter / click handling on purpose).
    auto *serverInfoWidget = new QWidget(m_sidebar);
    m_serverInfoWidget = serverInfoWidget;
    serverInfoWidget->setObjectName(QStringLiteral("sidebar-server-info"));
    m_serverInfoLayout = new QBoxLayout(QBoxLayout::LeftToRight, serverInfoWidget);
    m_serverInfoLayout->setContentsMargins(8, 0, 8, 10);
    m_serverInfoLayout->setSpacing(10);

    m_serverIconLabel = new QLabel(serverInfoWidget);
    m_serverIconLabel->setFixedSize(32, 32);
    m_serverIconLabel->setScaledContents(true);

    m_serverNameLayout = new QVBoxLayout();
    m_serverNameLayout->setContentsMargins(0, 0, 0, 0);
    m_serverNameLayout->setSpacing(0);
    m_serverNameLayout->setAlignment(Qt::AlignVCenter);

    m_serverNameLabel = new ElidedLabel(serverInfoWidget);
    m_serverNameLabel->setObjectName("sidebar-server-name");

    m_serverAddressLabel = new ElidedLabel(serverInfoWidget);
    m_serverAddressLabel->setObjectName("sidebar-server-address");

    m_serverNameLayout->addWidget(m_serverNameLabel);
    m_serverNameLayout->addWidget(m_serverAddressLabel);

    m_serverInfoLayout->addWidget(m_serverIconLabel, 0, Qt::AlignVCenter);
    m_serverInfoLayout->addLayout(m_serverNameLayout);
    layout->addWidget(serverInfoWidget);

    // —— 中部滚动区 ——
    // 聚合 / 当前服 / 媒体库这几组内容在窗口高度不足时需要滚动：否则
    // QVBoxLayout 会把子控件强行压缩到各自 minimum 之下——按钮被压扁、文字
    // 互相重叠（用户截图反馈）。顶部服务器卡与底部用户/操作区保持固定。
    auto *sidebarScroll = new QScrollArea(m_sidebar);
    sidebarScroll->setObjectName("sidebar-scroll");
    sidebarScroll->setFrameShape(QFrame::NoFrame);
    sidebarScroll->setWidgetResizable(true);
    sidebarScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sidebarScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    // 关闭 viewport 自动填充（配合 QSS 的 transparent），让侧边栏背景透出来。
    sidebarScroll->viewport()->setAutoFillBackground(false);

    auto *sidebarScrollContent = new QWidget();
    sidebarScrollContent->setObjectName("sidebar-scroll-content");
    auto *scrollLayout = new QVBoxLayout(sidebarScrollContent);
    scrollLayout->setContentsMargins(0, 0, 0, 0);
    scrollLayout->setSpacing(6);

    // —— 聚合分组：跨服务器搜索/历史/收藏 ——
    // 布局顺序（用户确认）：服务器信息 → 聚合分组 → 当前服分组 → 媒体库。
    // 聚合搜索是输入框（跨所有已添加服务器），聚合历史/收藏是按钮（点击进入）。
    m_aggregateGroupTitle = new QLabel(tr("聚合"), m_sidebar);
    m_aggregateGroupTitle->setObjectName("sidebar-title");
    scrollLayout->addWidget(m_aggregateGroupTitle);

    m_aggregatedSearchBox = new QLineEdit(m_sidebar);
    m_aggregatedSearchBox->setObjectName("sidebar-search");
    m_aggregatedSearchBox->setPlaceholderText(tr("聚合搜索..."));
    m_aggregatedSearchBox->setClearButtonEnabled(true);
    m_aggregatedSearchBox->setFocusPolicy(Qt::ClickFocus);
    scrollLayout->addWidget(m_aggregatedSearchBox);

m_btnAggregatedHistory = new QPushButton(tr("聚合历史"), m_sidebar);
m_btnAggregatedFavorites = new QPushButton(tr("聚合收藏"), m_sidebar);
    m_btnAggregatedHistory->setObjectName("sidebar-btn");
    m_btnAggregatedFavorites->setObjectName("sidebar-btn");
    m_btnAggregatedHistory->setCursor(Qt::PointingHandCursor);
    m_btnAggregatedFavorites->setCursor(Qt::PointingHandCursor);
    scrollLayout->addWidget(m_btnAggregatedHistory);
    scrollLayout->addWidget(m_btnAggregatedFavorites);

    // 聚合搜索回车 → 触发跨服务器搜索（阶段3 连接 aggregatedSearchRequested）。
    connect(m_aggregatedSearchBox, &QLineEdit::returnPressed, this,
            [this]() {
                const QString query = m_aggregatedSearchBox->text().trimmed();
                if (query.isEmpty()) return;
                Q_EMIT aggregatedSearchRequested(query);
            });
    connect(m_btnAggregatedHistory, &QPushButton::clicked, this,
            [this]() {
                // 历史条目点击穿透保护：popup 收起动画期间 release 可能
                // 落到本按钮（用户点历史条目却被误触发跳聚合历史）。
                if (QDateTime::currentMSecsSinceEpoch() - m_historyTermActivatedMs < 300)
                    return;
                Q_EMIT aggregatedHistoryRequested(); });
    connect(m_btnAggregatedFavorites, &QPushButton::clicked, this,
            [this]() {
                if (QDateTime::currentMSecsSinceEpoch() - m_historyTermActivatedMs < 300)
                    return;
                Q_EMIT aggregatedFavoritesRequested(); });

    // —— 当前服分组标题：显示当前 active server 名称（切服时更新）——
    m_currentServerLabel = new QLabel(tr("当前服"), m_sidebar);
    m_currentServerLabel->setObjectName("sidebar-title");
    m_currentServerLabel->setProperty("isCurrentServerGroup", true);
    scrollLayout->addWidget(m_currentServerLabel);

    m_navArea = new QWidget(m_sidebar);
    auto *navLayout = new QVBoxLayout(m_navArea);
    navLayout->setContentsMargins(0, 0, 16, 0);
    navLayout->setSpacing(0);

    m_searchBox = new QLineEdit(m_navArea);
    m_searchBox->setObjectName("sidebar-search");
    m_searchBox->setPlaceholderText(tr("Search..."));
    m_searchAction = new QAction(this);
    m_searchAction->setText(tr("Search"));
    m_searchBox->addAction(m_searchAction, QLineEdit::LeadingPosition);
    navLayout->addWidget(m_searchBox);
    m_searchSpacer = new QWidget(m_navArea);
    m_searchSpacer->setFixedHeight(10);
    navLayout->addWidget(m_searchSpacer);

    
    connect(m_searchBox, &QLineEdit::returnPressed, this,
            [this]()
            {
                if (m_searchCompleter && m_searchCompleter->popup()) {
                    m_searchCompleter->popup()->hide();
                }
                triggerSearch(m_searchBox->text());
                m_searchBox->clear(); 
                if (m_searchCompleter && m_searchCompleter->popup()) {
                    m_searchCompleter->popup()->hide();
                }
            });
    setupSearchHistory();
    setupSearchHistoryPopups();

    m_btnHome = new QPushButton(tr("Home"), m_navArea);
    m_btnFavorites = new QPushButton(tr("Favorites"), m_navArea);

    m_btnHome->setObjectName("sidebar-btn");

    m_btnFavorites->setObjectName("sidebar-btn");

    navLayout->addWidget(m_btnHome);
    navLayout->addWidget(m_btnFavorites);

    auto *sep1 = new QFrame(m_navArea);
    sep1->setObjectName("sidebar-sep");
    navLayout->addSpacing(3);
    navLayout->addWidget(sep1);
    navLayout->addSpacing(3);

    scrollLayout->addWidget(m_navArea);

    
    auto *libTitle = new QLabel(tr("MEDIA"), m_sidebar);
    libTitle->setObjectName("sidebar-title");
    scrollLayout->addWidget(libTitle);

    m_libraryList = new QListWidget(m_sidebar);
    m_libraryList->setObjectName("sidebar-list");
    m_libraryList->setFocusPolicy(Qt::NoFocus);
    m_libraryList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_libraryList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_libraryList->setTextElideMode(Qt::ElideRight);
    m_libraryList->setWordWrap(false);
    // 滚动区内的列表需要明确最小高度：否则滚动区内容的最小高度算不出来，
    // 窗口变矮时无法触发滚动条（内容会被压扁）。
    m_libraryList->setMinimumHeight(150);
    m_sidebarLibraryScrollController =
        new SmoothScrollController(m_libraryList->verticalScrollBar(), this);
    m_sidebarLibraryScrollController->setDuration(160);
    m_libraryList->viewport()->installEventFilter(this);
    scrollLayout->addWidget(m_libraryList, 1);

    // 滚动区内容定稿并挂进主布局：吸收中部多余空间，窗口变矮时自动出现
    // 滚动条（而不是把控件压变形）。
    sidebarScroll->setWidget(sidebarScrollContent);
    layout->addWidget(sidebarScroll, 1);

    

    auto *sep2 = new QFrame(m_sidebar);
    sep2->setObjectName("sidebar-sep");
    layout->addSpacing(3);
    layout->addWidget(sep2);
    layout->addSpacing(3);

    
    auto *userInfoWidget = new QWidget(m_sidebar);
    m_userInfoLayout = new QHBoxLayout(userInfoWidget);
    m_userInfoLayout->setContentsMargins(0, 0, 0, 10);
    m_userInfoLayout->setSpacing(10);
    m_userInfoLayout->setAlignment(Qt::AlignVCenter);

    m_userAvatarLabel = new QLabel(userInfoWidget);
    m_userAvatarLabel->setFixedSize(20, 20);
    m_userAvatarLabel->setScaledContents(true);

    // Plain account display — no click behaviour. The server-switcher entry
    // point is the titlebar server pill owned by MainWindow (see
    // showServerSwitcher(QWidget*)).
    m_userNameLabel = new ElidedLabel(userInfoWidget);
    m_userNameLabel->setObjectName("sidebar-user-name");
    m_userAvatarLabel->installEventFilter(this);
    m_userAvatarLabel->setCursor(Qt::PointingHandCursor);

    m_btnCloudSync = new QPushButton(userInfoWidget);
    m_btnCloudSync->setObjectName("sidebar-icon-btn");
    m_btnCloudSync->setCursor(Qt::PointingHandCursor);
    m_btnCloudSync->setToolTip(tr("Cloud Sync (WebDAV)"));

    m_btnDownloads = new QPushButton(userInfoWidget);
    m_btnDownloads->setObjectName("sidebar-icon-btn");
    m_btnDownloads->setCursor(Qt::PointingHandCursor);
    m_btnDownloads->setToolTip(tr("Downloads"));

    m_userInfoLayout->addWidget(m_userAvatarLabel);
    m_userInfoLayout->addWidget(m_userNameLabel, 1);
    m_userInfoLayout->addWidget(m_btnCloudSync, 0, Qt::AlignVCenter);
    m_userInfoLayout->addWidget(m_btnDownloads, 0, Qt::AlignVCenter);
    layout->addWidget(userInfoWidget);

    auto *footerActionsWidget = new QWidget(m_sidebar);
    m_sidebarFooterActionsLayout = new QVBoxLayout(footerActionsWidget);
    m_sidebarFooterActionsLayout->setContentsMargins(0, 0, 16, 0);
    m_sidebarFooterActionsLayout->setSpacing(6);

    m_btnSettings = new QPushButton(tr("Settings"), footerActionsWidget);
    m_btnManage = new QPushButton(tr("Manage"), footerActionsWidget);
    m_btnLogout = new QPushButton(tr("Logout"), footerActionsWidget);

    m_btnSettings->setObjectName("sidebar-btn");

    m_btnManage->setObjectName("sidebar-btn");

    m_btnLogout->setObjectName("sidebar-btn-danger");

    m_sidebarFooterActionsLayout->addWidget(m_btnSettings);
    m_sidebarFooterActionsLayout->addWidget(m_btnManage);
    m_sidebarFooterActionsLayout->addWidget(m_btnLogout);
    layout->addWidget(footerActionsWidget);

    
    connect(m_btnLogout, &QPushButton::clicked, this,
            [this]()
            {
                
                if (m_contentSwitcher->currentWidget() != m_dashboardView)
                {
                    resetToView(m_dashboardView);
                }
                Q_EMIT logoutRequested();
            });

    
    connect(m_btnHome, &QPushButton::clicked, this,
            [this]()
            {
                if (!canGoHome())
                {
                    ModernToast::showMessage(tr("Refreshing Home..."), 1000);
                }
                goHome(); 
            });

    connect(m_btnFavorites, &QPushButton::clicked, this,
            [this]()
            {
                if (!canGoFav())
                {
                    ModernToast::showMessage(tr("Refreshing Favorites..."), 1000);
                }
                goFav(); 
            });

    connect(m_btnSettings, &QPushButton::clicked, this,
            [this]()
            {
                m_libraryList->clearSelection();

                
                QWidget *current = m_contentSwitcher->currentWidget();
                if (current && current->property("routeType").toString() == "SettingsView")
                {
                    return;
                }

                
                pushView(createSettingsView());
            });
    connect(m_btnManage, &QPushButton::clicked, this,
            [this]()
            {
                m_libraryList->clearSelection();

                
                QWidget *current = m_contentSwitcher->currentWidget();
                if (current && current->property("routeType").toString() == "ManageView")
                {
                    return;
                }

                pushView(createManageView());
            });
    connect(m_btnCloudSync, &QPushButton::clicked, this,
            [this]()
            {
                if (m_libraryList)
                {
                    m_libraryList->clearSelection();
                }
                openCloudSyncDialog();
            });
    connect(m_btnDownloads, &QPushButton::clicked, this,
            [this]()
            {
                m_libraryList->clearSelection();
                DownloadManagerDialog::showManager(m_core, this);
            });

    connect(m_libraryList, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item)
            {
                QString viewId = item->data(Qt::UserRole).toString();
                QString cleanName = item->data(kSidebarLibraryNameRole).toString();
                if (cleanName.isEmpty())
                {
                    cleanName = item->data(Qt::ToolTipRole).toString();
                }
                if (cleanName.isEmpty())
                {
                    cleanName = item->text();
                }

                QWidget *current = m_contentSwitcher->currentWidget();
                
                if (current && current->property("routeId").toString() == viewId)
                {
                    return;
                }
                pushView(createLibraryView(viewId, cleanName));
            });

    m_sidebarAnim = new QPropertyAnimation(m_sidebar, "pos", this);
    m_sidebarAnim->setDuration(350);
    m_sidebarAnim->setEasingCurve(QEasingCurve::OutCubic);

    m_sidebar->installEventFilter(this);
    m_sidebar->setMouseTracking(true); 

    
    m_sidebarAutoHideTimer = new QTimer(this);
    m_sidebarAutoHideTimer->setSingleShot(true);
    m_sidebarAutoHideTimer->setInterval(5000);
    connect(m_sidebarAutoHideTimer, &QTimer::timeout, this,
            [this]()
            {
                
                QPoint globalPos = QCursor::pos();
                QPoint localPos = m_sidebar->mapFromGlobal(globalPos);
                if (!m_sidebar->rect().contains(localPos))
                {
                    hideSidebar();
                }
                else
                {
                    
                    m_sidebarAutoHideTimer->start();
                }
            });

    
    connect(ConfigStore::instance(), &ConfigStore::valueChanged, this,
            [this](const QString &key, const QVariant &value)
            {
                if (key == ConfigKeys::SidebarPosition)
                {
                    m_sidebarOnRight = (value.toString() == "right");
                    applySidebarPosition();
                }
                else if (key == ConfigKeys::SidebarPinned)
                {
                    bool pinned = value.toBool();
                    m_sidebarPinned = pinned;
                    applySidebarPinned(pinned);
                }
                else if (key == ConfigKeys::SidebarCustomEnabled ||
                         key == ConfigKeys::SidebarHideSearch ||
                         key == ConfigKeys::SidebarHideHome ||
                         key == ConfigKeys::SidebarHideFavorites)
                {
                    applySidebarCustomVisibility();
                }
            });

    applySidebarMetrics(m_sidebarPinned);
    applySidebarIcons();
    applySidebarCustomVisibility();
}

void HomeView::openCloudSyncDialog()
{
    if (!m_webdavStore)
    {
        m_webdavStore = new WebdavProfileStore(this);
        m_webdavStore->load();
        qInfo() << "[HomeView] WebdavProfileStore loaded | hasProfile:"
                << m_webdavStore->hasProfile();
    }

    ServerManager *serverManager = m_core ? m_core->serverManager() : nullptr;
    WebdavSyncDialog dialog(m_webdavStore, serverManager, this);
    dialog.exec();
}


void HomeView::triggerSearch(const QString &query)
{
    const QString trimmedQuery = query.trimmed();
    if (trimmedQuery.isEmpty())
        return;

    // 单服搜索历史：所有服共享一份记录（"__global__" 桶，空 serverId）。
    // 聚合搜索历史用 SearchHistoryManager::aggregatedBucket()（"__aggregated__"）。
    SearchHistoryManager::instance()->recordSearch(
        QString(), trimmedQuery);

    
    m_libraryList->clearSelection();
    pushView(createSearchView(trimmedQuery));
}

void HomeView::setupSearchHistory()
{
    if (!m_searchBox) {
        return;
    }

    m_searchHistoryModel = new QStringListModel(this);
    m_searchCompleter = new QCompleter(m_searchHistoryModel, this);
    m_searchCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    m_searchCompleter->setFilterMode(Qt::MatchContains);
    m_searchCompleter->setCompletionMode(QCompleter::PopupCompletion);
    m_searchCompleter->setMaxVisibleItems(8);
    m_searchCompleter->setPopup(new SearchCompleterPopup());
    if (auto *popup =
            qobject_cast<SearchCompleterPopup *>(m_searchCompleter->popup())) {
        popup->setMaxVisibleRows(m_searchCompleter->maxVisibleItems());
    }
    m_searchBox->setCompleter(m_searchCompleter);

    connect(m_searchBox, &QLineEdit::textEdited, this,
            [this](const QString &text) { updateSearchCompleter(text); });

    connect(SearchHistoryManager::instance(), &SearchHistoryManager::historyChanged,
            this, [this](const QString &serverId) {
                // 单服历史：所有服共享 __global__ 桶。
                if (serverId == QStringLiteral("__global__")) {
                    updateSearchCompleter(m_searchBox ? m_searchBox->text()
                                                      : QString());
                }
            });
    connect(SearchHistoryManager::instance(), &SearchHistoryManager::enabledChanged,
            this, [this](bool ) {
                updateSearchCompleter(m_searchBox ? m_searchBox->text()
                                                  : QString());
            });
    connect(SearchHistoryManager::instance(),
            &SearchHistoryManager::autocompleteEnabledChanged, this,
            [this](bool ) {
                updateSearchCompleter(m_searchBox ? m_searchBox->text()
                                                  : QString());
            });

    if (m_core && m_core->serverManager()) {
        connect(m_core->serverManager(), &ServerManager::activeServerChanged, this,
                [this](const ServerProfile &profile) {
                    // 「当前服」分组标题跟随 active server 更新。
                    if (m_currentServerLabel) {
                        const QString name = profile.name.isEmpty()
                                                 ? profile.url
                                                 : profile.name;
                        m_currentServerLabel->setText(
                            tr("当前服 · %1").arg(name));
                    }
                    updateSearchCompleter(m_searchBox ? m_searchBox->text()
                                                      : QString());
                    // 切服刷新 sidebar 三部分：服务器卡片（icon/name/url）、
                    // 媒体库列表（getUserViews）、用户名/头像、按钮可见性。
                    // 之前只刷新了「当前服」标题，导致切服后 sidebar 仍是旧服。
                    scheduleProfileRefresh();
                });

        // Forward the unreachable signal to whichever view is currently shown.
        // DashboardView owns a clearable empty state; other views ignore it.
        connect(this, &HomeView::serverUnreachable, this,
                [this](const QString &, const QString &) {
                    if (m_dashboardView && m_contentSwitcher
                        && m_contentSwitcher->currentWidget() == m_dashboardView) {
                        m_dashboardView->showServerUnreachableState();
                    }
                });
    }

    // 初始化「当前服」分组标题（首次进入可能未触发 activeServerChanged）。
    if (m_currentServerLabel && m_core && m_core->serverManager()) {
        const ServerProfile profile = m_core->serverManager()->activeProfile();
        const QString name = profile.name.isEmpty() ? profile.url : profile.name;
        m_currentServerLabel->setText(tr("当前服 · %1").arg(name));
    }

    updateSearchCompleter();
}

void HomeView::updateSearchCompleter(const QString &text)
{
    if (!m_searchHistoryModel || !m_searchCompleter || !m_searchBox) {
        return;
    }

    const QStringList suggestions =
        SearchHistoryManager::instance()->completionSuggestions(
            QString(), text, 8);
    m_searchHistoryModel->setStringList(suggestions);

    if (!SearchHistoryManager::instance()->isAutocompleteEnabled() ||
        text.trimmed().isEmpty() ||
        suggestions.isEmpty() || !m_searchBox->hasFocus()) {
        if (m_searchCompleter->popup()) {
            m_searchCompleter->popup()->hide();
        }
        return;
    }

    if (auto *popup =
            qobject_cast<SearchCompleterPopup *>(m_searchCompleter->popup())) {
        popup->setHighlightText(text);
        popup->syncWidthToAnchor(m_searchBox);
    }

    m_searchCompleter->setCompletionPrefix(text);
    if (auto *popup =
            qobject_cast<SearchCompleterPopup *>(m_searchCompleter->popup())) {
        m_searchCompleter->complete(popup->popupRectForAnchor(m_searchBox));
        return;
    }

    m_searchCompleter->complete();
}

void HomeView::setupSearchHistoryPopups()
{
    if (!m_searchBox && !m_aggregatedSearchBox) {
        return;
    }

    // —— 当前服搜索框历史（按 active server 分桶）——
    m_searchHistoryPopup = new SearchHistoryPopup(this);
    connect(m_searchHistoryPopup, &SearchHistoryPopup::termActivated, this,
            [this](const QString &term) {
                if (!m_searchBox) return;
                // Qt::Popup 窗口不会随视图切换自动关闭，先显式收起。
                dismissHistoryPopups();
                m_searchBox->setText(term);
                // 同聚合 popup：记录穿透保护时间戳；直接触发搜索（与回车
                // 路径一致，不绕 singleShot）。
                m_historyTermActivatedMs = QDateTime::currentMSecsSinceEpoch();
                triggerSearch(term);
            });
    connect(m_searchHistoryPopup, &SearchHistoryPopup::clearHistoryRequested, this,
            [this]() {
                // 单服历史：所有服共享一份（__global__ 桶）
                SearchHistoryManager::instance()->clearHistory(QString());
            });
    connect(m_searchHistoryPopup, &SearchHistoryPopup::removeHistoryTermRequested, this,
            [this](const QString &term) {
                SearchHistoryManager::instance()->removeHistoryTerm(
                    QString(), term);
            });

    // —— 聚合搜索框历史（__aggregated__ 桶，所有服务器共享）——
    m_aggregatedSearchHistoryPopup = new SearchHistoryPopup(this);
    connect(m_aggregatedSearchHistoryPopup, &SearchHistoryPopup::termActivated, this,
            [this](const QString &term) {
                if (!m_aggregatedSearchBox) return;
                // Qt::Popup 窗口不会随视图切换自动关闭，先显式收起。
                dismissHistoryPopups();
                m_aggregatedSearchBox->setText(term);
                // 记录时间戳：popup 收起动画期间 release 可能穿透到侧栏
                // 按钮（聚合历史/收藏），300ms 内的按钮点击忽略。
                m_historyTermActivatedMs = QDateTime::currentMSecsSinceEpoch();
                // 直接 emit（与回车路径完全一致），不再绕 singleShot——
                // 间接一拍曾被观察到搜索请求丢失（点了历史无搜索效果）。
                Q_EMIT aggregatedSearchRequested(term);
            });
    connect(m_aggregatedSearchHistoryPopup,
            &SearchHistoryPopup::clearHistoryRequested, this,
            [this]() {
                SearchHistoryManager::instance()->clearHistory(
                    SearchHistoryManager::aggregatedBucket());
            });
    connect(m_aggregatedSearchHistoryPopup,
            &SearchHistoryPopup::removeHistoryTermRequested, this,
            [this](const QString &term) {
                SearchHistoryManager::instance()->removeHistoryTerm(
                    SearchHistoryManager::aggregatedBucket(), term);
            });

    // 监听两个搜索框：聚焦/点击（空文本）显示历史，打字隐藏（让补全接管）。
    if (m_searchBox) {
        m_searchBox->installEventFilter(this);
        connect(m_searchBox, &QLineEdit::textEdited, this,
                [this]() { dismissHistoryPopups(); });
    }
    if (m_aggregatedSearchBox) {
        m_aggregatedSearchBox->installEventFilter(this);
        connect(m_aggregatedSearchBox, &QLineEdit::textEdited, this,
                [this]() { dismissHistoryPopups(); });
    }

    // 历史变更时刷新正在显示的下拉。
    connect(SearchHistoryManager::instance(), &SearchHistoryManager::historyChanged,
            this, [this](const QString &serverId) {
                // 单服历史：所有服共享 __global__ 桶（recordSearch emit 的
                // bucket 名即 effectiveServerBucket 的结果 "__global__"）。
                const bool isGlobalBucket =
                    serverId == QStringLiteral("__global__");
                if (m_searchHistoryPopup && m_searchHistoryPopup->isVisible()
                    && m_searchBox && m_searchBox->hasFocus()
                    && isGlobalBucket) {
                    showHistoryPopupFor(m_searchBox, m_searchHistoryPopup,
                                        QString());
                } else if (m_aggregatedSearchHistoryPopup
                           && m_aggregatedSearchHistoryPopup->isVisible()
                           && m_aggregatedSearchBox
                           && m_aggregatedSearchBox->hasFocus()
                           && serverId
                                  == SearchHistoryManager::aggregatedBucket()) {
                    showHistoryPopupFor(m_aggregatedSearchBox,
                                        m_aggregatedSearchHistoryPopup,
                                        SearchHistoryManager::aggregatedBucket());
                }
            });
}

void HomeView::showHistoryPopupFor(QLineEdit *box, SearchHistoryPopup *popup,
                                   const QString &bucket)
{
    if (!box || !popup || !SearchHistoryManager::instance()->isEnabled()) {
        dismissHistoryPopups();
        return;
    }
    if (!box->isVisible()) {
        dismissHistoryPopups();
        return;
    }
    // 不再检查 box->hasFocus()：popup 打开期间点搜索框的按压会被 Qt::Popup
    // 鼠标抓取吞掉（焦点由重弹路径手动补设），hasFocus 门卫会把正常的
    // 「点击必弹」误拦成「不弹出」。触发方（点击 Release/↓ 键）本身就是
    // 用户意图，无需焦点佐证。

    const auto entries = SearchHistoryManager::instance()->historyEntries(bucket);
    popup->setEntries(entries);
    if (!popup->hasContent()) {
        dismissHistoryPopups();
        return;
    }

    // 避免与另一个 popup / 补全下拉叠层。
    if (m_searchHistoryPopup && popup != m_searchHistoryPopup) {
        m_searchHistoryPopup->dismiss(true);
    }
    if (m_aggregatedSearchHistoryPopup && popup != m_aggregatedSearchHistoryPopup) {
        m_aggregatedSearchHistoryPopup->dismiss(true);
    }
    if (m_searchCompleter && m_searchCompleter->popup()) {
        m_searchCompleter->popup()->hide();
    }

    popup->showBelow(box);
}

void HomeView::dismissHistoryPopups()
{
    if (m_searchHistoryPopup) {
        m_searchHistoryPopup->dismiss(true);
    }
    if (m_aggregatedSearchHistoryPopup) {
        m_aggregatedSearchHistoryPopup->dismiss(true);
    }
}





void HomeView::pushView(QWidget *view)
{
    QWidget *current = m_contentSwitcher->currentWidget();
    if (current == view)
        return;

    if (m_contentSwitcher->indexOf(view) == -1)
    {
        m_contentSwitcher->addWidget(view);
    }

    
    if (current)
    {
        RouteInfo info;
        info.widget = current;
        info.isDynamic = current->property("isDynamic").toBool();
        info.routeType = current->property("routeType").toString();
        info.routeId = current->property("routeId").toString();
        info.routeTitle = current->property("routeTitle").toString();
        
        info.routeExtraId = current->property("routeExtraId").toString();
        m_navStack.push(info);
    }

    
    const int MAX_ACTIVE_VIEWS = 12;
    int activeDynamicCount = 0;

    
    for (int i = m_navStack.size() - 1; i >= 0; --i)
    {
        if (m_navStack[i].isDynamic && m_navStack[i].widget)
        {
            activeDynamicCount++;
            if (activeDynamicCount > MAX_ACTIVE_VIEWS)
            {
                
                m_contentSwitcher->removeWidget(m_navStack[i].widget);
                m_navStack[i].widget->deleteLater();
                
                
            }
        }
    }

    
    QMetaObject::invokeMethod(view, "scrollToTop", Qt::QueuedConnection);

    m_contentSwitcher->slideInWgt(view, SlidingStackedWidget::RightToLeft);
    Q_EMIT canNavigateBackChanged(!m_navStack.isEmpty());
}

void HomeView::navigateBack()
{
    QWidget *current = m_contentSwitcher->currentWidget();

    
    
    
    if (auto *baseView = qobject_cast<BaseView *>(current))
    {
        if (baseView->handleBackNavigation())
        {
            qDebug() << "[HomeView] Back navigation consumed by current view"
                     << "| routeType="
                     << current->property("routeType").toString();
            return;
        }

        qDebug() << "[HomeView] Preparing current view for back navigation"
                 << "| routeType="
                 << current->property("routeType").toString();
        baseView->prepareForStackLeave();
    }

    if (!m_navStack.isEmpty())
    {
        RouteInfo prevInfo = m_navStack.pop();
        QWidget *targetView = prevInfo.widget;

        
        if (!targetView && prevInfo.isDynamic)
        {
            if (prevInfo.routeType == "DetailView")
            {
                targetView = createDetailView(prevInfo.routeId, prevInfo.routeTitle);
            }
            else if (prevInfo.routeType == "LibraryView")
            {
                targetView = createLibraryView(prevInfo.routeId, prevInfo.routeTitle);
            }
            else if (prevInfo.routeType == "CategoryView")
            {
                targetView = createCategoryView(prevInfo.routeId, prevInfo.routeTitle);
            }
            else if (prevInfo.routeType == "SearchView")
            {
                targetView = createSearchView(prevInfo.routeId);
            }
            else if (prevInfo.routeType == "SettingsView")
            {
                targetView = createSettingsView();
            }
            else if (prevInfo.routeType == "PersonView")
            {
                targetView = createPersonView(prevInfo.routeId, prevInfo.routeTitle);
            }
            else if (prevInfo.routeType == "SeasonView")
            {
                
                targetView = createSeasonView(prevInfo.routeExtraId, prevInfo.routeId, prevInfo.routeTitle);
            }
            else if (prevInfo.routeType == "ManageView")
            {
                targetView = createManageView();
            }
            else if (prevInfo.routeType == "FilteredView")
            {
                targetView = createFilteredView(prevInfo.routeExtraId, prevInfo.routeTitle);
            }

            if (targetView && m_contentSwitcher->indexOf(targetView) == -1)
            {
                m_contentSwitcher->addWidget(targetView);
            }
        }

        
        if (!targetView)
            targetView = m_dashboardView;

        
        
        
        

        m_contentSwitcher->slideInWgt(targetView, SlidingStackedWidget::LeftToRight);
        Q_EMIT canNavigateBackChanged(!m_navStack.isEmpty());

        
        
        
        if (current && current->property("isDynamic").toBool())
        {
            m_contentSwitcher->disposeWidgetWhenSafe(current);
        }
    }
}

void HomeView::goHome()
{
    if (m_contentSwitcher->currentWidget() == m_dashboardView)
    {
        launchTask(m_dashboardView->loadDashboardData(), m_dashboardView);
        return;
    }
    m_libraryList->clearSelection();
    resetToView(m_dashboardView);
}

void HomeView::goFav()
{
    if (m_contentSwitcher->currentWidget() == m_favoritesView)
    {
        
        m_favoritesView->loadFavoritesData();
        return;
    }
    m_libraryList->clearSelection();
    resetToView(m_favoritesView);
}

void HomeView::resetToView(QWidget *view)
{
    if (m_contentSwitcher->currentWidget() == view)
        return;

    
    QList<QPointer<QWidget>> widgetsToDelete;
    while (!m_navStack.isEmpty())
    {
        RouteInfo info = m_navStack.pop();
        if (info.isDynamic && info.widget && info.widget != view)
        {
            m_contentSwitcher->removeWidget(info.widget);
            widgetsToDelete.append(info.widget);
        }
    }

    QWidget *current = m_contentSwitcher->currentWidget();
    if (auto *baseView = qobject_cast<BaseView *>(current))
    {
        qDebug() << "[HomeView] Preparing current view for reset navigation"
                 << "| routeType="
                 << current->property("routeType").toString();
        baseView->prepareForStackLeave();
    }

    if (m_contentSwitcher->indexOf(view) == -1)
    {
        m_contentSwitcher->addWidget(view);
    }

    
    QMetaObject::invokeMethod(view, "scrollToTop", Qt::QueuedConnection);

    m_contentSwitcher->slideInWgt(view, SlidingStackedWidget::Automatic);
    Q_EMIT canNavigateBackChanged(false);

    
    if (current && current->property("isDynamic").toBool() && current != view)
    {
        widgetsToDelete.append(QPointer<QWidget>(current));
    }

    if (!widgetsToDelete.isEmpty())
    {
        for (const QPointer<QWidget> &widget : widgetsToDelete)
        {
            if (widget)
            {
                m_contentSwitcher->disposeWidgetWhenSafe(widget);
            }
        }
    }
}

bool HomeView::canNavigateBack() const
{
    return !m_navStack.isEmpty();
}

bool HomeView::canGoHome() const
{
    if (!m_contentSwitcher || !m_dashboardView)
        return false;
    return m_contentSwitcher->currentWidget() != m_dashboardView;
}

bool HomeView::canGoFav() const
{
    if (!m_contentSwitcher || !m_favoritesView)
        return false;
    return m_contentSwitcher->currentWidget() != m_favoritesView;
}

void HomeView::scheduleProfileRefresh()
{
    m_pendingProfileRefreshTask = refreshProfile();
}


QCoro::Task<void> HomeView::refreshProfile()
{
    
    QPointer<HomeView> guard(this);
    const int refreshGeneration = ++m_profileRefreshGeneration;

    ServerProfile activeProfile = m_core->serverManager()->activeProfile();
    const bool canReuseExistingLibraries =
        !m_sidebarLibraryServerId.isEmpty() &&
        m_sidebarLibraryServerId == activeProfile.id &&
        m_sidebarLibraryUserId == activeProfile.userId;
    const int previousLibraryCount = m_libraryList ? m_libraryList->count() : 0;
    auto isRefreshStillCurrent = [this, guard, refreshGeneration, activeProfile]()
    {
        if (!guard)
            return false;

        const ServerProfile currentProfile = m_core->serverManager()->activeProfile();
        return refreshGeneration == m_profileRefreshGeneration &&
               currentProfile.id == activeProfile.id &&
               currentProfile.userId == activeProfile.userId;
    };

    // Read-only server info card at the sidebar top mirrors the active
    // profile (icon / name / address).
    if (m_serverIconLabel)
    {
        if (activeProfile.type == ServerProfile::Jellyfin)
        {
            m_serverIconLabel->setPixmap(QPixmap(":/svg/jellyfin.svg"));
        }
        else if (!activeProfile.iconBase64.isEmpty())
        {
            QPixmap pix;
            pix.loadFromData(QByteArray::fromBase64(activeProfile.iconBase64.toUtf8()));
            m_serverIconLabel->setPixmap(pix);
        }
        else
        {
            m_serverIconLabel->setPixmap(QPixmap(":/svg/emby.svg"));
        }
    }
    if (m_serverNameLabel)
    {
        const QString displayName = activeProfile.name.isEmpty()
                                        ? tr("My Server")
                                        : activeProfile.name;
        m_serverNameLabel->setFullText(displayName);
    }
    if (m_serverAddressLabel)
    {
        m_serverAddressLabel->setFullText(activeProfile.url);
    }

    applySidebarIcons();

    
    m_btnManage->setVisible(activeProfile.isAdmin);
    m_btnDownloads->setVisible(activeProfile.canDownloadMedia);

    const QString userRoleText = activeProfile.isAdmin ? tr("Administrator") : tr("User");
    QString displayUserName = activeProfile.userName.isEmpty() ? userRoleText : activeProfile.userName;
    m_userNameLabel->setFullText(displayUserName);
    m_userNameLabel->setProperty("userRoleText", userRoleText);
    applySidebarMetrics(m_sidebarPinned);

    try
    {
        QList<MediaItem> views = co_await m_core->mediaService()->getUserViews();
        if (!isRefreshStillCurrent())
        {
            qDebug() << "[HomeView] Ignoring stale sidebar library refresh after initial fetch"
                     << "| generation=" << refreshGeneration
                     << "| previousLibraryCount=" << previousLibraryCount;
            co_return;
        }

        if (views.isEmpty())
        {
            m_core->mediaService()->clearUserViewsCache();
            views = co_await m_core->mediaService()->getUserViews();

            if (!isRefreshStillCurrent())
            {
                qDebug() << "[HomeView] Ignoring stale sidebar library refresh after retry"
                         << "| generation=" << refreshGeneration
                         << "| previousLibraryCount=" << previousLibraryCount;
                co_return;
            }
        }

        QWidget *currentView = m_contentSwitcher ? m_contentSwitcher->currentWidget() : nullptr;
        const QString currentRouteType = currentView ? currentView->property("routeType").toString() : QString();
        const QString currentRouteId = currentView ? currentView->property("routeId").toString() : QString();
        QListWidgetItem *selectedLibraryItem = nullptr;
        const bool shouldKeepExistingLibraries =
            views.isEmpty() && previousLibraryCount > 0 && canReuseExistingLibraries;

        if (!shouldKeepExistingLibraries)
        {
            m_libraryList->clear();
            for (const auto &view : views)
            {
                QString iconStr = "📁 ";
                if (view.collectionType == "movies")
                    iconStr = "🎬 ";
                else if (view.collectionType == "tvshows")
                    iconStr = "📺 ";
                else if (view.collectionType == "music")
                    iconStr = "🎵 ";
                else if (view.collectionType == "homevideos" || view.collectionType == "photos")
                    iconStr = "🎞️ ";

                auto *item = new QListWidgetItem(iconStr + view.name);
                item->setData(Qt::UserRole, view.id);
                item->setData(kSidebarLibraryNameRole, view.name);
                item->setData(Qt::ToolTipRole, view.name);
                m_libraryList->addItem(item);

                if (currentRouteType == "LibraryView" && view.id == currentRouteId)
                {
                    selectedLibraryItem = item;
                }
            }

            m_sidebarLibraryServerId = activeProfile.id;
            m_sidebarLibraryUserId = activeProfile.userId;
        }
        else
        {
            qWarning().noquote() << "[HomeView] Keeping existing sidebar library list because refreshed views are unexpectedly empty"
                                 << "| previousLibraryCount=" << previousLibraryCount;
        }

        if (!selectedLibraryItem && currentRouteType == "LibraryView")
        {
            for (int i = 0; i < m_libraryList->count(); ++i)
            {
                QListWidgetItem *item = m_libraryList->item(i);
                if (item && item->data(Qt::UserRole).toString() == currentRouteId)
                {
                    selectedLibraryItem = item;
                    break;
                }
            }
        }

        if (selectedLibraryItem)
        {
            m_libraryList->setCurrentItem(selectedLibraryItem);
            selectedLibraryItem->setSelected(true);
        }
        else
        {
            m_libraryList->clearSelection();
        }
    }
    catch (const std::exception &e)
    {
        if (!isRefreshStillCurrent())
        {
            qDebug() << "[HomeView] Ignoring stale sidebar library refresh failure"
                     << "| generation=" << refreshGeneration
                     << "| previousLibraryCount=" << previousLibraryCount
                     << "| error:" << e.what();
            co_return;
        }
        qWarning().noquote() << "[HomeView] Failed to load library views for sidebar"
                             << "| generation=" << refreshGeneration
                             << "| previousLibraryCount=" << previousLibraryCount
                             << "| activeServerId:" << activeProfile.id
                             << "| error:" << e.what();
        // Clear stale entries so the user doesn't see a misleading list
        // (e.g. the previous server's libraries) after a server switch
        // when the new server's getUserViews() throws.
        if (m_libraryList) {
            m_libraryList->clear();
        }
        m_sidebarLibraryServerId = activeProfile.id;
        m_sidebarLibraryUserId = activeProfile.userId;
    }
}

void HomeView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);

    
    
    if (m_sidebarPinned && !m_sidebarPinnedApplied)
    {
        m_sidebarPinnedApplied = true;
        applySidebarPinned(true);
    }
    else if (m_sidebarPinned && m_sidebarPinnedApplied)
    {
        
        if (m_contentLayout->indexOf(m_sidebar) == -1)
        {
            if (m_sidebarOnRight)
            {
                m_contentLayout->addWidget(m_sidebar);
            }
            else
            {
                m_contentLayout->insertWidget(0, m_sidebar);
            }
            m_sidebar->show();
        }
    }

    syncSidebarVisibility();
    scheduleProfileRefresh();
}

void HomeView::hideEvent(QHideEvent *event)
{
    
    
    if (m_sidebarPinned && m_sidebarPinnedApplied)
    {
        m_contentLayout->removeWidget(m_sidebar);
        m_sidebar->hide();
    }

    dismissHistoryPopups();
    QWidget::hideEvent(event);
}

void HomeView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);

    
    if (m_sidebarPinned)
        return;

    const int sidebarW = m_sidebar->width();

    if (m_sidebarOnRight)
    {
        
        m_edgeTrigger->setGeometry(width() - kRightEdgeTriggerWidth, 0, kRightEdgeTriggerWidth, height());

        
        bool isShown = (m_sidebar->x() < width() && m_sidebar->x() >= width() - sidebarW);
        if (isShown)
        {
            m_sidebar->setGeometry(width() - sidebarW, 0, sidebarW, height());
        }
        else
        {
            m_sidebar->setGeometry(width() + kSidebarHiddenOffset, 0, sidebarW, height());
        }
    }
    else
    {
        
        m_edgeTrigger->setGeometry(0, 0, kLeftEdgeTriggerWidth, height());

        if (m_sidebar->x() < 0)
        {
            m_sidebar->setGeometry(-sidebarW - kSidebarHiddenOffset, 0, sidebarW, height());
        }
        else
        {
            m_sidebar->setGeometry(0, 0, sidebarW, height());
        }
    }
}

bool HomeView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_serverSwitcherViewport
        && (event->type() == QEvent::MouseMove
            || event->type() == QEvent::HoverMove))
    {
        // Drive hover feedback from HoverMove/MouseMove + itemAt() instead of
        // QListWidget::itemEntered, which is silently suppressed when
        // a sub-widget inside the setItemWidget() row intercepts the
        // enter event.
        // HoverMove 优先：viewport 已设 WA_Hover，鼠标在行内子 widget 上
        // 也持续触发（不需要 mouseTracking），响应最灵敏。
        auto *list = qobject_cast<QListWidget *>(m_serverSwitcherViewport->parent());
        if (!list) return false;
        QPoint viewportPos;
        if (event->type() == QEvent::HoverMove) {
            viewportPos = static_cast<QHoverEvent *>(event)->position().toPoint();
        } else {
            viewportPos = static_cast<QMouseEvent *>(event)->pos();
        }
        // QListWidget::itemAt expects coordinates RELATIVE TO THE VIEWPORT,
        // not the list widget — see Qt docs:
        //   "Returns a pointer to the item at the coordinates p. These
        //    coordinates are relative to the list widget's viewport."
        QListWidgetItem *item = list->itemAt(viewportPos);
        if (item == m_serverSwitcherHoverItem) return false;
        if (m_serverSwitcherHoverItem) {
            if (auto *prevRow = qvariant_cast<QWidget *>(
                    m_serverSwitcherHoverItem->data(Qt::UserRole + 2))) {
                prevRow->setProperty("switcherHover", false);
                prevRow->style()->unpolish(prevRow);
                prevRow->style()->polish(prevRow);
            }
        }
        m_serverSwitcherHoverItem = item;
        if (item) {
            if (auto *row = qvariant_cast<QWidget *>(
                    item->data(Qt::UserRole + 2))) {
                row->setProperty("switcherHover", true);
                row->style()->unpolish(row);
                row->style()->polish(row);
            }
        }
        return false;
    }


    if (watched == m_serverSwitcherViewport && event->type() == QEvent::Leave)
    {
        // Fallback for the rare case where the cursor leaves the viewport
        // without triggering a final MouseMove (e.g. into the popup margin).
        if (m_serverSwitcherHoverItem)
        {
            if (auto *row = qvariant_cast<QWidget *>(
                    m_serverSwitcherHoverItem->data(Qt::UserRole + 2)))
            {
                row->setProperty("switcherHover", false);
                row->style()->unpolish(row);
                row->style()->polish(row);
            }
            m_serverSwitcherHoverItem = nullptr;
        }
    }

    // —— 全局 mouse press：搜索历史 popup 打开时点 popup 外部 → dismiss ——
    // 注意：必须用「全局坐标是否落在 popup 矩形内」判断，不能用 watched
    // 父链判断——Qt::Popup 开启期间 Qt 会抓取鼠标，点在 popup 外部（包括
    // 搜索框自己）的 press 会被重定向给 popup 本身（watched == popup），
    // 父链判断永远得到「在 popup 内」→ 下拉永远收不起来（顶栏用的是
    // 坐标判断所以正常，侧栏因此一直点不掉）。
    if (event->type() == QEvent::MouseButtonPress) {
        const QPoint gp = QCursor::pos();
        auto handleOutsidePress = [&](SearchHistoryPopup *popup, QLineEdit *box,
                                      const QString &bucket) {
            if (!popup || !popup->isVisible()) {
                return;
            }
            const QRect popupRect(popup->mapToGlobal(QPoint(0, 0)), popup->size());
            if (popupRect.contains(gp)) {
                return;  // 点击在 popup 内，chip 自己处理
            }
            popup->dismiss(true);
            // 点的是锚点搜索框本身：按压已被 popup 吞掉（不会送达输入框、
            // 也不会设焦点），这里手动补焦点并重弹下拉（「点击必弹」语义）。
            if (box && box->isVisible()) {
                const QRect boxRect(box->mapToGlobal(QPoint(0, 0)), box->size());
                if (boxRect.contains(gp)) {
                    box->setFocus(Qt::MouseFocusReason);
                    QTimer::singleShot(0, this, [this, box, popup, bucket]() {
                        showHistoryPopupFor(box, popup, bucket);
                    });
                }
            }
        };
        handleOutsidePress(m_searchHistoryPopup, m_searchBox, QString());
        handleOutsidePress(m_aggregatedSearchHistoryPopup, m_aggregatedSearchBox,
                           SearchHistoryManager::aggregatedBucket());
    }

    // —— 搜索历史下拉：聚焦/点击（空文本）显示，↓ 键显示，Esc 隐藏 ——
    if (watched == m_searchBox || watched == m_aggregatedSearchBox) {
        const bool isAggregated = (watched == m_aggregatedSearchBox);
        auto *box = isAggregated ? m_aggregatedSearchBox : m_searchBox;
        SearchHistoryPopup *popup = isAggregated ? m_aggregatedSearchHistoryPopup
                                                 : m_searchHistoryPopup;
        // 单服历史：所有服共享一份（__global__ 桶，空 serverId）
        // 聚合历史：__aggregated__ 桶
        const QString bucket = isAggregated
                                   ? SearchHistoryManager::aggregatedBucket()
                                   : QString();

        if (event->type() == QEvent::MouseButtonRelease) {
            // 用 Release 而非 Press 触发：popup 在松开后才弹出，本轮点击的
            // press/release 都已正常落到输入框（不会被 Qt::Popup 的鼠标抓取
            // 重定向、也不会吞掉输入框焦点），点击必弹且无闪烁。
            // 点击必弹（不要求空文本）：选过历史后框内残留文字也应能
            // 再次打开下拉（否则点过一次历史记录后就「打不开」了）。
            QTimer::singleShot(0, this, [this, box, popup, bucket]() {
                showHistoryPopupFor(box, popup, bucket);
            });
            return false;
        }
        // 注意：不处理 FocusIn——Qt::Popup 关闭时会把焦点还给输入框并触发
        // FocusIn，若在此重新弹出会形成 show/close 振荡（下拉一直闪）。
        // 打开途径：点击搜索框、↓ 键。
        if (event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_Down) {
                showHistoryPopupFor(box, popup, bucket);
                return false;
            }
            if (keyEvent->key() == Qt::Key_Escape) {
                dismissHistoryPopups();
                return false;
            }
        }
    }

    if (m_libraryList && watched == m_libraryList->viewport() &&
        event->type() == QEvent::Wheel)
    {
        auto *wheelEvent = static_cast<QWheelEvent *>(event);
        if (m_sidebarLibraryScrollController &&
            m_sidebarLibraryScrollController->scrollByWheelEvent(wheelEvent, Qt::Vertical))
        {
            return true;
        }
    }

    
    if (m_sidebarPinned)
    {
        return QWidget::eventFilter(watched, event);
    }

    if (watched == m_edgeTrigger && event->type() == QEvent::Enter)
    {
        showSidebar();
        return true;
    }

    if (watched == m_sidebar)
    {
        if (event->type() == QEvent::Leave)
        {
            QPoint globalPos = QCursor::pos();
            QPoint localPos = m_sidebar->mapFromGlobal(globalPos);
            if (!m_sidebar->rect().contains(localPos))
            {
                hideSidebar();
            }
            return false;
        }
        
        if (event->type() == QEvent::MouseMove)
        {
            if (m_sidebarAutoHideTimer->isActive())
            {
                m_sidebarAutoHideTimer->start(); 
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void HomeView::showSidebar()
{
    
    if (m_sidebarPinned || isCurrentViewImmersive())
        return;

    
    bool reduceAnimations = ConfigStore::instance()->get<bool>(ConfigKeys::UiAnimations, false);
    if (reduceAnimations)
    {
        if (m_sidebarOnRight)
        {
            m_sidebar->move(width() - m_sidebar->width(), 0);
        }
        else
        {
            m_sidebar->move(0, 0);
        }
        m_sidebarAutoHideTimer->start(); 
        return;
    }

    if (m_sidebarAnim->state() == QAbstractAnimation::Running)
        m_sidebarAnim->stop();
    m_sidebarAnim->setStartValue(m_sidebar->pos());

    if (m_sidebarOnRight)
    {
        m_sidebarAnim->setEndValue(QPoint(width() - m_sidebar->width(), 0));
    }
    else
    {
        m_sidebarAnim->setEndValue(QPoint(0, 0));
    }
    m_sidebarAnim->start();
    m_sidebarAutoHideTimer->start(); 
}

void HomeView::hideSidebar()
{
    
    if (m_sidebarPinned)
        return;

    
    bool reduceAnimations = ConfigStore::instance()->get<bool>(ConfigKeys::UiAnimations, false);
    if (reduceAnimations)
    {
        if (m_sidebarOnRight)
        {
            m_sidebar->move(width() + kSidebarHiddenOffset, 0);
        }
        else
        {
            m_sidebar->move(-m_sidebar->width() - kSidebarHiddenOffset, 0);
        }
        return;
    }

    m_sidebarAutoHideTimer->stop(); 
    if (m_sidebarAnim->state() == QAbstractAnimation::Running)
        m_sidebarAnim->stop();
    m_sidebarAnim->setStartValue(m_sidebar->pos());

    if (m_sidebarOnRight)
    {
        m_sidebarAnim->setEndValue(QPoint(width() + kSidebarHiddenOffset, 0));
    }
    else
    {
        m_sidebarAnim->setEndValue(QPoint(-m_sidebar->width() - kSidebarHiddenOffset, 0));
    }
    m_sidebarAnim->start();
}

bool HomeView::isCurrentViewImmersive() const
{
    QWidget *current = m_contentSwitcher ? m_contentSwitcher->currentWidget() : nullptr;
    return current && current->property("isImmersive").toBool();
}

int HomeView::sidebarWidthForMode(bool pinned) const
{
    return pinned ? kPinnedSidebarWidth : kFloatingSidebarWidth;
}

void HomeView::applySidebarMetrics(bool pinned)
{
    if (!m_sidebar)
    {
        return;
    }

    m_sidebar->setFixedWidth(sidebarWidthForMode(pinned));

    auto *layout = qobject_cast<QVBoxLayout *>(m_sidebar->layout());
    const int horizontalInset = pinned ? 12 : 16;
    if (layout)
    {
        if (pinned)
        {
            layout->setContentsMargins(horizontalInset, 18, 0, 18);
            layout->setSpacing(4);
        }
        else
        {
            layout->setContentsMargins(horizontalInset, 20, 0, 20);
            layout->setSpacing(6);
        }
    }

    if (m_navArea && m_navArea->layout())
    {
        m_navArea->layout()->setContentsMargins(0, 0, horizontalInset, 0);
    }

    if (m_sidebarFooterActionsLayout)
    {
        m_sidebarFooterActionsLayout->setContentsMargins(0, 0, horizontalInset, 0);
        m_sidebarFooterActionsLayout->setSpacing(pinned ? 4 : 6);
    }

    if (m_serverInfoLayout)
    {
        m_serverInfoLayout->setDirection(pinned ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
        m_serverInfoLayout->setContentsMargins(pinned ? QMargins(4, 0, horizontalInset + 4, 10)
                                                       : QMargins(8, 0, horizontalInset + 8, 10));
        m_serverInfoLayout->setSpacing(pinned ? 8 : 10);
        m_serverInfoLayout->setAlignment(m_serverIconLabel, pinned ? Qt::AlignHCenter : Qt::AlignVCenter);
        if (m_serverNameLayout)
        {
            m_serverInfoLayout->setAlignment(m_serverNameLayout, pinned ? Qt::AlignHCenter : Qt::AlignVCenter);
        }
    }

    if (m_serverNameLayout)
    {
        m_serverNameLayout->setAlignment(pinned ? Qt::AlignHCenter : Qt::AlignVCenter);
    }

    if (m_serverNameLabel)
    {
        m_serverNameLabel->setAlignment(pinned ? Qt::AlignHCenter : Qt::AlignLeft);
        const QString serverName = m_serverNameLabel->fullText();
        const QString serverAddress = m_serverAddressLabel ? m_serverAddressLabel->fullText() : QString();

        if (pinned && !serverAddress.isEmpty())
        {
            m_serverNameLabel->setToolTip(serverName.isEmpty() ? serverAddress : serverName + "\n" + serverAddress);
        }
        else
        {
            m_serverNameLabel->setToolTip(serverName);
        }
    }

    if (m_serverAddressLabel)
    {
        m_serverAddressLabel->setAlignment(pinned ? Qt::AlignHCenter : Qt::AlignLeft);
        m_serverAddressLabel->setVisible(!pinned);
        if (!pinned)
        {
            m_serverAddressLabel->setToolTip(m_serverAddressLabel->fullText());
        }
    }

    if (m_userInfoLayout)
    {
        m_userInfoLayout->setContentsMargins(pinned ? QMargins(4, 0, horizontalInset + 4, 8)
                                                    : QMargins(0, 0, horizontalInset, 10));
        m_userInfoLayout->setSpacing(pinned ? 4 : 8);
        m_userInfoLayout->setAlignment(Qt::AlignVCenter);
    }

    if (m_userAvatarLabel)
    {
        const int avatarSize = pinned ? 12 : 20;
        m_userAvatarLabel->setFixedSize(avatarSize, avatarSize);
        m_userAvatarLabel->setVisible(true);
    }

    const int sidebarActionIconSize = pinned ? 19 : 22;
    auto applySidebarActionIconSize = [sidebarActionIconSize](QPushButton* button)
    {
        if (button)
        {
            button->setIconSize(QSize(sidebarActionIconSize, sidebarActionIconSize));
        }
    };
    applySidebarActionIconSize(m_btnHome);
    applySidebarActionIconSize(m_btnFavorites);
    applySidebarActionIconSize(m_btnSettings);
    applySidebarActionIconSize(m_btnManage);
    applySidebarActionIconSize(m_btnLogout);

    const int utilityButtonSize = pinned ? 16 : 20;
    const int utilityIconSize = pinned ? 11 : 13;
    auto applySidebarUtilityMetrics = [utilityButtonSize, utilityIconSize](QPushButton* button)
    {
        if (button)
        {
            button->setFixedSize(utilityButtonSize, utilityButtonSize);
            button->setIconSize(QSize(utilityIconSize, utilityIconSize));
        }
    };
    applySidebarUtilityMetrics(m_btnCloudSync);
    applySidebarUtilityMetrics(m_btnDownloads);

    if (m_userNameLabel)
    {
        m_userNameLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        const QString userName = m_userNameLabel->fullText();
        const QString userRole = m_userNameLabel->property("userRoleText").toString();
        if (pinned && !userRole.isEmpty())
        {
            m_userNameLabel->setToolTip(userName.isEmpty() ? userRole : userName + "\n" + userRole);
        }
        else
        {
            m_userNameLabel->setToolTip(userName);
        }
    }
}

void HomeView::applySidebarIcons()
{
    if (m_searchAction)
    {
        m_searchAction->setIcon(
            ThemeManager::getAdaptiveIcon(":/svg/light/search.svg"));
    }
    if (m_btnHome)
    {
        m_btnHome->setIcon(
            ThemeManager::getAdaptiveIcon(":/svg/light/home.svg"));
    }
    if (m_btnFavorites)
    {
        m_btnFavorites->setIcon(
            ThemeManager::getAdaptiveIcon(":/svg/light/heart.svg"));
    }
    if (m_btnSettings)
    {
        m_btnSettings->setIcon(
            ThemeManager::getAdaptiveIcon(":/svg/light/settings.svg"));
    }
    if (m_btnManage)
    {
        m_btnManage->setIcon(
            ThemeManager::getAdaptiveIcon(":/svg/light/server.svg"));
    }
    if (m_btnCloudSync)
    {
        m_btnCloudSync->setIcon(
            ThemeManager::getAdaptiveIcon(":/svg/light/cloud-sync.svg"));
    }
    if (m_btnDownloads)
    {
        m_btnDownloads->setIcon(
            ThemeManager::getAdaptiveIcon(":/svg/light/download-sidebar.svg"));
    }
    if (m_btnLogout)
    {
        m_btnLogout->setIcon(
            ThemeManager::getAdaptiveIcon(":/svg/light/logout.svg"));
    }
    if (m_userAvatarLabel && m_core && m_core->serverManager())
    {
        const ServerProfile activeProfile = m_core->serverManager()->activeProfile();
        const QString avatarIconPath = activeProfile.isAdmin
                                           ? QStringLiteral(":/svg/light/user-admin.svg")
                                           : QStringLiteral(":/svg/light/user.svg");
        m_userAvatarLabel->setPixmap(
            ThemeManager::getAdaptiveIcon(avatarIconPath).pixmap(20, 20));
    }
}

void HomeView::applySidebarCustomVisibility()
{
    const bool customEnabled = ConfigStore::instance()->get<bool>(ConfigKeys::SidebarCustomEnabled, false);

    if (!customEnabled)
    {
        if (m_searchBox) m_searchBox->setVisible(true);
        if (m_searchSpacer) m_searchSpacer->setVisible(true);
        if (m_btnHome) m_btnHome->setVisible(true);
        if (m_btnFavorites) m_btnFavorites->setVisible(true);
        return;
    }

    const bool hideSearch = ConfigStore::instance()->get<bool>(ConfigKeys::SidebarHideSearch, false);
    const bool hideHome = ConfigStore::instance()->get<bool>(ConfigKeys::SidebarHideHome, false);
    const bool hideFav = ConfigStore::instance()->get<bool>(ConfigKeys::SidebarHideFavorites, false);

    if (m_searchBox) m_searchBox->setVisible(!hideSearch);
    if (m_searchSpacer) m_searchSpacer->setVisible(!hideSearch);
    if (m_btnHome) m_btnHome->setVisible(!hideHome);
    if (m_btnFavorites) m_btnFavorites->setVisible(!hideFav);
}

void HomeView::syncSidebarVisibility()
{
    if (!m_sidebar || !m_edgeTrigger)
    {
        return;
    }

    const bool isImmersive = isCurrentViewImmersive();
    qDebug() << "[HomeView] Sync sidebar visibility:"
             << "pinned =" << m_sidebarPinned << "immersive =" << isImmersive;

    if (m_sidebarAnim && m_sidebarAnim->state() == QAbstractAnimation::Running)
    {
        m_sidebarAnim->stop();
    }
    if (m_sidebarAutoHideTimer)
    {
        m_sidebarAutoHideTimer->stop();
    }

    if (isImmersive)
    {
        m_sidebar->hide();
        m_edgeTrigger->hide();
        return;
    }

    if (m_sidebarPinned)
    {
        m_sidebar->show();
        m_edgeTrigger->hide();
        return;
    }

    const int sidebarW = m_sidebar->width();
    if (m_sidebarOnRight)
    {
        m_sidebar->setGeometry(width() + kSidebarHiddenOffset, 0, sidebarW, height());
        m_edgeTrigger->setGeometry(width() - kRightEdgeTriggerWidth, 0, kRightEdgeTriggerWidth, height());
    }
    else
    {
        m_sidebar->setGeometry(-sidebarW - kSidebarHiddenOffset, 0, sidebarW, height());
        m_edgeTrigger->setGeometry(0, 0, kLeftEdgeTriggerWidth, height());
    }

    m_sidebar->show();
    m_edgeTrigger->show();
}

void HomeView::applySidebarPosition()
{
    m_sidebar->setProperty("sidebarSide", m_sidebarOnRight ? "right" : "left");
    m_sidebar->style()->unpolish(m_sidebar);
    m_sidebar->style()->polish(m_sidebar);

    
    if (m_sidebarPinned)
    {
        m_contentLayout->removeWidget(m_sidebar);
        if (m_sidebarOnRight)
        {
            m_contentLayout->addWidget(m_sidebar);
        }
        else
        {
            m_contentLayout->insertWidget(0, m_sidebar);
        }
        syncSidebarVisibility();
        return;
    }

    const int sidebarW = m_sidebar->width();

    
    if (auto *shadow = qobject_cast<QGraphicsDropShadowEffect *>(m_sidebar->graphicsEffect()))
    {
        shadow->setOffset(m_sidebarOnRight ? -4 : 4, 0);
    }

    
    if (m_sidebarOnRight)
    {
        m_edgeTrigger->setGeometry(width() - kRightEdgeTriggerWidth, 0, kRightEdgeTriggerWidth, height());
    }
    else
    {
        m_edgeTrigger->setGeometry(0, 0, kLeftEdgeTriggerWidth, height());
    }

    
    if (m_sidebarAnim->state() == QAbstractAnimation::Running)
        m_sidebarAnim->stop();
    if (m_sidebarOnRight)
    {
        m_sidebar->setGeometry(width() + kSidebarHiddenOffset, 0, sidebarW, height());
    }
    else
    {
        m_sidebar->setGeometry(-sidebarW - kSidebarHiddenOffset, 0, sidebarW, height());
    }
}




void HomeView::applySidebarPinned(bool pinned)
{
    qDebug() << "[HomeView] Sidebar pinned mode changed:" << pinned
             << "position:" << (m_sidebarOnRight ? "right" : "left");

    if (pinned)
    {
        
        if (m_sidebarAnim->state() == QAbstractAnimation::Running)
            m_sidebarAnim->stop();
        m_sidebarAutoHideTimer->stop();

        
        m_sidebar->setGraphicsEffect(nullptr);

        
        m_sidebar->setProperty("pinned", true);
        m_sidebar->style()->unpolish(m_sidebar);
        m_sidebar->style()->polish(m_sidebar);

        
        applySidebarMetrics(true);
        m_sidebar->setMinimumHeight(0);
        m_sidebar->setMaximumHeight(QWIDGETSIZE_MAX);

        
        if (m_sidebarOnRight)
        {
            m_contentLayout->addWidget(m_sidebar);
        }
        else
        {
            m_contentLayout->insertWidget(0, m_sidebar);
        }
        syncSidebarVisibility();
        m_sidebar->raise(); 
    }
    else
    {
        
        m_contentLayout->removeWidget(m_sidebar);
        m_sidebar->setParent(this); 

        
        auto *shadow = new QGraphicsDropShadowEffect(this);
        shadow->setBlurRadius(25);
        shadow->setColor(QColor(0, 0, 0, 30));
        shadow->setOffset(m_sidebarOnRight ? -4 : 4, 0);
        m_sidebar->setGraphicsEffect(shadow);

        
        m_sidebar->setProperty("pinned", false);
        m_sidebar->style()->unpolish(m_sidebar);
        m_sidebar->style()->polish(m_sidebar);

        
        applySidebarMetrics(false);
        syncSidebarVisibility();
    }
}

void HomeView::showServerSwitcher(QWidget *anchorWidget)
{
    if (!m_core || !m_core->serverManager()) {
        return;
    }
    // Anchor the popup to the given trigger widget — normally the titlebar
    // server pill owned by MainWindow. Fall back to the read-only sidebar
    // server-info card when no anchor is supplied so the popup still has a
    // sane on-screen position.
    QWidget *anchor = anchorWidget ? anchorWidget : m_serverInfoWidget;
    if (anchor == nullptr) {
        return;
    }

    // The previous popup may have been closed and destroyed; its hovered
    // item is gone, so reset before building a fresh popup.
    m_serverSwitcherHoverItem = nullptr;

    // Reuse a single popup instance.
    auto *popup = new QWidget(nullptr, Qt::Popup | Qt::FramelessWindowHint);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setObjectName(QStringLiteral("server-switcher-popup"));

    auto *list = new QListWidget(popup);
    list->setObjectName(QStringLiteral("server-switcher-list"));
    list->setFrameShape(QFrame::NoFrame);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setFocusPolicy(Qt::NoFocus);
    list->setUniformItemSizes(true);
    list->setSpacing(0);
    list->setMouseTracking(true);
    // Viewport needs its own mouseTracking — QListWidget's setMouseTracking
    // doesn't always propagate down, and without it MouseMove stops firing
    // once the cursor is stationary inside the row, leaving the previous
    // item stuck in the "hovered" state until the next pixel of movement.
    list->viewport()->setAttribute(Qt::WA_Hover, true);
    list->viewport()->setMouseTracking(true);

    // Avatar palette (matches the mockup); colour is chosen by hashing the
    // stable server id so the same server always shows the same colour.
    static const QList<QColor> kAvatarColors = {
        QColor("#3B82F6"),  // blue
        QColor("#10B981"),  // green
        QColor("#F59E0B"),  // amber
        QColor("#8B5CF6"),  // purple
        QColor("#EC4899"),  // pink
        QColor("#06B6D4"),  // cyan
    };

    auto avatarColor = [](const QString &seed) {
        const uint h = qHash(seed);
        return kAvatarColors.at(static_cast<int>(h % kAvatarColors.size()));
    };
    auto avatarChar = [](const QString &name) {
        const QString trimmed = name.trimmed();
        if (trimmed.isEmpty()) return QStringLiteral("?");
        return QString(trimmed.at(0));
    };

    const QList<ServerProfile> all = m_core->serverManager()->servers();
    const QString activeId = m_core->serverManager()->activeProfile().id;
    for (const ServerProfile &p : all) {
        const QString displayName = p.name.isEmpty() ? p.url : p.name;
        const QString urlText = p.url;
        const bool isActive = (p.id == activeId);
        const QColor color = avatarColor(p.id);

        auto *row = new QWidget(list);
        row->setObjectName(QStringLiteral("server-switcher-row"));
        row->setProperty("switcherActive", isActive);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 0, 10, 0);
        rowLayout->setSpacing(10);

        auto *avatar = new QLabel(avatarChar(displayName), row);
        avatar->setObjectName(QStringLiteral("server-switcher-avatar"));
        avatar->setAlignment(Qt::AlignCenter);
        avatar->setFixedSize(24, 24);
        avatar->setStyleSheet(
            QStringLiteral("background-color: %1; color: #FFFFFF; border-radius: 6px;")
                .arg(color.name()));

        auto *textCol = new QWidget(row);
        textCol->setObjectName(QStringLiteral("server-switcher-text"));
        auto *textLayout = new QVBoxLayout(textCol);
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(0);
        auto *nameLabel = new QLabel(displayName, textCol);
        nameLabel->setObjectName(QStringLiteral("server-switcher-name"));
        if (isActive) {
            QFont f = nameLabel->font();
            f.setBold(true);
            nameLabel->setFont(f);
        }
        auto *urlLabel = new QLabel(urlText, textCol);
        urlLabel->setObjectName(QStringLiteral("server-switcher-url"));
        textLayout->addWidget(nameLabel);
        textLayout->addWidget(urlLabel);

        rowLayout->addWidget(avatar);
        rowLayout->addWidget(textCol, 1);
        if (isActive) {
            auto *check = new QLabel(row);
            check->setObjectName(QStringLiteral("server-switcher-check"));
            check->setText(QStringLiteral("\u2713"));
            check->setAlignment(Qt::AlignCenter);
            check->setFixedSize(20, 20);
            rowLayout->addWidget(check);
        } else {
            rowLayout->addSpacing(20);
        }
        // Hover feedback (blue background + border + 3% scale) is enough to
        // signal "click to switch" — the previous "Switch"/"Current" text
        // badge was redundant and cluttered the row. Keep a fixed-width
        // trailing spacing so the row width doesn't jump between the active
        // (has check mark) and inactive (has spacing) cases.
        rowLayout->addSpacing(64);

        auto *item = new QListWidgetItem(list);
        item->setData(Qt::UserRole, p.id);
        item->setData(Qt::UserRole + 1, displayName);
        item->setData(Qt::UserRole + 2, QVariant::fromValue<QWidget *>(row));
        item->setToolTip(urlText);
        item->setSizeHint(QSize(0, 40));
        list->addItem(item);
        list->setItemWidget(item, row);

        // Capture left-button press anywhere in the row (the row's child
        // QLabels would otherwise swallow MouseButtonPress, leaving the
        // QListWidget::itemClicked signal silent). Parent the filter to
        // the popup so it dies with the dialog.
        const QString switchId = p.id;
        const QString switchName = displayName;
        auto *clickFilter = new RowClickFilter(popup);
        clickFilter->onClick = [this, list, switchId, switchName]() {
            list->window()->close();
            if (switchId.isEmpty()) return;
            // launchTask keeps the coroutine alive (via QCoro::connect)
            // until trySwitchToServer finishes, including across the
            // co_await on the probe network call.
            launchTask(trySwitchToServer(switchId, switchName), this);
        };
        row->installEventFilter(clickFilter);
        for (QWidget *child : row->findChildren<QWidget *>()) {
            child->installEventFilter(clickFilter);
        }
    }
    // Width: comfortable minimum, capped at roughly the sidebar width so the
    // popup never grows absurdly wide on narrow windows.
    list->setMinimumWidth(220);
    list->setMaximumWidth(m_sidebar ? m_sidebar->width() - 32 : 260);
    m_serverSwitcherViewport = list->viewport();
    list->viewport()->installEventFilter(this);

    auto *layout = new QVBoxLayout(popup);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(list);
    popup->setMinimumWidth(220);

    // Anchor popup to the trigger widget, aligned to its left edge.
    const QPoint anchorPos = anchor->mapToGlobal(QPoint(0, anchor->height()));
    popup->move(anchorPos);
    popup->show();
    list->setFocus();
}

QCoro::Task<void> HomeView::trySwitchToServer(const QString &serverId,
                                              const QString &displayName)
{
    if (!m_core || !m_core->serverManager()) co_return;
    if (serverId == m_core->serverManager()->activeProfile().id) co_return;

    ServerManager *sm = m_core->serverManager();
    const QList<ServerProfile> all = sm->servers();
    const ServerProfile target = [&]() {
        for (const ServerProfile &p : all) {
            if (p.id == serverId) return p;
        }
        return ServerProfile{};
    }();
    if (target.id.isEmpty()) co_return;

    // Apply the switch eagerly so the sidebar library refresh, search box
    // placeholder and any dashboard re-bindings react immediately. The active
    // playback window is intentionally left running — it was constructed with
    // a snapshot of the old ApiClient and is streaming through mpv, so it
    // keeps playing even after the underlying client is retired 30s later
    // (see ServerManager::retireActiveClient). The user explicitly asked
    // for this behaviour.
    sm->setActiveServer(serverId);
    ModernToast::showMessage(tr("Switched to %1").arg(displayName), 1500);

    // 切服后内容区回主页：停留在旧服的详情页/媒体库页没有意义——旧服的
    // item id / 库 id 在新服不存在，继续操作只会请求失败或显示空白。
    // goHome() 在已是主页时刷新 dashboard 数据（新服），否则清栈回主页。
    goHome();

    // Asynchronously verify reachability. If the probe later fails we tell
    // the dashboard to render an empty/error state instead of flashing the
    // old server's contents; the active server still flips because the
    // user explicitly picked one and we'd rather show a recoverable error
    // than refuse to navigate. Stale probes from earlier switches
    // self-cancel via m_serverProbeGeneration (independent from
    // m_profileRefreshGeneration — a shared counter would make
    // trySwitchToServer's probe invalidate the sidebar library refresh
    // that setActiveServer just started, leaving the new server's
    // libraries unapplied). No need to hold the Task here — every
    // co_await inside verifyServerReachability can be abandoned the same
    // way as if the user had been routed away.
    const int generation = ++m_serverProbeGeneration;
    launchTask(verifyServerReachability(target, generation, displayName), this);
}

QCoro::Task<void> HomeView::verifyServerReachability(const ServerProfile &target,
                                                      int generation,
                                                      const QString &displayName)
{
    ServerManager *sm = m_core ? m_core->serverManager() : nullptr;
    if (!sm) co_return;

    NetworkRequestOptions opts;
    opts.ignoreSslErrors = target.ignoreSslVerification;
    opts.timeoutMs = 10000;
    const QString probeUrl = target.url + QStringLiteral("/System/Info/Public");
    // Strict-UA servers silently drop connections from the Qt default UA,
    // which would make a perfectly fine server look "unreachable". Send
    // the same UA the API path uses (see ApiClient::getAuthHeaders).
    QMap<QString, QString> probeHeaders;
    probeHeaders.insert(QStringLiteral("User-Agent"),
                        target.effectiveUserAgent());
    bool reachable = false;
    try {
        const QJsonObject body =
            co_await sm->network()->get(probeUrl, probeHeaders, opts);
        reachable = !body.value(QStringLiteral("ServerName")).toString().isEmpty()
                    || !body.isEmpty();
    } catch (...) {
        reachable = false;
    }

    // A newer switch has been initiated; the user already moved on, drop
    // this stale verification quietly.
    if (generation != m_serverProbeGeneration) co_return;

    if (reachable) co_return;

    // Server is unreachable: push the dashboard into an empty state so the
    // user sees the failure on the main content, not just a transient
    // toast. We do NOT roll back the active server — the click is treated
    // as the user's intent and we surface the error in place.
    ModernToast::showMessage(
        tr("Failed to connect to %1").arg(displayName), 3500);
    Q_EMIT serverUnreachable(target.id, displayName);
}
