#include "aggregatedhistoryview.h"
#include "../../utils/resumeitemresolver.h"
#include <services/aggregator/searchaggregator.h>
#include <services/manager/servermanager.h>

namespace {
// 聚合视图每 server 预览条数（防图片请求风暴，见 aggregatedsearchview.cpp 注释）。
constexpr int kPerServerHistoryLimit = 12;
}

AggregatedHistoryView::AggregatedHistoryView(QEmbyCore* core, QWidget* parent)
    : AggregatedViewBase(core, parent)
{
}

void AggregatedHistoryView::startLoad()
{
    if (!m_aggregator) {
        clearSections();
        return;
    }

    clearSections();
    createSkeletonSections();

    m_aggregator->getResumeItemsAllServers(
        kPerServerHistoryLimit,
        [this](const ServerProfile& profile, QList<MediaItem> items) {
            // 与主页 dashboard 继续观看同源折叠：同一 series 的多条 episode
            // 折叠成一条 Series 卡片（保留最新播放一集的进度上下文）。
            // 否则同一部剧的每一集都出现，聚合历史出现大量重复卡片。
            items = ResumeItemResolver::buildFallbackItems(
                std::move(items), QStringLiteral("aggregate-history"));
            fillSection(profile, items);
        },
        [this](int /*attempted*/, int /*succeeded*/) {
            setAllSectionsLoaded();
        });
}

void AggregatedHistoryView::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    // 每次进入视图刷新一次（历史进度随时在变）。
    startLoad();
}

QString AggregatedHistoryView::scopedPageTitle(const ServerProfile& profile) const
{
    Q_UNUSED(profile);
    return tr("继续观看");
}
