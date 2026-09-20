#include "horizontallistviewgallery.h"
#include "shimmerwidget.h"
#include "../utils/textwraputils.h"
#include "../views/media/medialistmodel.h"
#include <QApplication>
#include <QAbstractScrollArea>
#include <QListView>
#include <QVBoxLayout>
#include <QPushButton>
#include <QScrollBar>
#include <QPropertyAnimation>
#include <QEvent>
#include <QHideEvent>
#include <QShowEvent>
#include <QWheelEvent>
#include <QCursor>
#include <QScroller>           
#include <QScrollerProperties> 
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QDebug>
#include <QSet>
#include <algorithm>

namespace {

bool intersectsVisibleAncestorChain(const QWidget* widget)
{
    if (!widget || !widget->isVisible()) {
        return false;
    }

    QRect visibleRect = widget->rect();
    const QWidget* child = widget;
    const QWidget* parent = child->parentWidget();
    while (parent) {
        visibleRect.moveTopLeft(child->mapTo(parent, visibleRect.topLeft()));
        visibleRect = visibleRect.intersected(parent->rect());
        if (visibleRect.isEmpty()) {
            return false;
        }
        child = parent;
        parent = child->parentWidget();
    }
    return true;
}

} 

HorizontalListViewGallery::HorizontalListViewGallery(QEmbyCore* core, QWidget* parent)
    : QWidget(parent), m_core(core), m_hScrollAnim(nullptr), m_hScrollTarget(0), m_cardStyle(MediaCardDelegate::Poster)
{
    setObjectName("horizontal-listview-gallery");
    setAttribute(Qt::WA_StyledBackground, true);
    setMouseTracking(true);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_listView = new QListView(this);
    m_listView->setViewMode(QListView::IconMode);
    m_listView->setFlow(QListView::LeftToRight);
    m_listView->setWrapping(false);
    m_listView->setSpacing(0);
    m_listView->setMovement(QListView::Static);
    m_listView->setResizeMode(QListView::Adjust);
    m_listView->setUniformItemSizes(true);

    m_listView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_listView->setFrameShape(QFrame::NoFrame);
    m_listView->setMouseTracking(true);
    m_listView->viewport()->setAttribute(Qt::WA_Hover);
    m_listView->setStyleSheet("QListView { background: transparent; outline: none; border: none; }");
    m_listView->setSelectionMode(QAbstractItemView::NoSelection);

    
    
    
    
    m_listView->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);

    
    QScroller::grabGesture(m_listView->viewport(), QScroller::LeftMouseButtonGesture);
    QScroller* scroller = QScroller::scroller(m_listView->viewport());
    QScrollerProperties props = scroller->scrollerProperties();
    
    props.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
    props.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
    
    props.setScrollMetric(QScrollerProperties::DragStartDistance, 0.001);
    scroller->setScrollerProperties(props);

    
    m_hScrollAnim = new QPropertyAnimation(m_listView->horizontalScrollBar(), "value", this);
    m_hScrollAnim->setEasingCurve(QEasingCurve::OutCubic);
    m_hScrollAnim->setDuration(450);

    mainLayout->addWidget(m_listView);

    
    m_shimmer = new ShimmerWidget(this);
    m_shimmer->hide();

    
    
    
    m_listModel = new MediaListModel(400, m_core, this);
    m_listDelegate = new MediaCardDelegate(MediaCardDelegate::Poster, this);

    m_listView->setModel(m_listModel);
    m_listView->setItemDelegate(m_listDelegate);

    
    connect(m_listDelegate, &MediaCardDelegate::playRequested, this, &HorizontalListViewGallery::playRequested);
    connect(m_listDelegate, &MediaCardDelegate::favoriteRequested, this, &HorizontalListViewGallery::favoriteRequested);
    connect(m_listDelegate, &MediaCardDelegate::moreMenuRequested, this, &HorizontalListViewGallery::moreMenuRequested);

    
    connect(m_listView, &QListView::clicked, this, [this](const QModelIndex& index) {
        if (m_listModel) {
            Q_EMIT itemClicked(m_listModel->getItem(index));
        }
    });

    
    
    
    m_btnLeft = new QPushButton("❮", this);
    m_btnRight = new QPushButton("❯", this);

    QString btnStyle = "QPushButton { background-color: rgba(0,0,0,120); color: white; font-size: 20px; border: none; border-radius: 8px; }"
                       "QPushButton:hover { background-color: rgba(60,60,60,200); }";
    m_btnLeft->setStyleSheet(btnStyle);
    m_btnRight->setStyleSheet(btnStyle);

    m_btnLeft->setFixedSize(40, 60);
    m_btnRight->setFixedSize(40, 60);
    m_btnLeft->setCursor(Qt::PointingHandCursor);
    m_btnRight->setCursor(Qt::PointingHandCursor);
    m_btnLeft->setFocusPolicy(Qt::NoFocus);
    m_btnRight->setFocusPolicy(Qt::NoFocus);

    m_btnLeft->hide();
    m_btnRight->hide();

    connect(m_btnLeft, &QPushButton::clicked, this, [this]() { scrollByCardSteps(-1); });
    connect(m_btnRight, &QPushButton::clicked, this, [this]() { scrollByCardSteps(1); });

    connect(m_listView->horizontalScrollBar(), &QScrollBar::valueChanged, this,
            [this]() {
                updateButtonsVisibility();
                updateVisibleImagePriority();
            });
    connect(m_listView->horizontalScrollBar(), &QScrollBar::rangeChanged, this, &HorizontalListViewGallery::updateButtonsVisibility);

    m_listView->viewport()->installEventFilter(this);
    this->installEventFilter(this);
}

HorizontalListViewGallery::~HorizontalListViewGallery()
{
    if (m_hScrollAnim) {
        m_hScrollAnim->stop();
    }
    if (m_listView && m_listView->viewport()) {
        m_listView->viewport()->removeEventFilter(this);
        QScroller::ungrabGesture(m_listView->viewport());
    }
    removeEventFilter(this);
}


void HorizontalListViewGallery::setItems(const QList<MediaItem>& items)
{
    if (m_listModel) {
        m_listModel->setItems(items);
        QTimer::singleShot(0, this,
                           [this]() { updateVisibleImagePriority(); });
    }
    
    
    
    if (m_shimmer && !items.isEmpty()) {
        m_shimmer->stopAnimation();
        m_shimmer->hide();
    }
}


void HorizontalListViewGallery::updateItem(const MediaItem& item)
{
    if (m_listModel) {
        m_listModel->updateItem(item);
    }
}

void HorizontalListViewGallery::prependOrUpdateItem(const MediaItem& item,
                                                    int maxItems)
{
    if (m_listModel) {
        m_listModel->prependOrUpdateItem(item, maxItems);
        QTimer::singleShot(0, this, [this]() {
            updateVisibleImagePriority();
            updateButtonsVisibility();
        });
    }
}


void HorizontalListViewGallery::removeItem(const QString& itemId)
{
    if (m_listModel) {
        const int previousCount = m_listModel->rowCount();
        m_listModel->removeItem(itemId);
        if (m_listView && m_listModel->rowCount() < previousCount) {
            m_listView->doItemsLayout();
            m_listView->updateGeometry();
            m_listView->update();
            m_listView->viewport()->update();
            QTimer::singleShot(0, this, [this]() {
                if (!m_listView) {
                    return;
                }

                m_listView->doItemsLayout();
                m_listView->viewport()->update();
                updateButtonsVisibility();
            });
        }
    }
}

int HorizontalListViewGallery::itemCount() const
{
    return m_listModel ? m_listModel->rowCount() : 0;
}

QList<MediaItem> HorizontalListViewGallery::items() const
{
    return m_listModel ? m_listModel->items() : QList<MediaItem> {};
}

void HorizontalListViewGallery::clearImageCache()
{
    if (m_listModel) {
        m_listModel->clearImageCache();
    }
}

void HorizontalListViewGallery::setForceNetworkImages(bool forceNetwork)
{
    if (m_listModel) {
        m_listModel->setForceNetworkImages(forceNetwork);
    }
}

void HorizontalListViewGallery::clearFailedImageItems()
{
    if (m_listModel) {
        m_listModel->clearFailedImageItems();
    }
}

void HorizontalListViewGallery::setCardStyle(MediaCardDelegate::CardStyle style)
{
    m_cardStyle = style; 
    if (m_listDelegate) {
        m_listDelegate->setStyle(style);

        
        if (style == MediaCardDelegate::LibraryTile) {
            int imgHeight = 160;
            int imgWidth = qRound(imgHeight * 16.0 / 9.0); 
            int cardWidth = imgWidth + 16;  
            const int hoverExpandH = qRound(imgHeight * 0.035);
            const int cardHeight =
                8 + imgHeight + hoverExpandH + 4 + 20 + 1 + 18 + 8;
            m_listDelegate->setTileSize(QSize(cardWidth, cardHeight));
        } else if (style == MediaCardDelegate::Poster) {
            m_listDelegate->setTileSize(QSize(160, 270));
        }

        
        m_listView->doItemsLayout();
        m_listView->viewport()->update();
    }
    
    if (m_listModel) {
        m_listModel->setPreferThumb(style == MediaCardDelegate::LibraryTile || style == MediaCardDelegate::EpisodeList);
        updateImageRequestSize();
    }
    
    updateButtonPositions();
}

void HorizontalListViewGallery::setTileSize(const QSize &size)
{
    if (m_listDelegate) {
        m_listDelegate->setTileSize(size);
        m_listView->doItemsLayout();
        m_listView->viewport()->update();
        // 防高度裁切：tileSize 高度不含 QListView frame(≈2px×2) + 滚动条(≈15px)，
        // 若 gallery 总高度 = tileSize.height()，item 底部会被裁约 19px。
        // 加 24px 缓冲保证 delegate paint 区域完整。
        m_listView->setMinimumHeight(size.height() + 24);
    }
    updateImageRequestSize();
    updateButtonPositions();
}

void HorizontalListViewGallery::setTextPixelSizes(int titlePx, int subTitlePx)
{
    if (m_listDelegate) {
        m_listDelegate->setTextPixelSizes(titlePx, subTitlePx);
        m_listView->doItemsLayout();
        m_listView->viewport()->update();
    }
}

void HorizontalListViewGallery::setContentPadding(int padding)
{
    if (m_listDelegate) {
        m_listDelegate->setContentPadding(padding);
        m_listView->doItemsLayout();
        m_listView->viewport()->update();
    }
    updateImageRequestSize();
    updateButtonPositions();
}

void HorizontalListViewGallery::setHoverControls(
    MediaCardDelegate::HoverControls controls)
{
    if (m_listDelegate) {
        m_listDelegate->setHoverControls(controls);
        m_listView->viewport()->update();
    }
}

void HorizontalListViewGallery::scrollToItemId(const QString &itemId)
{
    const QString targetItemId = itemId.trimmed();
    if (targetItemId.isEmpty() || !m_listView || !m_listModel) {
        return;
    }

    QTimer::singleShot(0, this, [this, targetItemId]() {
        if (!m_listView || !m_listModel) {
            return;
        }

        m_listView->doItemsLayout();
        const int rowCount = m_listModel->rowCount();
        for (int row = 0; row < rowCount; ++row) {
            const QModelIndex index = m_listModel->index(row, 0);
            const MediaItem item =
                index.data(MediaListModel::ItemDataRole).value<MediaItem>();
            if (item.id != targetItemId) {
                continue;
            }

            qDebug() << "[HorizontalListViewGallery] Scroll to item"
                     << "| itemId=" << targetItemId
                     << "| row=" << row;
            m_listView->scrollTo(index, QAbstractItemView::PositionAtCenter);
            return;
        }

        qDebug() << "[HorizontalListViewGallery] Scroll target not found"
                 << "| itemId=" << targetItemId
                 << "| rowCount=" << rowCount;
    });
}

void HorizontalListViewGallery::setHighlightedItemId(const QString &id)
{
    if (m_listDelegate) {
        m_listDelegate->setHighlightedItemId(id);
        m_listView->viewport()->update();
    }
}

void HorizontalListViewGallery::scrollByCardSteps(int steps)
{
    if (!m_listView || !m_listModel) {
        return;
    }
    QScrollBar *bar = m_listView->horizontalScrollBar();
    if (!bar) {
        return;
    }

    // 单卡片像素宽：优先取第 0 个 item 的实际布局宽度，回退 220。
    int cardWidth = 220;
    if (m_listModel->rowCount() > 0) {
        const QModelIndex idx = m_listModel->index(0, 0);
        if (idx.isValid()) {
            const QRect r = m_listView->visualRect(idx);
            if (r.width() > 0) {
                cardWidth = r.width();
            }
        }
    }

    // 用户要求：点击一次移动 2 张卡片。
    const int step = qMax(1, cardWidth) * 2;
    const int targetValue =
        qBound(0, bar->value() + steps * step, bar->maximum());

    auto *anim = new QPropertyAnimation(bar, "value", this);
    anim->setDuration(400);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->setStartValue(bar->value());
    anim->setEndValue(targetValue);
    anim->start(QAbstractAnimation::DeleteWhenStopped);

    // 滚动后同步按钮可见性（常驻箭头：左可见=有内容可滚回，右可见=还有内容）。
    updateButtonsVisibility();
}

void HorizontalListViewGallery::setLoading(bool loading)
{
    if (!m_shimmer) {
        return;
    }

    if (loading) {
        
        QStyleOptionViewItem opt;
        const QSize cardSize = m_listDelegate->sizeHint(opt, QModelIndex());
        m_shimmer->setCardSize(cardSize);
        m_shimmer->setShowSubtitle(
            m_cardStyle == MediaCardDelegate::Poster ||
            m_cardStyle == MediaCardDelegate::Cast);
        m_shimmer->setGeometry(m_listView->geometry());
        m_shimmer->raise();
        m_shimmer->show();
        m_shimmer->startAnimation();
    } else {
        m_shimmer->stopAnimation();
        m_shimmer->hide();
    }
}

void HorizontalListViewGallery::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateButtonPositions();
    updateButtonsVisibility();
    updateVisibleImagePriority();

    
    if (m_shimmer && m_shimmer->isVisible()) {
        m_shimmer->setGeometry(m_listView->geometry());
    }
}

void HorizontalListViewGallery::hideEvent(QHideEvent* event)
{
    if (m_listModel && !m_imageRequestsSuspendedForVisibility) {
        m_listModel->suspendImageRequests();
        m_imageRequestsSuspendedForVisibility = true;
    }
    QWidget::hideEvent(event);
}

void HorizontalListViewGallery::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (m_listModel && m_imageRequestsSuspendedForVisibility) {
        m_listModel->resumeImageRequests();
        m_imageRequestsSuspendedForVisibility = false;
    }
    QTimer::singleShot(0, this, [this]() {
        updateVisibleImagePriority();
        if (m_listView && m_listView->viewport()) {
            m_listView->viewport()->update();
        }
    });
}

void HorizontalListViewGallery::updateButtonPositions()
{
    int currentWidth = this->width();
    
    
    QStyleOptionViewItem dummyOption;
    QModelIndex dummyIndex;
    QSize itemSize = m_listDelegate->sizeHint(dummyOption, dummyIndex);
    
    int padding = m_listDelegate ? m_listDelegate->contentPadding() : 8;
    int imgWidth = itemSize.width() - padding * 2;
    int imgHeight = 0;
    
    
    if (m_cardStyle == MediaCardDelegate::LibraryTile) {
        imgHeight = qRound(imgWidth * 9.0 / 16.0);
    } else {
        
        imgHeight = qRound(imgWidth * 1.5);
    }
    
    
    int imageCenterY = padding + (imgHeight / 2);
    int btnY = imageCenterY - (m_btnLeft->height() / 2);

    m_btnLeft->move(10, btnY);
    m_btnRight->move(currentWidth - m_btnRight->width() - 10, btnY);

    m_btnLeft->raise();
    m_btnRight->raise();
}

bool HorizontalListViewGallery::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::Enter || event->type() == QEvent::MouseMove) {
        updateButtonsVisibility();
    } else if (event->type() == QEvent::Leave) {
        QPoint globalPos = QCursor::pos();
        if (!this->rect().contains(this->mapFromGlobal(globalPos))) {
            m_btnLeft->hide();
            m_btnRight->hide();
        }
    } else if (obj == m_listView->viewport() && event->type() == QEvent::ToolTip) {
        return TextWrapUtils::showWrappedMediaItemToolTip(m_listView, event);
    } else if (event->type() == QEvent::Wheel) {
        
        QWheelEvent* wheelEvent = static_cast<QWheelEvent*>(event);
        if (qAbs(wheelEvent->angleDelta().x()) > qAbs(wheelEvent->angleDelta().y())) {
            
            if (!m_listView || !m_hScrollAnim || !m_hScrollAnim->targetObject()) {
                wheelEvent->ignore();
                return false;
            }
            QScrollBar* hBar = m_listView->horizontalScrollBar();
            if (hBar) {
                int currentVal = hBar->value();
                if (m_hScrollAnim->state() == QAbstractAnimation::Running) {
                    currentVal = m_hScrollTarget;
                }
                int step = wheelEvent->angleDelta().x();
                int newTarget = currentVal - step;
                newTarget = qBound(hBar->minimum(), newTarget, hBar->maximum());

                if (newTarget != hBar->value()) {
                    m_hScrollTarget = newTarget;
                    m_hScrollAnim->stop();
                    m_hScrollAnim->setStartValue(hBar->value());
                    m_hScrollAnim->setEndValue(m_hScrollTarget);
                    m_hScrollAnim->start();
                }
            }
            return true; 
        } else {
            // 垂直滚轮：不消费成 gallery 水平滚动——QAbstractScrollArea
            // 默认会把垂直滚轮 fallback 到水平 scrollbar（水平 bar 可滚时），
            // 吞掉事件导致外层滚动区（聚合结果页/dashboard）无法上下滚动。
            // 转发给最近 QAbstractScrollArea 祖先的 viewport 实现页面滚动
            //（wheel 处理逻辑挂在 viewportEvent 上，发给 scrollarea 本身
            // 不会被处理）。
            QWidget *w = parentWidget();
            while (w && !qobject_cast<QAbstractScrollArea *>(w))
                w = w->parentWidget();
            if (w) {
                QApplication::sendEvent(
                    static_cast<QAbstractScrollArea *>(w)->viewport(), event);
            } else {
                wheelEvent->ignore();
            }
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

void HorizontalListViewGallery::updateButtonsVisibility()
{
    if (m_persistentArrows) {
        // 聚合视图常驻箭头：有内容可滚即显示，不依赖鼠标位置。
        QScrollBar* bar = m_listView->horizontalScrollBar();
        if (!bar) {
            m_btnLeft->hide();
            m_btnRight->hide();
            return;
        }
        m_btnLeft->setVisible(bar->value() > 0);
        m_btnRight->setVisible(bar->value() < bar->maximum());
        return;
    }

    QPoint globalPos = QCursor::pos();
    QPoint localPos = this->mapFromGlobal(globalPos);

    if (!this->rect().contains(localPos)) {
        m_btnLeft->hide();
        m_btnRight->hide();
        return;
    }

    int currentWidth = this->width();
    QScrollBar* bar = m_listView->horizontalScrollBar();

    bool isLeftHalf = localPos.x() < (currentWidth / 2);

    m_btnLeft->setVisible(isLeftHalf && bar->value() > 0);
    m_btnRight->setVisible(!isLeftHalf && bar->value() < bar->maximum());
}

void HorizontalListViewGallery::setPersistentArrows(bool persistent)
{
    m_persistentArrows = persistent;
    updateButtonsVisibility();
}

void HorizontalListViewGallery::updateVisibleImagePriority()
{
    if (!m_listView || !m_listModel) {
        return;
    }

    
    
    
    
    if (!intersectsVisibleAncestorChain(this)) {
        m_listModel->setPriorityRows({});
        return;
    }

    QWidget* viewport = m_listView->viewport();
    if (!viewport) {
        return;
    }

    QStyleOptionViewItem option;
    const QSize cellSize = m_listDelegate->sizeHint(option, QModelIndex());
    const int stepX = qMax(1, cellSize.width() / 2);
    const int stepY = qMax(1, cellSize.height() / 2);

    QSet<int> rowSet;
    const QRect rect = viewport->rect();
    for (int y = rect.top(); y <= rect.bottom(); y += stepY) {
        for (int x = rect.left(); x <= rect.right(); x += stepX) {
            const QModelIndex idx = m_listView->indexAt(QPoint(x, y));
            if (idx.isValid()) {
                rowSet.insert(idx.row());
            }
        }
    }

    QList<int> rows = rowSet.values();
    std::sort(rows.begin(), rows.end());
    m_listModel->setPriorityRows(rows);
}

void HorizontalListViewGallery::updateImageRequestSize()
{
    if (!m_listModel || !m_listDelegate) {
        return;
    }

    QStyleOptionViewItem option;
    const QSize cardSize = m_listDelegate->sizeHint(option, QModelIndex());
    const int displayWidth =
        qMax(1, cardSize.width() - m_listDelegate->contentPadding() * 2);
    const int requestWidth = qBound(
        160, qRound(displayWidth * devicePixelRatioF()), 768);
    m_listModel->setImageMaxWidth(requestWidth);
}
