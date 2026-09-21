#ifndef LOGINVIEW_H
#define LOGINVIEW_H

#include <QWidget>
#include <optional>
#include <qcorotask.h>
#include "../../managers/thememanager.h"
#include "models/profile/proxyconfig.h"

class QLineEdit;
class QPushButton;
class QLabel;
class QStackedWidget;
class QScrollArea;
class QVBoxLayout;
class QEmbyCore;
class QResizeEvent;
class QAction; 
class QUrl;

struct ServerProfile;


class LoadingOverlay;
class ModernComboBox;
class ModernSwitch;
class WebdavProfileStore;

class LoginView : public QWidget
{
    Q_OBJECT
public:
    explicit LoginView(QEmbyCore* core, QWidget *parent = nullptr);

Q_SIGNALS:
    void loginCompleted();

protected:
    void showEvent(QShowEvent *event) override;
    
    void resizeEvent(QResizeEvent *event) override;

private Q_SLOTS:

    QCoro::Task<void> onLoginClicked();
    QCoro::Task<void> onTestConnectionClicked();
    QCoro::Task<void> onServerCardClicked(const QString& serverId);

    void showAddPage();
    void showListPage();
    void onRemoveServerClicked(const QString& serverId);
    void onEditServerClicked(const QString& serverId);

    
    void onCloudSyncClicked();

    
    void onThemeChanged(ThemeManager::Theme theme);

private:
    void updateSslOptionsVisibility();
    void syncProtocolSelectionFromUrlText(const QString& text);
    QUrl buildNormalizedServerUrl(QString* errorMessage) const;
    void applyServerUrlToForm(const QUrl& url, bool ignoreSslVerification);
    QString displayServerAddress(const QUrl& url) const;
    // 共享校验: 地址非空 + 用户名非空 + 解析 normalized URL.
    // 用于 onLoginClicked 和 onTestConnectionClicked 复用, 失败填 errorMessage.
    std::optional<QUrl> validateServerUrl(QString* errorMessage) const;

    QEmbyCore* m_core;

    QStackedWidget* m_pageSwitcher;

    QWidget* m_listPage;
    QWidget* m_addPage;

    // 服务器列表页：面板（有边框的框）内放 QScrollArea，
    // 逐行显示服务器；底部固定「添加新服务器」行。
    QWidget* m_serverPanel = nullptr;
    QScrollArea* m_serverScroll = nullptr;
    QWidget* m_serverListContainer = nullptr;
    QVBoxLayout* m_serverListLayout = nullptr;
    QPushButton* m_addServerBtn = nullptr;

    ModernComboBox* m_protocolInput;
    QLineEdit* m_serverAddressInput;
    QLineEdit* m_portInput = nullptr;
    QWidget* m_sslOptionsRow = nullptr;
    ModernSwitch* m_ignoreSslSwitch = nullptr;

    QLineEdit* m_usernameInput;
    QLineEdit* m_passwordInput;
    QLineEdit* m_userAgentInput = nullptr;
    QPushButton* m_loginButton;
    QPushButton* m_testConnButton;
    QLabel* m_testResultLabel;
    QLabel* m_errorLabel;
    
    
    QAction* m_togglePwdAction = nullptr;

    QString m_editingServerId;
    
    
    LoadingOverlay* m_loadingOverlay = nullptr;

    bool m_autoLoginAttempted = false;

    
    
    QPushButton* m_serverProxyBtn = nullptr;

    
    QPushButton* m_cloudSyncBtn = nullptr;

    
    WebdavProfileStore* m_webdavStore = nullptr;

    
    
    ProxyConfig m_pendingProxy;
    bool        m_pendingUseGlobalProxy = false;

    void setupUi();
    void setupListPage();
    void setupAddPage();

    // 列表重排后滚动条该停在哪。
    // 重建行会把内容清空、滚动位置被夹回顶部，所以重排后必须显式定位，
    // 否则用户上/下移一台服务器后视角会跳回列表开头。
    enum class RowScrollIntent {
        KeepSelected,  // 把上次使用的服务器滚进可见区（默认）
        Preserve,      // 保持删除前的滚动位置（删掉一行后其余行不该跳）
        Top,           // 置顶 -> 滚到顶部
        Bottom,        // 置底 -> 拉到最底部
        StepUp,        // 上移一格 -> 滚动条跟一行，被移动项在视口里的位置不变
        StepDown,      // 下移一格 -> 同上
    };

    void refreshServerList(
        RowScrollIntent intent = RowScrollIntent::KeepSelected);
    // 清空并重建列表页里的服务器行（不含底部「添加新服务器」行），
    // 同时按行数调整滚动区高度：<= 5 行不滚动，> 5 行封顶并出现滚动条。
    void rebuildServerRows(
        RowScrollIntent intent = RowScrollIntent::KeepSelected);
    QWidget* createServerRow(const ServerProfile& server);
    // 弹出行尾「⋮」菜单（编辑 / 上移 / 下移 / 置顶 / 置底 / 删除）。
    void showServerMenu(const QString& serverId, QWidget* anchor);
    // 把服务器移动到指定下标（越界由 ServerManager 夹取），
    // 并按 intent 决定重排后滚动条的落点。
    void moveServerTo(const QString& serverId, int newIndex,
                      RowScrollIntent intent = RowScrollIntent::KeepSelected);
    void refreshServerProxyTooltip();
    void openProxyDialogForCurrentEntry();
    
    
    QString getThemeSvgPath(const QString& iconName) const;
};

#endif 
