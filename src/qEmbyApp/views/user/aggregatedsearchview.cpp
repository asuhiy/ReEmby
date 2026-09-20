#include "aggregatedsearchview.h"
#include <services/aggregator/searchaggregator.h>
#include <services/manager/servermanager.h>

namespace {
// 聚合视图每 server 预览条数：刻意保持小值。8+ server × limit 张卡片
// 同时渲染会触发海量图片请求（400+ 并发涌入 MediaService 图片调度队列，
// 实测崩溃）；要看该服全部结果点击 section header 进 Server Scoped 页。
constexpr int kPerServerSearchLimit = 12;
}

AggregatedSearchView::AggregatedSearchView(QEmbyCore* core, QWidget* parent)
    : AggregatedViewBase(core, parent)
{
    // 类别 tab：全部 / 影视 / 剧集 / 演员。
    addCategoryTab(tr("全部"), /*checked=*/true);
    addCategoryTab(tr("电影"));
    addCategoryTab(tr("剧集"));
    addCategoryTab(tr("演员"));
}

void AggregatedSearchView::search(const QString& query)
{
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) return;
    m_query = trimmed;
    startLoad();
}

void AggregatedSearchView::startLoad()
{
    if (m_query.isEmpty() || !m_aggregator) {
        clearSections();
        return;
    }

    clearSections();
    createSkeletonSections();

    // 流式：每个服务器结果到达 → fillSection（先返回先显示）。
    m_aggregator->searchAll(
        m_query, m_includeTypes, kPerServerSearchLimit,
        [this](const ServerProfile& profile, QList<MediaItem> items) {
            fillSection(profile, items);
        },
        [this](int /*attempted*/, int /*succeeded*/) {
            setAllSectionsLoaded();
        });
}

void AggregatedSearchView::onCategoryTabClicked(const QString& label)
{
    AggregatedViewBase::onCategoryTabClicked(label);
    m_includeTypes = includeTypesForTab(label);
    startLoad();
}

QString AggregatedSearchView::scopedPageTitle(const ServerProfile& profile) const
{
    Q_UNUSED(profile);
    if (m_query.isEmpty()) return tr("搜索");
    return tr("搜索：%1").arg(m_query);
}

QString AggregatedSearchView::includeTypesForTab(const QString& label) const
{
    if (label == tr("电影")) {
        return QStringLiteral("Movie");
    }
    if (label == tr("剧集")) {
        return QStringLiteral("Series");
    }
    if (label == tr("演员")) {
        return QStringLiteral("Person");
    }
    // "全部" / 默认：电影 + 剧集 + 合集 + 演员。
    return QStringLiteral("Movie,Series,BoxSet,Person");
}
