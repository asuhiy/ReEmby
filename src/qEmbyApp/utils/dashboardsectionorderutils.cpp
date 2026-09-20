#include "dashboardsectionorderutils.h"

namespace {

bool isKnownSectionId(const QString& sectionId)
{
    return sectionId == QLatin1String(DashboardSectionOrderUtils::ContinueWatchingSectionId) ||
           sectionId == QLatin1String(DashboardSectionOrderUtils::LatestMediaSectionId) ||
           sectionId == QLatin1String(DashboardSectionOrderUtils::RecommendedSectionId) ||
           sectionId == QLatin1String(DashboardSectionOrderUtils::CompletedWatchingSectionId) ||
           sectionId == QLatin1String(DashboardSectionOrderUtils::AllLibrariesSectionId) ||
           sectionId == QLatin1String(
               DashboardSectionOrderUtils::EachLibrarySectionsSectionId);
}

} 

namespace DashboardSectionOrderUtils {

QStringList defaultSectionOrder()
{
    // 为你推荐 (Emby 官方原生推荐) 作为首屏第一眼内容优先展示，
    // 后面依次是断点续看、最新入库。其他保持原相对顺序。
    // 注意：DashboardView::applyDashboardSectionOrder() 在 setupUi /
    // 切服 / refresh 时都会按这个列表 insertWidget —— 改动直接生效，
    // 无需同步改 dashboardview.cpp:setupUi 里的 addWidget 顺序。
    return {
        QString::fromLatin1(RecommendedSectionId),
        QString::fromLatin1(ContinueWatchingSectionId),
        QString::fromLatin1(LatestMediaSectionId),
        QString::fromLatin1(CompletedWatchingSectionId),
        QString::fromLatin1(AllLibrariesSectionId),
        QString::fromLatin1(EachLibrarySectionsSectionId)
    };
}

QStringList normalizeSectionOrder(QStringList order)
{
    QStringList normalized;
    normalized.reserve(defaultSectionOrder().size());

    for (QString& sectionId : order) {
        sectionId = sectionId.trimmed();
        if (sectionId.isEmpty() || !isKnownSectionId(sectionId) ||
            normalized.contains(sectionId)) {
            continue;
        }
        normalized.append(sectionId);
    }

    const QStringList defaults = defaultSectionOrder();
    for (int i = 0; i < defaults.size(); ++i) {
        const QString& sectionId = defaults[i];
        if (normalized.contains(sectionId)) {
            continue;
        }

        int insertIndex = normalized.size();
        for (int j = i + 1; j < defaults.size(); ++j) {
            const int nextIndex = normalized.indexOf(defaults[j]);
            if (nextIndex >= 0) {
                insertIndex = nextIndex;
                break;
            }
        }
        normalized.insert(insertIndex, sectionId);
    }

    return normalized;
}

} 
