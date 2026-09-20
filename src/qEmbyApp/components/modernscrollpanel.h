#ifndef MODERNSCROLLPANEL_H
#define MODERNSCROLLPANEL_H

#include <QFrame>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QPushButton>
#include <QVariant>
#include <QString>
#include <QList>
#include <QWheelEvent>
#include <QPaintEvent>

class ModernScrollPanel : public QFrame {
    Q_OBJECT
public:
    explicit ModernScrollPanel(QWidget *parent = nullptr);
    ~ModernScrollPanel() override = default;

    // hasSubmenu = true 时该项右侧显示 "▶"（悬停/点击后由外部弹出级联子菜单）。
    void addItem(const QString &text, const QVariant &userData, bool isSelected = false,
                 bool hasSubmenu = false);

    
    void finalizeLayout(int maxHeight, int maxWidth = 250);

signals:
    void itemTriggered(const QVariant &userData, const QString &text);
    // 鼠标悬停到某个菜单项（含无子菜单的项）：外部据此展开/收起级联子菜单。
    // relativeY 为该按钮在面板内的 y 坐标，用于把子菜单对齐到悬停项。
    void itemHovered(const QVariant &userData, int relativeY);

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    
    struct MenuItem {
        QPushButton* btn;
        QString fullText;
        QVariant userData;
        bool hasSubmenu = false;
    };

    QVBoxLayout *m_mainLayout;
    QScrollArea *m_scrollArea;
    QWidget *m_container;
    QVBoxLayout *m_layout;

    QList<MenuItem> m_items;
    int m_maxContentWidth; 
};

#endif 
