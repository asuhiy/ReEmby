#include "aggregatedviewbase.h"
#include "../../components/horizontallistviewgallery.h"
#include "../../utils/smoothscrollcontroller.h"
#include <qembycore.h>
#include <services/aggregator/searchaggregator.h>
#include <services/manager/servermanager.h>
#include <config/config_keys.h>
#include <config/configstore.h>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QListView>
#include <QMouseEvent>
#include <QToolButton>
#include <QFrame>
#include <utility>

namespace {

// 该服务器是否参与聚合（设置 → 媒体库 → 参与聚合的服务）。
// 无键 = true（默认全部参与，与历史行为一致）；服务器被删除后清理见
// ServerManager::removeServer。
bool isAggregateEnabled(const ServerProfile& profile)
{
    if (!profile.isValid()) return false;
    return ConfigStore::instance()->get<bool>(
        ConfigKeys::forServer(profile.id, ConfigKeys::AggregateEnabled), true);
}

} // namespace

// ============================ AggregatedServerSection ============================

AggregatedServerSection::AggregatedServerSection(QEmbyCore* core,
                                                 const ServerProfile& profile,
                                                 QWidget* parent)
    : QWidget(parent)
    , m_core(core)
    , m_profile(profile)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 4);
    layout->setSpacing(4);

    // —— header 行：◆ 服名 (N 项) [加载中...]  [◀][▶]  › ——
    // 用 QWidget 容器（不用 QPushButton——QPushButton 默认 vertical
    // sizeHint 高度为 0，整块被压扁看不到文字）。点击进入 Scoped 用
    // widget 自身 mousePressEvent 实现（见事件处理实现）。
    auto* header = new QWidget(this);
    header->setObjectName(QStringLiteral("aggregate-server-header"));
    header->setCursor(Qt::PointingHandCursor);
    // 强制最小高度 28px，避免 QVBoxLayout 把 header 压扁。
    header->setMinimumHeight(28);

    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(8, 4, 8, 4);
    headerLayout->setSpacing(6);

    m_headerLabel = new QLabel(header);
    m_headerLabel->setObjectName(QStringLiteral("aggregate-server-name"));
    const QString name = profile.name.isEmpty() ? profile.url : profile.name;
    m_headerLabel->setText(QStringLiteral("\u25C6 %1").arg(name)); // ◆
    headerLayout->addWidget(m_headerLabel);

    m_countLabel = new QLabel(header);
    m_countLabel->setObjectName(QStringLiteral("aggregate-server-count"));
    m_countLabel->setText(QString());
    headerLayout->addWidget(m_countLabel);

    m_loadingLabel = new QLabel(header);
    m_loadingLabel->setObjectName(QStringLiteral("aggregate-server-loading"));
    m_loadingLabel->setText(QStringLiteral("\u23F3 %1").arg(tr("加载中...")));
    headerLayout->addWidget(m_loadingLabel);

    // 头行左右箭头（按 gallery 可滚性启用/禁用）
    m_btnScrollLeft = new QToolButton(header);
    m_btnScrollLeft->setObjectName("aggregate-header-arrow");
    m_btnScrollLeft->setText(QStringLiteral("\u25C0")); // ◀
    m_btnScrollLeft->setAutoRaise(true);
    m_btnScrollLeft->setCursor(Qt::PointingHandCursor);
    m_btnScrollLeft->setEnabled(false);
    headerLayout->addWidget(m_btnScrollLeft);

    m_btnScrollRight = new QToolButton(header);
    m_btnScrollRight->setObjectName("aggregate-header-arrow");
    m_btnScrollRight->setText(QStringLiteral("\u25B6")); // ▶
    m_btnScrollRight->setAutoRaise(true);
    m_btnScrollRight->setCursor(Qt::PointingHandCursor);
    m_btnScrollRight->setEnabled(false);
    headerLayout->addWidget(m_btnScrollRight);

    auto* chevron = new QLabel(QStringLiteral("\u203A"), header); // ›
    chevron->setObjectName(QStringLiteral("aggregate-server-chevron"));
    headerLayout->addWidget(chevron);

    // header 紧凑左对齐（不再 addStretch）
    layout->addWidget(header, 0, Qt::AlignLeft);

    // 点击进入 Scoped：mousePressEvent 拦截（QToolButton 箭头自动吞 mouse press 不冒泡）
    connect(m_btnScrollLeft, &QToolButton::clicked, this,
            [this]() { m_gallery->scrollByCardSteps(-1); });
    connect(m_btnScrollRight, &QToolButton::clicked, this,
            [this]() { m_gallery->scrollByCardSteps(1); });

    // —— 横向卡片 gallery（参考 dashboard 继续观看：按窗口宽度自适应
    // 一行显示 N 张，超出用头行左右箭头滑动切换；gallery 内不再放
    // 常驻箭头——箭头已移至 header 行，避免重复）——
    m_gallery = new HorizontalListViewGallery(core, this);
    m_gallery->setObjectName(QStringLiteral("aggregate-server-gallery"));
    // 与主界面 dashboard 卡片保持一致：同样的 CardStyle（默认 Poster，
    // 用户设置 DefaultLibraryView=tile 时 LibraryTile）+ 同样的固定高度。
    // 之前聚合页用 gallery 构造默认（Poster 160x270）且不设高度，若用户
    // 在主界面选了 tile 视图，聚合页仍是竖卡，视觉不一致。
    {
        const bool isTile =
            ConfigStore::instance()->get<QString>(ConfigKeys::DefaultLibraryView,
                                                  "poster") == "tile";
        const MediaCardDelegate::CardStyle style =
            isTile ? MediaCardDelegate::LibraryTile : MediaCardDelegate::Poster;
        m_gallery->setCardStyle(style);
        m_gallery->setFixedHeight(isTile ? 230 : 300);
    }
    // 宽度撑满 section（默认 Preferred 在部分父布局里只给 sizeHint 宽度，
    // 导致一行只显示 1-2 张卡片的假"显示不全"）。
    m_gallery->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // 头行箭头 enabled 状态跟随 gallery 滚动位置（按内容可滚性启用/禁用）
    auto updateHeaderArrows = [this]() {
        if (!m_gallery || !m_btnScrollLeft || !m_btnScrollRight) return;
        QScrollBar *bar = m_gallery->listView()->horizontalScrollBar();
        m_btnScrollLeft->setEnabled(bar->value() > 0);
        m_btnScrollRight->setEnabled(bar->value() < bar->maximum());
    };
    auto *hbar = m_gallery->listView()->horizontalScrollBar();
    connect(hbar, &QScrollBar::valueChanged, this, updateHeaderArrows);
    connect(hbar, &QScrollBar::rangeChanged, this, updateHeaderArrows);
    connect(m_gallery, &HorizontalListViewGallery::itemClicked, this,
            [this](const MediaItem& item) { Q_EMIT itemActivated(item); });
    connect(m_gallery, &HorizontalListViewGallery::playRequested, this,
            &AggregatedServerSection::playRequested);
    connect(m_gallery, &HorizontalListViewGallery::favoriteRequested, this,
            &AggregatedServerSection::favoriteRequested);
    connect(m_gallery, &HorizontalListViewGallery::moreMenuRequested, this,
            &AggregatedServerSection::moreMenuRequested);
    // gallery 给 vertical stretch=1，避免在 items 极少时 vertical layout
    // 把 gallery 高度压缩成窄条（头像被压成 1px 竖线 bug）。AlignTop 让
    // gallery 按 sizeHint 完整显示卡片，下方留白。
    layout->addWidget(m_gallery, 1, Qt::AlignTop);
}

void AggregatedServerSection::setItems(const QList<MediaItem>& items)
{
    m_items = items;
    // 空结果时整 section 隐藏（不显示 header/gallery，避免
    // "0 项"的空 section 堆在聚合视图里；同时解决 gallery 在 0/1 张
    // 卡片时高度被布局误压缩成窄条的 bug）。
    if (items.isEmpty()) {
        hide();
        m_gallery->setItems({});
        m_countLabel->setText(QString());
        setLoading(false);
        return;
    }
    show();
    m_gallery->setItems(items);
    m_countLabel->setText(tr("(%1)").arg(items.size()));
    setLoading(false);
}

void AggregatedServerSection::clearItems()
{
    m_items.clear();
    hide();
    m_gallery->setItems({});
    m_countLabel->setText(QString());
}

void AggregatedServerSection::setLoading(bool loading)
{
    if (m_loadingLabel) {
        m_loadingLabel->setVisible(loading);
    }
    if (m_countLabel) {
        m_countLabel->setVisible(!loading);
    }
}

// ============================ AggregatedViewBase ============================

AggregatedViewBase::AggregatedViewBase(QEmbyCore* core, QWidget* parent)
    : BaseView(core, parent)
    , m_core(core)
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // —— 顶部类别 tab 区（子类 addCategoryTab 填充）——
    m_tabBar = new QWidget(this);
    m_tabBar->setObjectName(QStringLiteral("aggregate-tab-bar"));
    m_tabLayout = new QHBoxLayout(m_tabBar);
    m_tabLayout->setContentsMargins(24, 12, 24, 0);
    m_tabLayout->setSpacing(24);
    m_tabLayout->addStretch(1);
    rootLayout->addWidget(m_tabBar);

    // —— 滚动区：按服务器分组的 section 列表 ——
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setObjectName(QStringLiteral("aggregate-scroll"));
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* container = new QWidget(m_scrollArea);
    container->setObjectName(QStringLiteral("aggregate-container"));
    m_sectionsLayout = new QVBoxLayout(container);
    m_sectionsLayout->setContentsMargins(24, 12, 24, 24);
    m_sectionsLayout->setSpacing(8);
    m_sectionsLayout->addStretch(1);
    m_scrollArea->setWidget(container);
    rootLayout->addWidget(m_scrollArea, 1);

    m_vScrollController =
        new SmoothScrollController(m_scrollArea->verticalScrollBar(), this);
    m_vScrollController->setDuration(160);

    // —— SearchAggregator：跨服务器 fan-out ——
    if (m_core && m_core->serverManager()) {
        m_aggregator = new SearchAggregator(
            m_core->serverManager(),
            m_core->serverManager()->network(), this);
    }
}

AggregatedViewBase::~AggregatedViewBase() = default;

void AggregatedViewBase::createSkeletonSections()
{
    if (!m_core || !m_core->serverManager()) return;

    const QList<ServerProfile> servers = m_core->serverManager()->servers();

    // Sections 常驻：只在服务器列表变化时重建 widget（避免反复
    // deleteLater/重建 gallery + QScroller grab 引发的时序风险）；
    // 相同列表时仅重置为加载中态。
    QStringList currentIds;
    for (const ServerProfile& profile : servers) {
        if (isAggregateEnabled(profile)) currentIds.append(profile.id);
    }
    QStringList existingIds;
    for (const AggregatedServerSection* section : std::as_const(m_sections)) {
        existingIds.append(section->serverId());
    }
    if (existingIds != currentIds) {
        clearSections();
        for (const ServerProfile& profile : servers) {
            if (!isAggregateEnabled(profile)) continue;
            auto* section = new AggregatedServerSection(m_core, profile, this);
            connect(section, &AggregatedServerSection::sectionClicked, this,
                    [this](const ServerProfile& p) {
                        Q_EMIT serverScopedRequested(p);
                    });
            // 卡片交互 → BaseView 统一处理（播放/收藏/详情/更多菜单）。
            connect(section, &AggregatedServerSection::itemActivated, this,
                    [this](const MediaItem& item) {
                        Q_EMIT navigateToDetail(item.id, item.name, item);
                    });
            connect(section, &AggregatedServerSection::playRequested, this,
                    &BaseView::handlePlayRequested);
            connect(section, &AggregatedServerSection::favoriteRequested, this,
                    &BaseView::handleFavoriteRequested);
            connect(section, &AggregatedServerSection::moreMenuRequested, this,
                    &BaseView::handleMoreMenuRequested);
            m_sectionsLayout->insertWidget(m_sectionsLayout->count() - 1, section);
            m_sections.append(section);
        }
    }

    // 重置全部 section 为加载中态。
    for (AggregatedServerSection* section : std::as_const(m_sections)) {
        section->clearItems();
        section->setLoading(true);
    }
}

void AggregatedViewBase::fillSection(const ServerProfile& profile,
                                     const QList<MediaItem>& items)
{
    for (AggregatedServerSection* section : std::as_const(m_sections)) {
        if (section->serverId() == profile.id) {
            section->setItems(items);
            return;
        }
    }
}

void AggregatedViewBase::setAllSectionsLoaded()
{
    for (AggregatedServerSection* section : std::as_const(m_sections)) {
        section->setLoading(false);
    }
}

void AggregatedViewBase::clearSections()
{
    for (AggregatedServerSection* section : std::as_const(m_sections)) {
        m_sectionsLayout->removeWidget(section);
        section->deleteLater();
    }
    m_sections.clear();
}

QList<MediaItem> AggregatedViewBase::itemsForServer(
    const ServerProfile& profile) const
{
    for (AggregatedServerSection* section : std::as_const(m_sections)) {
        if (section->serverId() == profile.id) {
            return section->items();
        }
    }
    return {};
}

QString AggregatedViewBase::scopedPageTitle(const ServerProfile& profile) const
{
    const QString name = profile.name.isEmpty() ? profile.url : profile.name;
    return name;
}

QPushButton* AggregatedViewBase::addCategoryTab(const QString& label, bool checked)
{
    auto* tab = new QPushButton(label, m_tabBar);
    tab->setObjectName(QStringLiteral("aggregate-category-tab"));
    tab->setCheckable(true);
    tab->setChecked(checked);
    tab->setCursor(Qt::PointingHandCursor);
    connect(tab, &QPushButton::clicked, this,
            [this, label]() { onCategoryTabClicked(label); });
    m_tabLayout->insertWidget(m_tabLayout->count() - 1, tab);
    m_categoryTabs.append(tab);
    if (checked) m_currentTabLabel = label;
    return tab;
}

void AggregatedViewBase::onCategoryTabClicked(const QString& label)
{
    m_currentTabLabel = label;
    for (QPushButton* tab : std::as_const(m_categoryTabs)) {
        tab->setChecked(tab->text() == label);
    }
    // 子类重写并调用 startLoad()。
}

void AggregatedServerSection::mousePressEvent(QMouseEvent *event)
{
    // header 区域点击：emit sectionClicked 进入 Scoped 页（箭头按钮
    // 自己处理 mouse press，不冒泡到这里——QToolButton 默认吞掉）。
    if (event->button() == Qt::LeftButton) {
        Q_EMIT sectionClicked(m_profile);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}
