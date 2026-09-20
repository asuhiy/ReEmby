#ifndef AGGREGATEDHISTORYVIEW_H
#define AGGREGATEDHISTORYVIEW_H

#include "aggregatedviewbase.h"

// =============================================================================
// AggregatedHistoryView — 聚合历史（跨服务器「继续观看」）
//
// 侧边栏「聚合历史」按钮触发。每个服务器的可续看条目（Filters=IsResumable）
// 按服务器分组展示。
// =============================================================================
class AggregatedHistoryView : public AggregatedViewBase {
    Q_OBJECT
public:
    explicit AggregatedHistoryView(QEmbyCore* core, QWidget* parent = nullptr);

    void startLoad() override;

protected:
    void showEvent(QShowEvent* event) override;
    // 面包屑标题：继续观看
    QString scopedPageTitle(const ServerProfile& profile) const override;
};

#endif // AGGREGATEDHISTORYVIEW_H
