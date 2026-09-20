#include "serverscopedview.h"
#include "../../components/mediagridwidget.h"
#include <qembycore.h>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QFrame>

ServerScopedView::ServerScopedView(QEmbyCore* core, QWidget* parent)
    : BaseView(core, parent)
    , m_core(core)
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // —— 面包屑行：[← 返回]  ◆ 服名 · 标题 ——
    auto* crumbBar = new QWidget(this);
    crumbBar->setObjectName(QStringLiteral("scoped-crumb-bar"));
    auto* crumbLayout = new QHBoxLayout(crumbBar);
    crumbLayout->setContentsMargins(24, 14, 24, 10);
    crumbLayout->setSpacing(10);

    auto* backBtn = new QPushButton(crumbBar);
    backBtn->setObjectName(QStringLiteral("scoped-back-btn"));
    backBtn->setText(QStringLiteral("\u2190 %1").arg(tr("返回"))); // ← 返回
    backBtn->setCursor(Qt::PointingHandCursor);
    backBtn->setFlat(true);
    connect(backBtn, &QPushButton::clicked, this, &ServerScopedView::navigateBack);
    crumbLayout->addWidget(backBtn);

    auto* sep = new QLabel(QStringLiteral("|"), crumbBar);
    sep->setObjectName(QStringLiteral("scoped-crumb-sep"));
    crumbLayout->addWidget(sep);

    m_titleLabel = new QLabel(crumbBar);
    m_titleLabel->setObjectName(QStringLiteral("scoped-title"));
    crumbLayout->addWidget(m_titleLabel);
    crumbLayout->addStretch(1);

    rootLayout->addWidget(crumbBar);

    // —— 分隔线 ——
    auto* line = new QFrame(this);
    line->setObjectName(QStringLiteral("scoped-sep-line"));
    line->setFrameShape(QFrame::HLine);
    rootLayout->addWidget(line);

    // —— 主体网格 ——
    m_grid = new MediaGridWidget(core, this);
    m_grid->setObjectName(QStringLiteral("scoped-grid"));
    m_grid->setBasePadding(24);
    rootLayout->addWidget(m_grid, 1);

    // 卡片交互 → BaseView 统一路由（详情/播放/收藏/更多菜单），不切服。
    connect(m_grid, &MediaGridWidget::itemClicked, this,
            [this](const MediaItem& item) {
                Q_EMIT navigateToDetail(item.id, item.name, item);
            });
    connect(m_grid, &MediaGridWidget::playRequested, this,
            &BaseView::handlePlayRequested);
    connect(m_grid, &MediaGridWidget::favoriteRequested, this,
            &BaseView::handleFavoriteRequested);
    connect(m_grid, &MediaGridWidget::moreMenuRequested, this,
            &BaseView::handleMoreMenuRequested);
}

void ServerScopedView::setContext(const ServerProfile& profile,
                                  const QList<MediaItem>& items,
                                  const QString& title)
{
    m_profile = profile;
    const QString name = profile.name.isEmpty() ? profile.url : profile.name;
    m_titleLabel->setText(QStringLiteral("\u25C6 %1  \u00B7  %2").arg(name, title)); // ◆ 服名 · 标题
    m_grid->setItems(items);
}
