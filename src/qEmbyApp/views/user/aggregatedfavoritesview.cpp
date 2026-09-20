#include "aggregatedfavoritesview.h"
#include <services/aggregator/searchaggregator.h>
#include <services/manager/servermanager.h>

namespace {
// 聚合视图每 server 预览条数（防图片请求风暴，见 aggregatedsearchview.cpp 注释）。
constexpr int kPerServerFavoritesLimit = 12;
}

AggregatedFavoritesView::AggregatedFavoritesView(QEmbyCore* core, QWidget* parent)
    : AggregatedViewBase(core, parent)
{
    // 类别 tab：电影 / 电视剧 / 单集 / 演员 / 合集（与单服务器收藏一致）。
    addCategoryTab(tr("电影"), /*checked=*/true);
    addCategoryTab(tr("电视剧"));
    addCategoryTab(tr("单集"));
    addCategoryTab(tr("演员"));
    addCategoryTab(tr("合集"));
}

void AggregatedFavoritesView::startLoad()
{
    if (!m_aggregator) {
        clearSections();
        return;
    }

    clearSections();
    createSkeletonSections();

    m_aggregator->getFavoritesAllServers(
        m_includeTypes, kPerServerFavoritesLimit,
        [this](const ServerProfile& profile, QList<MediaItem> items) {
            fillSection(profile, items);
        },
        [this](int /*attempted*/, int /*succeeded*/) {
            setAllSectionsLoaded();
        });
}

void AggregatedFavoritesView::onCategoryTabClicked(const QString& label)
{
    AggregatedViewBase::onCategoryTabClicked(label);
    m_includeTypes = includeTypesForTab(label);
    startLoad();
}

QString AggregatedFavoritesView::includeTypesForTab(const QString& label) const
{
    if (label == tr("电视剧")) return QStringLiteral("Series");
    if (label == tr("单集")) return QStringLiteral("Episode");
    if (label == tr("演员")) return QStringLiteral("Person");
    if (label == tr("合集")) return QStringLiteral("BoxSet");
    return QStringLiteral("Movie");
}

void AggregatedFavoritesView::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    startLoad();
}

QString AggregatedFavoritesView::scopedPageTitle(const ServerProfile& profile) const
{
    Q_UNUSED(profile);
    return tr("收藏");
}
