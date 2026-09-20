#include "moderntoast.h"
#include "../utils/textwraputils.h"
#include <QApplication>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QScreen>
#include <QPainter>
#include <QResizeEvent>
#include <QStyleOption>
#include <QEasingCurve>
#include <functional>

namespace {

constexpr int kToastFontPixelSize = 14;
constexpr int kToastHorizontalPadding = 18;
constexpr int kToastVerticalPadding = 10;
constexpr int kToastBottomMargin = 120;
constexpr int kToastMinimumWidth = 64;
constexpr int kToastMinimumTextWidth = 120;
constexpr int kToastAbsoluteMaxWidth = 560;
constexpr qreal kToastRelativeMaxWidth = 0.58;

// 阴影扩散留白。QGraphicsDropShadowEffect(blur=20, offset=(0,4)) 会把绘制
// 区域向四周扩张最多 20px（向下再多 4px）；而本窗口是 layered 顶层窗口
// （WA_TranslucentBackground → UpdateLayeredWindow），**对脏区零容忍**：
// 只要有一像素画到窗口边界外，整帧就被系统拒绝
// （qemby.log: UpdateLayeredWindowIndirect failed, dirty=(..., -20, -16)），
// 表现为 toast 显示残缺/闪烁。所以窗口必须比可见内容大出这段扩散距离，
// 26 = blur 20 + offset 4 + 2px 余量。
constexpr int kToastShadowPadding = 26;

// 内层实心表面：QSS 背景（#toastSurface）、阴影 effect、文字的唯一绘制者。
// 它比窗口小一圈（四边各 kToastShadowPadding），于是：
//   1. effect 生成阴影时扩张 20px，仍落在窗口内部 → 不越界；
//   2. 窗口自身完全透明、不绘制任何内容（isCrtFadingOut 阶段除外，见下）。
// CRT 关闭动画直接作用于本 widget 的 geometry（而不是窗口），收缩出的
// 线/点尺寸与实际可见内容一致，视觉与改造前逐像素一致。
class ToastSurface : public QWidget {
public:
    using PaintHandler = std::function<void(QPainter &)>;
    explicit ToastSurface(QWidget *parent) : QWidget(parent) {}
    PaintHandler paintHandler;

protected:
    void paintEvent(QPaintEvent *) override {
        if (!paintHandler) {
            return;
        }
        QPainter painter(this);
        paintHandler(painter);
    }
};

} 

QPointer<ModernToast> ModernToast::s_instance = nullptr;

QWidget* ModernToast::getMainWindow() {
    QWidget *bestWin = nullptr;
    int maxArea = 0;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (w->isVisible() && w->windowType() == Qt::Window) {
            int area = w->width() * w->height();
            if (area > maxArea) {
                maxArea = area;
                bestWin = w;
            }
        }
    }
    return bestWin;
}


void ModernToast::showMessage(const QString &msg, int durationMs) {
    if (!s_instance) {
        s_instance = new ModernToast(getMainWindow());
    }
    s_instance->showWithAnimation(msg, durationMs);
}

ModernToast::ModernToast(QWidget *parent)
    : QWidget(parent), m_textScale(1.0), m_comboCount(0), m_isCrtFadingOut(false)
{
    setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);

    
    setMinimumSize(0, 0);

    // 内层表面：背景/阴影/文字都在它上面（原因见 kToastShadowPadding 注释）。
    // QSS 选择器 #toastSurface 提供背景色、描边、圆角与文字色。
    auto *surface = new ToastSurface(this);
    surface->setObjectName(QStringLiteral("toastSurface"));
    surface->paintHandler = [this](QPainter &painter) {
        QStyleOption opt;
        opt.initFrom(m_surface);
        m_surface->style()->drawPrimitive(QStyle::PE_Widget, &opt, &painter, m_surface);

        // CRT 关闭阶段只保留实心背景（与原实现一致：先画背景，再决定是否画字）。
        if (m_isCrtFadingOut) {
            return;
        }

        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.setFont(m_surface->font());
        painter.setPen(opt.palette.color(QPalette::WindowText));

        const QPointF center = m_surface->rect().center();
        painter.translate(center);
        painter.scale(m_textScale, m_textScale);
        painter.translate(-center);

        painter.drawText(textRect(),
                         Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextDontClip,
                         m_wrappedMessage);
    };
    m_surface = surface;

    auto *shadow = new QGraphicsDropShadowEffect(m_surface);
    shadow->setBlurRadius(20);
    shadow->setColor(QColor(0, 0, 0, 160));
    shadow->setOffset(0, 4);
    m_surface->setGraphicsEffect(shadow);

    setWindowOpacity(0.0);

    
    m_showOpacity = new QPropertyAnimation(this, "windowOpacity", this);
    m_showOpacity->setDuration(200);
    m_showOpacity->setEasingCurve(QEasingCurve::OutQuad);

    m_showSlide = new QPropertyAnimation(this, "geometry", this);
    m_showSlide->setDuration(350);
    QEasingCurve slideCurve(QEasingCurve::OutBack);
    slideCurve.setOvershoot(1.2);
    m_showSlide->setEasingCurve(slideCurve);

    m_showGroup = new QParallelAnimationGroup(this);
    m_showGroup->addAnimation(m_showOpacity);
    m_showGroup->addAnimation(m_showSlide);

    
    
    
    // CRT 关闭动画作用于内层 surface（而不是窗口）：收缩出的线/点与实际
    // 可见内容尺寸一致，视觉与改造前逐像素一致。窗口本体保持原尺寸（透明，
    // 无视觉影响），因此 surface 的收缩不会破坏"阴影永不越界"的约束。
    m_crtSquashY = new QPropertyAnimation(m_surface, "geometry", this);
    m_crtSquashY->setDuration(160); 
    m_crtSquashY->setEasingCurve(QEasingCurve::InCubic); 

    m_crtSquashX = new QPropertyAnimation(m_surface, "geometry", this);
    m_crtSquashX->setDuration(120);
    m_crtSquashX->setEasingCurve(QEasingCurve::InExpo); 

    m_hideOpacity = new QPropertyAnimation(this, "windowOpacity", this);
    m_hideOpacity->setDuration(120);
    m_hideOpacity->setEasingCurve(QEasingCurve::InCubic);

    m_crtPhase2Group = new QParallelAnimationGroup(this);
    m_crtPhase2Group->addAnimation(m_crtSquashX);
    m_crtPhase2Group->addAnimation(m_hideOpacity);

    m_hideGroup = new QSequentialAnimationGroup(this);
    m_hideGroup->addAnimation(m_crtSquashY);
    m_hideGroup->addAnimation(m_crtPhase2Group);

    connect(m_hideGroup, &QSequentialAnimationGroup::finished, this, [this]() {
        hide();
        m_comboCount = 0;
        m_isCrtFadingOut = false; 

        // 阴影 effect 挂在 surface 上（见 kToastShadowPadding 注释）。
        if (m_surface && m_surface->graphicsEffect()) {
            m_surface->graphicsEffect()->setEnabled(true);
        }
    });

    m_stayTimer = new QTimer(this);
    m_stayTimer->setSingleShot(true);
    connect(m_stayTimer, &QTimer::timeout, this, [this]() {
        m_isCrtFadingOut = true;
        if (m_surface) {
            m_surface->update(); 
        }

        // CRT 收缩期间不需要阴影（会拖尾），临时禁用 surface 的 effect。
        if (m_surface && m_surface->graphicsEffect()) {
            m_surface->graphicsEffect()->setEnabled(false);
        }

        // 收缩的几何基准 = 内层 surface（可见背景）的几何，与改造前窗口
        // 几何等价 —— 线/点尺寸与实际可见内容一致。
        QRect startGeo = m_surface ? m_surface->geometry() : geometry();
        
        QRect lineGeo(startGeo.x(), startGeo.y() + startGeo.height() / 2 - 1, startGeo.width(), 2);
        
        QRect dotGeo(startGeo.x() + startGeo.width() / 2 - 1, startGeo.y() + startGeo.height() / 2 - 1, 2, 2);

        m_crtSquashY->setStartValue(startGeo);
        m_crtSquashY->setEndValue(lineGeo);

        m_crtSquashX->setStartValue(lineGeo);
        m_crtSquashX->setEndValue(dotGeo);

        m_hideOpacity->setStartValue(windowOpacity());
        m_hideOpacity->setEndValue(0.0);

        m_hideGroup->start();
    });

    
    QEasingCurve expandCurve(QEasingCurve::OutBack);
    expandCurve.setOvershoot(2.5);

    m_shellExpand = new QPropertyAnimation(this, "geometry", this);
    m_shellExpand->setDuration(160);
    m_shellExpand->setEasingCurve(expandCurve);

    m_textExpand = new QPropertyAnimation(this, "textScale", this);
    m_textExpand->setDuration(160);
    m_textExpand->setEasingCurve(expandCurve);

    m_expandGroup = new QParallelAnimationGroup(this);
    m_expandGroup->addAnimation(m_shellExpand);
    m_expandGroup->addAnimation(m_textExpand);

    QEasingCurve shrinkCurve(QEasingCurve::OutCubic);

    m_shellShrink = new QPropertyAnimation(this, "geometry", this);
    m_shellShrink->setDuration(220);
    m_shellShrink->setEasingCurve(shrinkCurve);

    m_textShrink = new QPropertyAnimation(this, "textScale", this);
    m_textShrink->setDuration(220);
    m_textShrink->setEasingCurve(shrinkCurve);

    m_shrinkGroup = new QParallelAnimationGroup(this);
    m_shrinkGroup->addAnimation(m_shellShrink);
    m_shrinkGroup->addAnimation(m_textShrink);

    m_popSequentialGroup = new QSequentialAnimationGroup(this);
    m_popSequentialGroup->addAnimation(m_expandGroup);
    m_popSequentialGroup->addAnimation(m_shrinkGroup);
}

ModernToast::~ModernToast() {}

void ModernToast::setTextScale(qreal scale) {
    m_textScale = scale;
    // 文字绘制在内层 surface 上。
    if (m_surface) {
        m_surface->update();
    }
}

QRect ModernToast::resolveAnchorRect() const
{
    QWidget *mainWin = parentWidget();
    if (!mainWin) {
        mainWin = getMainWindow();
    }

    if (mainWin) {
        return mainWin->geometry();
    }

    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        return screen->geometry();
    }

    return QRect(0, 0, 1280, 720);
}

QRect ModernToast::textRect() const
{
    // 文字区 = 内层 surface（实际可见背景面）再内缩一圈内容 padding。窗口
    // 额外留出了阴影扩散区（kToastShadowPadding），不能直接基于窗口 rect。
    const QRect base = m_surface ? m_surface->rect() : rect();
    return base.adjusted(kToastHorizontalPadding, kToastVerticalPadding,
                         -kToastHorizontalPadding, -kToastVerticalPadding);
}

QSize ModernToast::wrappedTextSize(const QFont& font, int maxTextWidth) const
{
    return TextWrapUtils::measureWrappedPlainText(m_message, font,
                                                  maxTextWidth);
}

void ModernToast::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // 窗口只做几何/动画宿主，内容全部画在内层 surface 上；窗口尺寸变化时
    // 同步 surface（内缩 kToastShadowPadding），保证阴影扩散区始终在窗口内。
    syncSurfaceGeometry();
}

void ModernToast::syncSurfaceGeometry()
{
    if (!m_surface) {
        return;
    }
    m_surface->setGeometry(rect().adjusted(kToastShadowPadding, kToastShadowPadding,
                                           -kToastShadowPadding, -kToastShadowPadding));
}


void ModernToast::showWithAnimation(const QString &msg, int durationMs) {
    m_message = msg;

    QFont currentFont = this->font();
    currentFont.setPixelSize(kToastFontPixelSize);
    this->setFont(currentFont);

    const QRect anchorRect = resolveAnchorRect();
    const int availableWidth = qMax(kToastMinimumWidth,
                                    anchorRect.width() - 48);
    const int preferredMaxWidth =
        qRound(anchorRect.width() * kToastRelativeMaxWidth);
    const int maxToastWidth =
        qMin(availableWidth,
             qMax(220, qMin(preferredMaxWidth, kToastAbsoluteMaxWidth)));
    const int maxTextWidth =
        qMax(kToastMinimumTextWidth,
             maxToastWidth - kToastHorizontalPadding * 2);

    m_wrappedMessage =
        TextWrapUtils::wrapPlainText(msg, currentFont, maxTextWidth);
    const QSize wrappedSize = wrappedTextSize(currentFont, maxTextWidth);

    const int targetW = qMax(
        kToastMinimumWidth,
        qMin(maxToastWidth, wrappedSize.width() + kToastHorizontalPadding * 2));
    const int targetH =
        qMax(currentFont.pixelSize() + kToastVerticalPadding * 2,
             wrappedSize.height() + kToastVerticalPadding * 2);

    // 窗口几何 = 可见背景尺寸 + 两侧阴影扩散留白（kToastShadowPadding）。
    // 对位规则与改造前完全一致：水平居中于 anchor；背景底边距 anchor 底
    // kToastBottomMargin（背景区 = 窗口内缩 padding 后的区域）。
    const int winW = targetW + kToastShadowPadding * 2;
    const int winH = targetH + kToastShadowPadding * 2;
    const int px = anchorRect.x() + (anchorRect.width() - winW) / 2;
    const int py =
        anchorRect.y() + anchorRect.height() - targetH - kToastBottomMargin - kToastShadowPadding;
    m_baseGeometry = QRect(px, py, winW, winH);

    show();

    // 恢复/归位内层 surface：上次 CRT 关闭动画会把 surface 缩成线/点并留在那个
    // 尺寸上；若本次窗口尺寸与上次相同，resizeEvent 不会触发，必须手动同步。
    syncSurfaceGeometry();

    bool isFullyHidden = (windowOpacity() == 0.0 && m_showGroup->state() != QAbstractAnimation::Running);
    bool isFadingOut = (m_hideGroup->state() == QAbstractAnimation::Running);

    if (isFullyHidden || isFadingOut) {
        m_comboCount = 0;

        m_hideGroup->stop();
        m_isCrtFadingOut = false;

        // 阴影 effect 挂在 surface 上（见 kToastShadowPadding 注释）。
        if (m_surface && m_surface->graphicsEffect()) {
            m_surface->graphicsEffect()->setEnabled(true);
        }

        m_popSequentialGroup->stop();
        setTextScale(1.0);

        QRect slideStartGeo = m_baseGeometry.translated(0, 20);
        setGeometry(slideStartGeo);

        m_showSlide->setStartValue(slideStartGeo);
        m_showSlide->setEndValue(m_baseGeometry);

        m_showOpacity->setStartValue(windowOpacity());
        m_showOpacity->setEndValue(1.0);

        m_showGroup->start();
    } else {
        m_comboCount++;

        qreal dynamicScale = 1.15 + qMin(m_comboCount * 0.05, 0.3);

        int dx = qRound(targetW * (dynamicScale - 1.0) / 2.0) + 2;
        int dy = qRound(targetH * (dynamicScale - 1.0) / 2.0) + 2;

        m_showGroup->stop();
        setWindowOpacity(1.0);

        m_popSequentialGroup->stop();
        setTextScale(1.0);
        setGeometry(m_baseGeometry);

        QRect expandedRect = m_baseGeometry.adjusted(-dx, -dy, dx, dy);

        m_shellExpand->setStartValue(m_baseGeometry);
        m_shellExpand->setEndValue(expandedRect);

        m_textExpand->setStartValue(m_textScale);
        m_textExpand->setEndValue(dynamicScale);

        m_shellShrink->setStartValue(expandedRect);
        m_shellShrink->setEndValue(m_baseGeometry);

        m_textShrink->setStartValue(dynamicScale);
        m_textShrink->setEndValue(1.0);

        m_popSequentialGroup->start();
    }

    
    m_stayTimer->start(durationMs);
}
