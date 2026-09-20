#ifndef MODERNTOAST_H
#define MODERNTOAST_H

#include <QWidget>
#include <QTimer>
#include <QPointer>
#include <QPropertyAnimation>
#include <QSequentialAnimationGroup>
#include <QParallelAnimationGroup>

class QFont;

class ModernToast : public QWidget {
    Q_OBJECT
    Q_PROPERTY(qreal textScale READ textScale WRITE setTextScale)

public:
    
    static void showMessage(const QString &msg, int durationMs = 2500);

protected:
    explicit ModernToast(QWidget *parent = nullptr);
    ~ModernToast() override;

    void resizeEvent(QResizeEvent *event) override;

private:
    
    void showWithAnimation(const QString &msg, int durationMs);
    static QWidget* getMainWindow();
    QRect resolveAnchorRect() const;
    QRect textRect() const;
    QSize wrappedTextSize(const QFont& font, int maxTextWidth) const;
    // 把内层 surface 的几何同步为窗口 rect 内缩 kToastShadowPadding 的区域
    // （阴影扩散留白；窗口自身不绘制任何内容）。
    void syncSurfaceGeometry();

    qreal textScale() const { return m_textScale; }
    void setTextScale(qreal scale);

    static QPointer<ModernToast> s_instance;

    // 内层实心表面：QSS 背景（#toastSurface）、drop-shadow effect、文字都画在
    // 它上面。它比窗口小一圈（四边各留阴影扩散所需的 kToastShadowPadding），
    // 因此阴影不会画到窗口边界外（layered 窗口脏区越界会导致整帧被系统拒绝）。
    QWidget *m_surface = nullptr;

    QString m_message;
    QString m_wrappedMessage;
    qreal m_textScale;

    int m_comboCount;
    bool m_isCrtFadingOut; 

    QTimer *m_stayTimer;

    
    QParallelAnimationGroup *m_showGroup;
    QPropertyAnimation *m_showOpacity;
    QPropertyAnimation *m_showSlide;

    
    QSequentialAnimationGroup *m_popSequentialGroup;
    QParallelAnimationGroup *m_expandGroup;
    QParallelAnimationGroup *m_shrinkGroup;
    QPropertyAnimation *m_shellExpand;
    QPropertyAnimation *m_shellShrink;
    QPropertyAnimation *m_textExpand;
    QPropertyAnimation *m_textShrink;

    
    QSequentialAnimationGroup *m_hideGroup;
    QPropertyAnimation *m_crtSquashY;
    QParallelAnimationGroup *m_crtPhase2Group;
    QPropertyAnimation *m_crtSquashX;
    QPropertyAnimation *m_hideOpacity;

    QRect m_baseGeometry;
};

#endif 
