#ifndef AGGREGATEDVIEWBASE_H
#define AGGREGATEDVIEWBASE_H

#include "../baseview.h"
#include <QVector>
#include <models/media/mediaitem.h>
#include <models/profile/serverprofile.h>

class HorizontalListViewGallery;
class QScrollArea;
class QVBoxLayout;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QToolButton;
class QEmbyCore;
class SearchAggregator;
class SmoothScrollController;

// =============================================================================
// AggregatedServerSection — 单个服务器的结果分组
//
// 展示：◆ 服务器名 (结果数) [加载中...]   ›
//        └── HorizontalListViewGallery（横向卡片列表）
//
// header 可点击（emit sectionClicked）→ 阶段5 跳转 Server Scoped 页面。
// =============================================================================
class AggregatedServerSection : public QWidget {
    Q_OBJECT
public:
    explicit AggregatedServerSection(QEmbyCore* core,
                                     const ServerProfile& profile,
                                     QWidget* parent = nullptr);

    void setItems(const QList<MediaItem>& items); // 填充结果（同时更新计数）
    void clearItems();                            // 清空结果（保留 widget，重置为空态）

    void mousePressEvent(QMouseEvent *event) override; // header 区域点击 emit sectionClicked
    void setLoading(bool loading);                // 加载中状态（header 右侧提示）
    QString serverId() const { return m_profile.id; }
    const ServerProfile& profile() const { return m_profile; }
    // 当前 section 已加载的结果（阶段5 Server Scoped 页面复用）。
    const QList<MediaItem>& items() const { return m_items; }

Q_SIGNALS:
    void sectionClicked(const ServerProfile& profile);
    // 转发自内部 gallery（AggregatedServerSection 非 BaseView，无法直接用
    // BaseView 信号，故向上转发由 AggregatedViewBase 统一处理）。
    void itemActivated(const MediaItem& item);
    void playRequested(const MediaItem& item);
    void favoriteRequested(const MediaItem& item);
    void moreMenuRequested(const MediaItem& item, const QPoint& globalPos);

private:
    QEmbyCore* m_core;
    ServerProfile m_profile;
    QList<MediaItem> m_items;       // 已加载结果（setItems 时保存）
    QLabel* m_headerLabel = nullptr;   // "◆ 服名"
    QLabel* m_countLabel = nullptr;    // "(N 项)"
    QLabel* m_loadingLabel = nullptr;  // "⏳ 加载中..."
    QToolButton* m_btnScrollLeft = nullptr;  // 头行左箭头（按内容可滚性启用）
    QToolButton* m_btnScrollRight = nullptr; // 头行右箭头
    HorizontalListViewGallery* m_gallery = nullptr;  // 实际使用：横向 gallery（dashboard 同款）
};

// =============================================================================
// AggregatedViewBase — 聚合视图公共基类
//
// 统一布局：顶部类别 tab（子类按需添加）+ 滚动区（按服务器分组的 section 列表）。
// 数据流（streaming）：
//   1. 子类 startLoad() 调 SearchAggregator
//   2. createSkeletonSections() 先为所有服务器建空 section（加载中态）
//   3. 每个服务器结果到达 → fillSection() 填充对应 section（先返回先显示）
//   4. 全部完成 → setAllSectionsLoaded()
// =============================================================================
class AggregatedViewBase : public BaseView {
    Q_OBJECT
public:
    explicit AggregatedViewBase(QEmbyCore* core, QWidget* parent = nullptr);
    ~AggregatedViewBase() override;

    // 子类实现：发起聚合查询（调用 SearchAggregator 的 searchAll /
    // getResumeItemsAllServers / getFavoritesAllServers）。
    virtual void startLoad() = 0;

    // 清空全部 section（新查询/切 tab 时调用）。
    void clearSections();

    // 取某个服务器当前已加载的结果（阶段5 Server Scoped 页面复用；
    // 未匹配到则返回空列表）。
    QList<MediaItem> itemsForServer(const ServerProfile& profile) const;

    // Server Scoped 页面的面包屑标题（子类覆盖，如"搜索：九门"/"继续观看"/
    // "收藏"）；默认返回服务器名。
    virtual QString scopedPageTitle(const ServerProfile& profile) const;

Q_SIGNALS:
    // 点击某个服务器的 section header → 跳转 Server Scoped 页面（阶段5）。
    void serverScopedRequested(const ServerProfile& profile);

protected:
    // 按 servers() 顺序为每个服务器创建空 section（loading 态）。
    void createSkeletonSections();
    // 填充某个服务器的结果（按 serverId 定位 section）。
    void fillSection(const ServerProfile& profile, const QList<MediaItem>& items);
    // 全部服务器查询结束：清除所有 loading 标记。
    void setAllSectionsLoaded();
    // 添加一个类别 tab（全部/影视/剧集...），点击时回调 onCategoryTabClicked。
    QPushButton* addCategoryTab(const QString& label, bool checked = false);
    // 子类覆盖：类别 tab 被点击（重新 startLoad）。
    virtual void onCategoryTabClicked(const QString& label);

    QEmbyCore* m_core = nullptr;
    SearchAggregator* m_aggregator = nullptr;

    QWidget* m_tabBar = nullptr;
    QHBoxLayout* m_tabLayout = nullptr;
    QVector<QPushButton*> m_categoryTabs;

    QScrollArea* m_scrollArea = nullptr;
    QVBoxLayout* m_sectionsLayout = nullptr;
    QVector<AggregatedServerSection*> m_sections;
    SmoothScrollController* m_vScrollController = nullptr;

    QString m_currentTabLabel; // 当前激活 tab
};

#endif // AGGREGATEDVIEWBASE_H
