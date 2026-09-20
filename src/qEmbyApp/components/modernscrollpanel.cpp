#include "modernscrollpanel.h"
#include "modernmenuscrollbar.h"
#include <QGraphicsDropShadowEffect>
#include <QPainter>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QScrollBar>

namespace {













constexpr int kMenuItemMinHeight = 42;
constexpr int kMenuItemVerticalPadding = 18;
constexpr int kMenuItemSpacing = 2;
constexpr int kScrollAreaFrameReserve = 2;

int menuItemHeight(const QFont &font)
{
    const QFontMetrics fm(font);
    return qMax(kMenuItemMinHeight, fm.height() + kMenuItemVerticalPadding);
}

}

ModernScrollPanel::ModernScrollPanel(QWidget *parent) : QFrame(parent), m_maxContentWidth(0) {
    setObjectName("modernScrollMenuOuter");

    setWindowFlags(Qt::Widget | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_StyledBackground, true);

    auto *shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(20);
    shadow->setColor(QColor(0, 0, 0, 150));
    shadow->setOffset(0, 4);
    setGraphicsEffect(shadow);

    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setContentsMargins(0, 0, 0, 0);
    m_mainLayout->setSpacing(0);

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setObjectName("modernMenuScrollArea");
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);

    
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    
    
    
    
    
    
    
    
    
    
    
    
    
    
    if (auto *vp = m_scrollArea->viewport()) {
        vp->setObjectName("modernMenuViewport");
        vp->setAutoFillBackground(false);
        vp->setAttribute(Qt::WA_StyledBackground, true);
    }

    
    
    
    
    
    
    
    
    auto *vBar = new ModernMenuScrollBar(Qt::Vertical, m_scrollArea);
    vBar->setObjectName("modernMenuScrollBar");  
                                                  
    m_scrollArea->setVerticalScrollBar(vBar);

    m_container = new QWidget(m_scrollArea);
    m_container->setObjectName("modernMenuContainer");
    m_container->setAttribute(Qt::WA_StyledBackground, true);

    m_layout = new QVBoxLayout(m_container);
    m_layout->setContentsMargins(4, 4, 4, 4);
    
    
    m_layout->setSpacing(kMenuItemSpacing);

    m_scrollArea->setWidget(m_container);
    m_mainLayout->addWidget(m_scrollArea);
}

void ModernScrollPanel::addItem(const QString &text, const QVariant &userData, bool isSelected,
                                bool hasSubmenu) {
    auto *btn = new QPushButton(m_container);
    btn->setObjectName("modernMenuItemBtn");

    QPixmap pixmap(18, 18);
    pixmap.fill(Qt::transparent);

    if (isSelected) {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QColor(255, 255, 255, 255));
        QFont font = painter.font();
        font.setPixelSize(14);
        font.setBold(true);
        painter.setFont(font);
        painter.drawText(pixmap.rect(), Qt::AlignCenter, "✓");
    }

    btn->setIcon(QIcon(pixmap));
    btn->setText(text); 

    
    btn->setToolTip(text);

    btn->setCursor(Qt::PointingHandCursor);
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setFixedHeight(menuItemHeight(btn->font()));

    // 悬停通知（级联子菜单展开用；WA_Hover 才会产生 HoverEnter 事件）。
    btn->setAttribute(Qt::WA_Hover, true);
    btn->installEventFilter(this);

    
    QFontMetrics fm(btn->font());
    m_maxContentWidth = qMax(m_maxContentWidth, fm.horizontalAdvance(text));
    if (hasSubmenu) {
        // 右侧 "▶" 宽度预留（finalizeLayout 里按此省略主体文本）。
        m_maxContentWidth =
            qMax(m_maxContentWidth, fm.horizontalAdvance(text) + 24);
    }

    
    m_items.append({btn, text, userData, hasSubmenu});

    connect(btn, &QPushButton::clicked, this, [this, userData, text]() {
        emit itemTriggered(userData, text); 
    });

    m_layout->addWidget(btn);
}

bool ModernScrollPanel::eventFilter(QObject *watched, QEvent *event)
{
    // 悬停进入某个菜单项 → 通知外部（展开/收起级联子菜单）。
    if (event->type() == QEvent::HoverEnter)
    {
        for (const MenuItem &item : m_items)
        {
            if (item.btn == watched)
            {
                const int relativeY = item.btn->mapTo(this, QPoint(0, 0)).y();
                emit itemHovered(item.userData, relativeY);
                break;
            }
        }
    }
    return QFrame::eventFilter(watched, event);
}

void ModernScrollPanel::finalizeLayout(int maxHeight, int maxWidth) {
    m_layout->addStretch();

    m_container->adjustSize();
    int contentHeight = m_container->sizeHint().height();

    if (contentHeight < 10) {
        contentHeight = 40;
    }

    m_container->setMinimumHeight(contentHeight);

    const int panelContentHeight = contentHeight + kScrollAreaFrameReserve;
    int finalHeight = qMin(panelContentHeight, maxHeight);
    bool needsVScroll = (panelContentHeight > maxHeight);

    if (needsVScroll) {
        m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    } else {
        m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

    
    
    
    
    int scrollBarBuffer = needsVScroll ? 12 : 0; 
    int requiredWidth = m_maxContentWidth + 45 + scrollBarBuffer;

    
    int minWidth = 130;

    
    int finalWidth = qBound(minWidth, requiredWidth, maxWidth);

    
    int textAvailableWidth = finalWidth - 45 - scrollBarBuffer;
    if (textAvailableWidth < 20) {
        textAvailableWidth = 20; 
    }

    
    for (const auto& item : m_items) {
        QFontMetrics fm(item.btn->font());
        if (item.hasSubmenu) {
            // 子菜单项：给右侧箭头预留宽度，主体省略后追加 "  ▶"。
            const QString arrow = QStringLiteral("  ▶");
            const int arrowWidth = fm.horizontalAdvance(arrow);
            item.btn->setText(
                fm.elidedText(item.fullText, Qt::ElideRight,
                              qMax(20, textAvailableWidth - arrowWidth)) +
                arrow);
        } else {
            QString elidedText = fm.elidedText(item.fullText, Qt::ElideRight, textAvailableWidth);
            item.btn->setText(elidedText);
        }
    }

    
    m_scrollArea->setFixedSize(finalWidth, finalHeight);
    this->setFixedSize(finalWidth, finalHeight);
}

void ModernScrollPanel::wheelEvent(QWheelEvent *event) {
    QFrame::wheelEvent(event);
    event->accept();
}





















void ModernScrollPanel::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    
    painter.setBrush(QColor(30, 30, 30, 242));

    
    QPen pen(QColor(255, 255, 255, 26));
    pen.setWidth(1);
    painter.setPen(pen);

    
    QRectF rf(rect().x() + 0.5, rect().y() + 0.5,
              rect().width() - 1.0, rect().height() - 1.0);
    painter.drawRoundedRect(rf, 6.0, 6.0);
}
