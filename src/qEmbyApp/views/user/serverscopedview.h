#ifndef SERVERSCOPEDVIEW_H
#define SERVERSCOPEDVIEW_H

#include "../baseview.h"
#include <models/media/mediaitem.h>
#include <models/profile/serverprofile.h>

class QEmbyCore;
class MediaGridWidget;
class QLabel;
class QPushButton;

// =============================================================================
// ServerScopedView — 单个服务器的全部结果页（阶段5）
//
// 从聚合视图点击某个服务器的 section header 进入：面包屑
//   [← 返回]  ◆ 服名 · 上下文标题
// 主体为该服务器已加载的全部结果（MediaGridWidget 网格）。
//
// 返回：面包屑的返回按钮 emit navigateBack()，HomeView 统一 pop 回聚合视图。
// 播放/收藏/详情复用 BaseView 路由（不切服，点击卡片 → 详情页可直连播放）。
// =============================================================================
class ServerScopedView : public BaseView {
    Q_OBJECT
public:
    explicit ServerScopedView(QEmbyCore* core, QWidget* parent = nullptr);

    // 设置上下文并展示（items 来自聚合视图已加载的结果）。
    void setContext(const ServerProfile& profile,
                    const QList<MediaItem>& items,
                    const QString& title);

private:
    QEmbyCore* m_core = nullptr;
    ServerProfile m_profile;
    MediaGridWidget* m_grid = nullptr;
    QLabel* m_titleLabel = nullptr;
};

#endif // SERVERSCOPEDVIEW_H
