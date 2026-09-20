#ifndef AGGREGATEDSEARCHVIEW_H
#define AGGREGATEDSEARCHVIEW_H

#include "aggregatedviewbase.h"

// =============================================================================
// AggregatedSearchView — 聚合搜索视图
//
// 侧边栏「聚合搜索...」回车触发。在所有已添加服务器中搜索，结果按服务器分组
// 流式展示（先返回先显示）。
//
// 类别 tab：全部 / 影视 / 剧集 / 演员（切换时重新搜索，includeItemTypes 变化）。
// =============================================================================
class AggregatedSearchView : public AggregatedViewBase {
    Q_OBJECT
public:
    explicit AggregatedSearchView(QEmbyCore* core, QWidget* parent = nullptr);

    // 设置搜索词并触发加载（侧边栏回车 / 点击历史记录时调用）。
    void search(const QString& query);
    // 当前搜索词（供 server-scoped 页面/历史记录复用）。
    QString currentQuery() const { return m_query; }

    void startLoad() override;

protected:
    void onCategoryTabClicked(const QString& label) override;
    // 面包屑标题：搜索：<query>
    QString scopedPageTitle(const ServerProfile& profile) const override;

private:
    QString includeTypesForTab(const QString& label) const;

    QString m_query;
    QString m_includeTypes = QStringLiteral("Movie,Series,BoxSet,Person");
};

#endif // AGGREGATEDSEARCHVIEW_H
