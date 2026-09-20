#ifndef AGGREGATEDFAVORITESVIEW_H
#define AGGREGATEDFAVORITESVIEW_H

#include "aggregatedviewbase.h"

// =============================================================================
// AggregatedFavoritesView — 聚合收藏（跨服务器收藏条目）
//
// 侧边栏「聚合收藏」按钮触发。类别 tab：电影 / 电视剧 / 单集 / 演员 / 合集，
// 结果按服务器分组展示。
// =============================================================================
class AggregatedFavoritesView : public AggregatedViewBase {
    Q_OBJECT
public:
    explicit AggregatedFavoritesView(QEmbyCore* core, QWidget* parent = nullptr);

    void startLoad() override;

protected:
    void onCategoryTabClicked(const QString& label) override;
    void showEvent(QShowEvent* event) override;
    // 面包屑标题：收藏
    QString scopedPageTitle(const ServerProfile& profile) const override;

private:
    QString includeTypesForTab(const QString& label) const;

    QString m_includeTypes = QStringLiteral("Movie");
};

#endif // AGGREGATEDFAVORITESVIEW_H
