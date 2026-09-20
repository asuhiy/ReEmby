#ifndef SETTINGSVIEW_H
#define SETTINGSVIEW_H

#include "../baseview.h"
#include <QHideEvent>
#include <QLabel>
#include <QList>
#include <QListWidget>
#include <QPointer>
#include <QPropertyAnimation>
#include <QScrollArea>
#include <QVBoxLayout>

class SlidingStackedWidget;

class SettingsView : public BaseView {
    Q_OBJECT
public:
    explicit SettingsView(QEmbyCore* core, QWidget* parent = nullptr);
    ~SettingsView() override = default;

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

    /// 离开设置页时保存当前页签与滚动位置（下次进入时恢复）
    void hideEvent(QHideEvent* event) override;

private:
    void setupUi();
    void setupConnections();

    
    
    void ensurePageAt(int row);

    
    
    QScrollArea* wrapInScrollArea(QWidget* page, int row);

    /// 记住/恢复上次停留的页签与该页签的滚动位置
    void restoreNavigationState();
    void saveNavigationState();
    void applyPendingScroll();

private slots:
    
    void onThemeChanged();

private:
    QWidget* m_leftPanel;
    QLabel* m_titleLabel;
    QListWidget* m_navMenu;
    SlidingStackedWidget* m_stack;

    
    
    QList<QScrollArea*>        m_scrollAreas;
    QList<QPropertyAnimation*> m_scrollAnims;
    QList<int>                 m_scrollTargets;

    
    
    QList<QPointer<QWidget>>   m_pages;

    /// 恢复滚动位置时目标页可能刚被懒加载、布局尚未完成（maximum 仍为 0），需重试等待
    int m_pendingScrollRow     = -1;
    int m_pendingScrollPos     = 0;
    int m_pendingScrollRetries = 0;
};

#endif 
