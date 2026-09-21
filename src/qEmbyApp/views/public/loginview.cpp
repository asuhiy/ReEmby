#include "loginview.h"
#include "../../components/iconpickerdialog.h"
#include "../../components/libraryinfodialog.h"
#include "../../components/loadingoverlay.h"
#include "../../components/moderncombobox.h"
#include "../../components/modernmessagebox.h"
#include "../../components/modernswitch.h"
#include "../../components/proxysettingsdialog.h"
#include "../../components/webdavsyncdialog.h"
#include <config/webdavprofilestore.h>
#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <config/config_keys.h>
#include <config/configstore.h>
#include <models/profile/serverprofile.h>
#include <qembycore.h>
#include <services/auth/authservice.h>
#include <services/manager/servermanager.h>

namespace {
// 列表页栅格（与设计稿一致）：行高 68，最多 5 行可见。
// 行改成扁平样式后不再靠空距分隔，改用 1px 分隔线（见 server-list-sep）。
constexpr int kServerRowHeight = 68;
constexpr int kServerRowSepHeight = 1;
constexpr int kMaxVisibleServerRows = 5;
// 列表页内容宽度；页宽 = 内容 + 左右各 32 的留白。
constexpr int kServerListWidth = 536;
constexpr int kServerListPadding = 32;
constexpr int kServerListSpacing = 10;
// 列表面板的内边距：给滚动条和行留一点呼吸空间，别贴着面板边框。
constexpr int kServerPanelPadding = 6;
constexpr int kAddServerRowHeight = 52;
// 底部左侧「隐藏 / 显示」按钮的宽度（与「添加新服务器」虚线框同高并列）。
constexpr int kToggleUrlBtnWidth = 72;
// 「隐藏地址」时每行显示的圆点个数。**固定个数**而不是按原串长度：
// 长度随地址变化会让每行的占位参差不齐，看起来像没对齐。
constexpr int kMaskedUrlDots = 18;
// 「连接服务器」表单页的宽度：页面整体是 600 宽，但表单保持原来的窄宽度居中。
constexpr int kAddFormWidth = 360;
// 一行里被固定部件占掉的宽度：左边距 12 + 图标 46 + 图标后间距 16 +
// 末尾「⋯」按钮 28 + 右边距 12，再减去面板左右内边距与滚动条槽位。
// 剩下的宽度才留给服务器名 / 地址，用于提前做省略号（QLabel 不会自己 elide）。
constexpr int kServerRowChrome = 12 + 46 + 16 + 28 + 12;
// 预留：面板内边距(左右各 6) + 垂直滚动条槽位 ≈ 10，再留一点余量。
constexpr int kServerRowReserve = 12 + 10 + 16;
constexpr int kServerRowTextWidth =
    kServerListWidth - kServerRowChrome - kServerRowReserve;
}  // namespace

// 服务器行内点击：行里的子控件（标签）不接受鼠标事件，会冒泡到行；
// 行尾的「⋮」按钮会自行消费事件，所以点它不会触发登录。
class RowClickFilter : public QObject {
public:
  RowClickFilter(std::function<void()> onClick, QObject *parent)
      : QObject(parent), m_onClick(onClick) {}

protected:
  bool eventFilter(QObject *obj, QEvent *event) override {
    if (event->type() == QEvent::MouseButtonRelease) {
      auto *mouseEvent = static_cast<QMouseEvent *>(event);
      if (mouseEvent->button() == Qt::LeftButton) {
        m_onClick();
        return true;
      }
    }
    return QObject::eventFilter(obj, event);
  }

private:
  std::function<void()> m_onClick;
};

LoginView::LoginView(QEmbyCore *core, QWidget *parent)
    : QWidget(parent), m_core(core) {
  setupUi();

  
  connect(ThemeManager::instance(), &ThemeManager::themeChanged, this,
          &LoginView::onThemeChanged);
}

void LoginView::showEvent(QShowEvent *event) {
  QWidget::showEvent(event);
  
  if (m_loadingOverlay) {
    m_loadingOverlay->hide();
  }
  refreshServerList();

  
  if (!m_autoLoginAttempted && m_loadingOverlay != nullptr) {
    m_autoLoginAttempted = true; 

    qDebug() << "LoginView::showEvent: auto login checks starting";

    bool rememberServer =
        ConfigStore::instance()->get<bool>(ConfigKeys::RememberServer, false);
    QString lastServerId =
        ConfigStore::instance()->get<QString>(ConfigKeys::LastSelectedServerId);

    qDebug() << "LoginView::showEvent: rememberServer =" << rememberServer
             << ", lastServerId =" << lastServerId;

    if (rememberServer && !lastServerId.isEmpty()) {
      
      bool serverExists = false;
      auto servers = m_core->serverManager()->servers();
      qDebug() << "LoginView::showEvent: checking if server exists among"
               << servers.size() << "servers";

      for (const auto &s : servers) {
        if (s.id == lastServerId) {
          serverExists = true;
          break;
        }
      }

      qDebug() << "LoginView::showEvent: serverExists =" << serverExists;

      if (serverExists) {
        
        
        qDebug() << "LoginView::showEvent: setting up QTimer::singleShot(0)";
        QTimer::singleShot(0, this, [this, lastServerId]() {
          qDebug() << "LoginView::showEvent delayed: starting m_loadingOverlay";
          
          m_loadingOverlay->start();

          qDebug() << "LoginView::showEvent delayed: setting up "
                      "QTimer::singleShot(800)";
          
          QTimer::singleShot(800, this, [this, lastServerId]() {
            
            qDebug() << "LoginView::showEvent 800ms delayed: checking 'this' "
                        "pointer";
            if (this) {
              qDebug() << "LoginView::showEvent 800ms delayed: calling "
                          "onServerCardClicked";
              onServerCardClicked(lastServerId);
            } else {
              qDebug() << "LoginView::showEvent 800ms delayed: 'this' pointer "
                          "is null, aborting";
            }
          });
        });
      }
    }
  }
}

void LoginView::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  
  if (m_loadingOverlay) {
    m_loadingOverlay->resize(event->size());
  }
}


QString LoginView::getThemeSvgPath(const QString &iconName) const {
  return ThemeManager::instance()->isDarkMode() ? (":/svg/dark/" + iconName)
                                                : (":/svg/light/" + iconName);
}

void LoginView::onThemeChanged(ThemeManager::Theme theme) {
  Q_UNUSED(theme);

  
  if (m_togglePwdAction) {
    if (m_togglePwdAction->isChecked()) {
      m_togglePwdAction->setIcon(QIcon(getThemeSvgPath("eye-off.svg")));
    } else {
      m_togglePwdAction->setIcon(QIcon(getThemeSvgPath("eye.svg")));
    }
  }

  
  if (m_serverProxyBtn) {
    m_serverProxyBtn->setIcon(QIcon(getThemeSvgPath("proxy.svg")));
  }

  
  if (m_cloudSyncBtn) {
    m_cloudSyncBtn->setIcon(QIcon(getThemeSvgPath("cloud-sync.svg")));
  }

  
  
  if (m_pageSwitcher->currentWidget() == m_listPage) {
    // 行内的「⋮」图标要跟着主题换色；只重建行，不切页。
    rebuildServerRows();
  }
}

void LoginView::onCloudSyncClicked() {
  
  if (!m_webdavStore) {
    m_webdavStore = new WebdavProfileStore(this);
    m_webdavStore->load();
    qInfo() << "[LoginView] WebdavProfileStore loaded | hasProfile:"
            << m_webdavStore->hasProfile();
  }

  ServerManager *sm = m_core ? m_core->serverManager() : nullptr;
  WebdavSyncDialog dlg(m_webdavStore, sm, this);
  dlg.exec();

  
  refreshServerList();
}

void LoginView::updateSslOptionsVisibility() {
    const bool isHttps =
        m_protocolInput != nullptr &&
        m_protocolInput->currentText().compare("https://", Qt::CaseInsensitive) ==
            0;
  
    if (m_sslOptionsRow != nullptr) {
      m_sslOptionsRow->setVisible(isHttps);
    }
  
    if (!isHttps && m_ignoreSslSwitch != nullptr) {
      m_ignoreSslSwitch->setChecked(false);
    }
  }

void LoginView::syncProtocolSelectionFromUrlText(const QString &text) {
  if (m_protocolInput == nullptr) {
    return;
  }

  const QString trimmedText = text.trimmed();
  int targetIndex = -1;
  if (trimmedText.startsWith("https://", Qt::CaseInsensitive)) {
    targetIndex = 1;
  } else if (trimmedText.startsWith("http://", Qt::CaseInsensitive)) {
    targetIndex = 0;
  }

  if (targetIndex != -1 && m_protocolInput->currentIndex() != targetIndex) {
    const QSignalBlocker blocker(m_protocolInput);
    m_protocolInput->setCurrentIndex(targetIndex);
    updateSslOptionsVisibility();
  }
}

QUrl LoginView::buildNormalizedServerUrl(QString *errorMessage) const {
  const QString rawAddress = m_serverAddressInput->text().trimmed();
  const QString rawPort = m_portInput != nullptr ? m_portInput->text().trimmed()
                                                 : QString();
  QString candidate = rawAddress;

  int portFromField = -1;
  if (!rawPort.isEmpty()) {
    bool ok = false;
    const int parsedPort = rawPort.toInt(&ok);
    if (!ok || parsedPort < 1 || parsedPort > 65535) {
      if (errorMessage != nullptr) {
        *errorMessage = tr("Please enter a valid port number.");
      }
      qWarning() << "[LoginView] Invalid port input" << "| port:" << rawPort;
      return QUrl();
    }
    portFromField = parsedPort;
  }

  if (!candidate.startsWith("http://", Qt::CaseInsensitive) &&
      !candidate.startsWith("https://", Qt::CaseInsensitive)) {
    candidate.prepend(m_protocolInput->currentText());
  }

  QUrl normalizedUrl(candidate, QUrl::TolerantMode);
  if (!normalizedUrl.isValid() || normalizedUrl.host().isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = tr("Please enter a valid server address.");
    }
    return QUrl();
  }

  const QString scheme = normalizedUrl.scheme().toLower();
  if (scheme != "http" && scheme != "https") {
    if (errorMessage != nullptr) {
      *errorMessage = tr("Server URL must use http:// or https://.");
    }
    return QUrl();
  }

  if (!normalizedUrl.query().isEmpty() || !normalizedUrl.fragment().isEmpty()) {
    qWarning() << "[LoginView] Stripping query or fragment from server URL"
               << "| original:" << candidate;
  }
  if (!normalizedUrl.userName().isEmpty() ||
      !normalizedUrl.password().isEmpty()) {
    qWarning() << "[LoginView] Stripping embedded credentials from server URL"
               << "| original:" << candidate;
  }

  normalizedUrl.setUserName(QString());
  normalizedUrl.setPassword(QString());
  normalizedUrl.setQuery(QString());
  normalizedUrl.setFragment(QString());

  const int parsedPort = normalizedUrl.port(-1);
  if (portFromField != -1 && parsedPort != portFromField) {
    if (parsedPort != -1) {
      qDebug() << "[LoginView] Overriding URL port with port field"
               << "| parsedPort:" << parsedPort
               << "| inputPort:" << portFromField;
    }
    normalizedUrl.setPort(portFromField);
  }

  QString normalizedPath = normalizedUrl.path(QUrl::FullyDecoded);
  if (normalizedPath == "/") {
    normalizedPath.clear();
  }
  while (normalizedPath.size() > 1 && normalizedPath.endsWith('/')) {
    normalizedPath.chop(1);
  }
  normalizedUrl.setPath(normalizedPath);

  qDebug() << "[LoginView] Normalized server URL"
           << "| address:" << displayServerAddress(normalizedUrl)
           << "| port:" << normalizedUrl.port(-1)
           << "| finalUrl:" << normalizedUrl.toString(QUrl::FullyEncoded);

  return normalizedUrl;
}

QString LoginView::displayServerAddress(const QUrl &url) const {
  QString address = url.host(QUrl::FullyDecoded);
  if (address.contains(':') && !address.startsWith('[')) {
    address = QStringLiteral("[%1]").arg(address);
  }
  const QString path = url.path(QUrl::FullyDecoded);
  if (!path.isEmpty() && path != "/") {
    address += path;
  }
  return address;
}

std::optional<QUrl> LoginView::validateServerUrl(QString *errorMessage) const {
  if (m_serverAddressInput->text().trimmed().isEmpty() ||
      m_usernameInput->text().trimmed().isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = tr("Server address and username cannot be empty.");
    }
    return std::nullopt;
  }
  return buildNormalizedServerUrl(errorMessage);
}

void LoginView::applyServerUrlToForm(const QUrl &url,
                                     bool ignoreSslVerification) {
  if (m_protocolInput == nullptr || m_serverAddressInput == nullptr ||
      m_portInput == nullptr) {
    return;
  }

  {
    const QSignalBlocker blocker(m_protocolInput);
    m_protocolInput->setCurrentIndex(
        url.scheme().compare("https", Qt::CaseInsensitive) == 0 ? 1 : 0);
  }

  {
    const QSignalBlocker blocker(m_serverAddressInput);
    m_serverAddressInput->setText(displayServerAddress(url));
  }

  {
    const QSignalBlocker blocker(m_portInput);
    const int port = url.port(-1);
    m_portInput->setText(port > 0 ? QString::number(port) : QString());
  }

  if (m_ignoreSslSwitch != nullptr) {
    m_ignoreSslSwitch->setChecked(ignoreSslVerification);
  }
  updateSslOptionsVisibility();
}

void LoginView::setupUi() {
  // 「隐藏服务器地址」是跨会话记住的偏好：先读上次的选择，后面创建按钮、
  // 渲染每一行时都按它来（见 ConfigKeys::ServerListHideUrls）。
  m_hideServerUrls = ConfigStore::instance()->get<bool>(
      ConfigKeys::ServerListHideUrls, false);

  this->setProperty("showGlobalSearch", false);
  this->setProperty("viewTitle", QStringLiteral("ReEmby"));
  this->setProperty("showGlobalBack", false);
  this->setProperty("showGlobalHome", false);
  this->setProperty("showGlobalFav", false);
  auto *mainLayout = new QVBoxLayout(this);
  mainLayout->setContentsMargins(0, 0, 0, 8); 

  m_pageSwitcher = new QStackedWidget(this);
  m_pageSwitcher->setFixedWidth(kServerListWidth + kServerListPadding * 2);

  setupListPage();
  setupAddPage();

  m_pageSwitcher->addWidget(m_listPage);
  m_pageSwitcher->addWidget(m_addPage);

  mainLayout->addStretch();
  mainLayout->addWidget(m_pageSwitcher, 0, Qt::AlignHCenter);
  mainLayout->addStretch();

  
  QString orgName = qApp->organizationName();
  QString version = qApp->applicationVersion();
  if (version.isEmpty())
    version = "1.0.0";
  if (orgName.isEmpty())
    orgName = "asuhiy";

  
  auto *footerRow = new QWidget(this);
  footerRow->setObjectName("login-footer-row");
  auto *footerLayout = new QHBoxLayout(footerRow);
  footerLayout->setContentsMargins(12, 0, 12, 0);
  footerLayout->setSpacing(6);

  m_cloudSyncBtn = new QPushButton(this);
  m_cloudSyncBtn->setObjectName("login-cloud-sync-btn");
  m_cloudSyncBtn->setCursor(Qt::PointingHandCursor);
  m_cloudSyncBtn->setIcon(QIcon(getThemeSvgPath("cloud-sync.svg")));
  m_cloudSyncBtn->setIconSize(QSize(14, 14));
  m_cloudSyncBtn->setFixedSize(22, 22);
  m_cloudSyncBtn->setToolTip(tr("Cloud Sync (WebDAV)"));
  connect(m_cloudSyncBtn, &QPushButton::clicked, this,
          &LoginView::onCloudSyncClicked);

  auto *versionLabel =
      new QLabel(QString("%1 v%2").arg(orgName, version), this);
  versionLabel->setObjectName("app-version-label");

  
  auto *footerSpacer = new QWidget(this);
  footerSpacer->setFixedSize(22, 22);

  footerLayout->addWidget(m_cloudSyncBtn, 0, Qt::AlignVCenter);
  footerLayout->addStretch();
  footerLayout->addWidget(versionLabel, 0, Qt::AlignVCenter);
  footerLayout->addStretch();
  footerLayout->addWidget(footerSpacer, 0, Qt::AlignVCenter);
  mainLayout->addWidget(footerRow, 0, Qt::AlignHCenter);

  
  m_loadingOverlay = new LoadingOverlay(this);
}

void LoginView::setupListPage() {
  m_listPage = new QWidget(this);
  m_listPage->setObjectName("login-list-page");
  auto *layout = new QVBoxLayout(m_listPage);
  layout->setSpacing(0);
  layout->setContentsMargins(kServerListPadding, 8, kServerListPadding, 8);

  auto *titleLabel = new QLabel(tr("Select Server"), this);
  titleLabel->setObjectName("login-title");
  titleLabel->setAlignment(Qt::AlignCenter);
  layout->addWidget(titleLabel);
  layout->addSpacing(12);

  // 服务器逐行显示；超过 kMaxVisibleServerRows 行时由这里滚动。
  // 外面套一层「面板」：给列表一个可见边界，滚动条才有归属感
  // （同时面板内改用扁平行，避免面板边框 + 每行卡片边框叠成双层框）。
  m_serverPanel = new QWidget(this);
  m_serverPanel->setObjectName("server-list-panel");
  m_serverPanel->setAttribute(Qt::WA_StyledBackground, true);
  auto *panelLayout = new QVBoxLayout(m_serverPanel);
  panelLayout->setContentsMargins(kServerPanelPadding, kServerPanelPadding,
                                  kServerPanelPadding, kServerPanelPadding);
  panelLayout->setSpacing(0);

  m_serverScroll = new QScrollArea(m_serverPanel);
  m_serverScroll->setObjectName("server-list-scroll");
  m_serverScroll->setFrameShape(QFrame::NoFrame);
  m_serverScroll->setWidgetResizable(true);
  m_serverScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_serverScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  m_serverScroll->viewport()->setAutoFillBackground(false);
  // 全局 QScrollBar 样式只有 4px 宽 / 30% 不透明度，基本看不见。
  // 用一个专用 objectName 给它单独的样式（必须用 ID 直选器，见 QSS 里的说明）。
  m_serverScroll->verticalScrollBar()->setObjectName("serverPanelScrollBar");

  m_serverListContainer = new QWidget(m_serverScroll);
  m_serverListContainer->setObjectName("server-list-container");
  m_serverListLayout = new QVBoxLayout(m_serverListContainer);
  m_serverListLayout->setContentsMargins(0, 0, 0, 0);
  m_serverListLayout->setSpacing(0);
  m_serverScroll->setWidget(m_serverListContainer);

  panelLayout->addWidget(m_serverScroll);
  layout->addWidget(m_serverPanel);
  layout->addSpacing(kServerListSpacing);

  // 底部一排：左边「隐藏 / 显示」按钮（截图分享时把地址打码），右边
  // 「添加新服务器」。两者同高并列，虚线框相应变窄 —— 它们都在滚动区
  // 之外，所以服务器再多也始终可见。
  auto *footerRow = new QHBoxLayout();
  footerRow->setContentsMargins(0, 0, 0, 0);
  footerRow->setSpacing(8);

  m_toggleUrlBtn = new QPushButton(
      m_hideServerUrls ? tr("Show") : tr("Hide"), this);
  m_toggleUrlBtn->setObjectName("toggle-url-btn");
  m_toggleUrlBtn->setCursor(Qt::PointingHandCursor);
  m_toggleUrlBtn->setFixedSize(kToggleUrlBtnWidth, kAddServerRowHeight);
  connect(m_toggleUrlBtn, &QPushButton::clicked, this,
          &LoginView::toggleServerUrlVisibility);
  footerRow->addWidget(m_toggleUrlBtn);

  m_addServerBtn = new QPushButton(tr("Add New Server"), this);
  m_addServerBtn->setObjectName("add-server-btn");
  m_addServerBtn->setCursor(Qt::PointingHandCursor);
  m_addServerBtn->setFixedHeight(kAddServerRowHeight);
  connect(m_addServerBtn, &QPushButton::clicked, this, &LoginView::showAddPage);
  footerRow->addWidget(m_addServerBtn, 1);

  layout->addLayout(footerRow);

  layout->addStretch();
}

void LoginView::setupAddPage() {
    m_addPage = new QWidget(this);
    m_addPage->setObjectName("login-add-page");
    m_addPage->setAttribute(Qt::WA_StyledBackground, true);
    // 列表页现在是 600 宽（见 setupUi），表单页跟着变宽会把输入框拉得很长。
    // 这里外层只负责把表单居中，表单本身维持原来的宽度。
    // 垂直方向也要居中：原来外层是 QHBoxLayout，formContainer 会被拉满高度，
    // 而内部末尾那个 addStretch 又把 footer 推到底 => 中间空一大片、footer 贴底。
    auto *pageLayout = new QVBoxLayout(m_addPage);
    pageLayout->setContentsMargins(0, 16, 0, 16);
    auto *formContainer = new QWidget(m_addPage);
    formContainer->setObjectName("login-add-form");
    formContainer->setFixedWidth(kAddFormWidth);
    auto *layout = new QVBoxLayout(formContainer);
    layout->setSpacing(8);
    layout->setContentsMargins(8, 8, 8, 8);
    // 上下各留一份可伸缩空白 => 表单在页内垂直居中。
    // 空间不够时两份 stretch 会被压成 0，自然退化为顶部对齐，不会把内容挤没。
    pageLayout->addStretch();
    pageLayout->addWidget(formContainer, 0, Qt::AlignHCenter);
    pageLayout->addStretch();

  auto *titleLabel = new QLabel(tr("Connect to Server"), this);
  titleLabel->setObjectName("login-title");
  titleLabel->setAlignment(Qt::AlignCenter);
  layout->addWidget(titleLabel);

    m_errorLabel = new QLabel(this);
    m_errorLabel->setObjectName("login-error-label");
    m_errorLabel->setAlignment(Qt::AlignCenter);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->hide();
    layout->addWidget(m_errorLabel);
  
  const QString serverAddressTooltip = tr(
      "Server Address (e.g. example.com/emby or 192.168.1.1/jellyfin)");
  m_serverAddressInput = new QLineEdit(this);
  m_serverAddressInput->setPlaceholderText(tr("Server Address"));
  m_serverAddressInput->setToolTip(serverAddressTooltip);
  m_serverAddressInput->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  layout->addWidget(m_serverAddressInput);

    auto *protoPortLayout = new QHBoxLayout();
    protoPortLayout->setSpacing(10);

  m_protocolInput = new ModernComboBox(this);
  m_protocolInput->setObjectName("login-protocol-combo");
  m_protocolInput->addItems({"http://", "https://"});
  m_protocolInput->setCursor(Qt::PointingHandCursor);
  m_protocolInput->setFixedWidth(qMax(126, m_protocolInput->sizeHint().width()));

  m_portInput = new QLineEdit(this);
  m_portInput->setPlaceholderText(tr("Port"));
  m_portInput->setToolTip(tr("Port (e.g. 8096)"));
  m_portInput->setValidator(new QIntValidator(1, 65535, m_portInput));
  m_portInput->setMaxLength(5);
  m_portInput->setMinimumWidth(110);
  m_portInput->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  m_portInput->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

  
  
  
  
  m_serverProxyBtn = new QPushButton(this);
  m_serverProxyBtn->setObjectName("login-proxy-icon-btn");
  m_serverProxyBtn->setCursor(Qt::PointingHandCursor);
  m_serverProxyBtn->setIcon(QIcon(getThemeSvgPath("proxy.svg")));
  m_serverProxyBtn->setIconSize(QSize(18, 18));
  m_serverProxyBtn->setFixedWidth(36);
  m_serverProxyBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

  protoPortLayout->addWidget(m_protocolInput, 0);
  protoPortLayout->addWidget(m_portInput, 1);
  
  
  protoPortLayout->addWidget(m_serverProxyBtn, 0);
  layout->addLayout(protoPortLayout);

  connect(m_serverProxyBtn, &QPushButton::clicked, this,
          &LoginView::openProxyDialogForCurrentEntry);

  refreshServerProxyTooltip();

    m_sslOptionsRow = new QWidget(this);
    m_sslOptionsRow->setObjectName("login-ssl-card");
    m_sslOptionsRow->setAttribute(Qt::WA_StyledBackground, true);
    auto *sslOptionsLayout = new QHBoxLayout(m_sslOptionsRow);
    sslOptionsLayout->setContentsMargins(14, 8, 10, 8);
    sslOptionsLayout->setSpacing(10);
  
    const QString sslTooltip = tr(
        "Use this only for trusted self-signed or private CA HTTPS servers.");

  auto *sslTitleLabel =
      new QLabel(tr("Ignore SSL certificate verification"), m_sslOptionsRow);
  sslTitleLabel->setObjectName("login-ssl-title");
  sslTitleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  sslTitleLabel->setToolTip(sslTooltip);

  m_ignoreSslSwitch = new ModernSwitch(m_sslOptionsRow);
  m_ignoreSslSwitch->setToolTip(sslTooltip);

  sslOptionsLayout->addWidget(sslTitleLabel, 1);
  sslOptionsLayout->addWidget(m_ignoreSslSwitch, 0, Qt::AlignVCenter);
  m_sslOptionsRow->setToolTip(sslTooltip);
  layout->addWidget(m_sslOptionsRow);

  m_usernameInput = new QLineEdit(this);
  m_usernameInput->setPlaceholderText(tr("Username"));
  layout->addWidget(m_usernameInput);

  m_passwordInput = new QLineEdit(this);
  m_passwordInput->setPlaceholderText(tr("Password"));
  m_passwordInput->setEchoMode(QLineEdit::Password);

  
  m_togglePwdAction =
      new QAction(QIcon(getThemeSvgPath("eye.svg")), tr("Show Password"), this);
  m_togglePwdAction->setCheckable(true);
  m_passwordInput->addAction(m_togglePwdAction, QLineEdit::TrailingPosition);

  connect(m_togglePwdAction, &QAction::toggled, this, [this](bool checked) {
    if (checked) {
      m_passwordInput->setEchoMode(QLineEdit::Normal);
      m_togglePwdAction->setIcon(QIcon(getThemeSvgPath("eye-off.svg")));
    } else {
      m_passwordInput->setEchoMode(QLineEdit::Password);
      m_togglePwdAction->setIcon(QIcon(getThemeSvgPath("eye.svg")));
    }
  });

  layout->addWidget(m_passwordInput);

  // Custom User-Agent 输入框已隐藏：客户端身份固定为 ReEmby 原生
  // （ServerProfile::defaultUserAgent），不再提供伪装入口。控件仍构造并参与
  // 读取（值恒为空 = 无 per-server UA），将来如需恢复删除 setVisible(false)。
  m_userAgentInput = new QLineEdit(this);
  m_userAgentInput->setPlaceholderText(
      tr("Custom User-Agent (optional, for strict servers)"));
  m_userAgentInput->setToolTip(tr(
      "Present this User-Agent to this server for API and streaming "
      "requests. Useful when the server only allows specific players, "
      "e.g. \"ReEmby/0.11.0 (Windows NT 10.0.26100; x64)\""));
  m_userAgentInput->setVisible(false);
  connect(m_userAgentInput, &QLineEdit::returnPressed, this,
          &LoginView::onLoginClicked);
  layout->addWidget(m_userAgentInput);

  auto *btnLayout = new QHBoxLayout();
  btnLayout->setSpacing(8);

  auto *cancelBtn = new QPushButton(tr("Cancel"), this);
  cancelBtn->setCursor(Qt::PointingHandCursor);
  connect(cancelBtn, &QPushButton::clicked, this, &LoginView::showListPage);

  m_loginButton = new QPushButton(tr("Login"), this);
  m_loginButton->setObjectName("login-button");
  m_loginButton->setCursor(Qt::PointingHandCursor);
  connect(m_loginButton, &QPushButton::clicked, this,
          &LoginView::onLoginClicked);

  btnLayout->addWidget(cancelBtn);
  btnLayout->addWidget(m_loginButton);
  layout->addLayout(btnLayout);

  // 测试连接：不保存, 不切换会话, 只跑一遍 GET /System/Info/Public +
  // (有账号时) POST /Users/AuthenticateByName; 显示绿色 ✓ / 红色 ✗ 结果.
  m_testConnButton = new QPushButton(tr("Test Connection"), this);
  m_testConnButton->setObjectName("login-test-conn-btn");
  m_testConnButton->setCursor(Qt::PointingHandCursor);
  connect(m_testConnButton, &QPushButton::clicked, this,
          &LoginView::onTestConnectionClicked);
  layout->addWidget(m_testConnButton);

  m_testResultLabel = new QLabel(this);
  m_testResultLabel->setObjectName("login-test-result-label");
  m_testResultLabel->setAlignment(Qt::AlignCenter);
  m_testResultLabel->setWordWrap(true);
  m_testResultLabel->hide();
  layout->addWidget(m_testResultLabel);

  layout->addStretch();

  
  
  
  connect(m_protocolInput, &QComboBox::currentTextChanged, this,
          [this](const QString &) { updateSslOptionsVisibility(); });
  connect(m_serverAddressInput, &QLineEdit::textChanged, this,
          &LoginView::syncProtocolSelectionFromUrlText);
  connect(m_serverAddressInput, &QLineEdit::returnPressed, this,
          &LoginView::onLoginClicked);
  connect(m_portInput, &QLineEdit::returnPressed, this,
          &LoginView::onLoginClicked);
  connect(m_usernameInput, &QLineEdit::returnPressed, this,
          &LoginView::onLoginClicked);
  connect(m_passwordInput, &QLineEdit::returnPressed, this,
          &LoginView::onLoginClicked);

  updateSslOptionsVisibility();
}

void LoginView::refreshServerList(RowScrollIntent intent) {
  rebuildServerRows(intent);

  if (m_core->serverManager()->servers().isEmpty()) {
    // 全新安装 / 删光服务器：直接进表单页，少点一次。
    showAddPage();
    return;
  }

  m_editingServerId.clear();
  m_pageSwitcher->setCurrentWidget(m_listPage);
}

void LoginView::rebuildServerRows(RowScrollIntent intent) {
  if (!m_serverListLayout) {
    return;
  }

  // 重建会把内容清空，QScrollArea 的 maximum 随之变 0、滚动值被夹回顶部，
  // 所以必须在清空之前把当前位置记下来（上/下移要靠它算目标位置）。
  const int previousScroll =
      m_serverScroll ? m_serverScroll->verticalScrollBar()->value() : 0;

  //
  // 先清空旧行。用 hide() + deleteLater() 而不是直接 delete：
  // 本函数会在 QMenu 的嵌套事件循环里被调用（菜单项触发的重排），
  // 立即析构会让菜单所属的事件目标悬空。
  QLayoutItem *item = nullptr;
  while ((item = m_serverListLayout->takeAt(0)) != nullptr) {
    if (QWidget *w = item->widget()) {
      w->hide();
      w->deleteLater();
    }
    delete item;
  }

  const QList<ServerProfile> servers = m_core->serverManager()->servers();
  for (int i = 0; i < servers.size(); ++i) {
    m_serverListLayout->addWidget(createServerRow(servers[i]));
    // 行改名扁平行后不再靠空距分隔，改用 1px 分隔线（最后一行不加）。
    if (i + 1 < servers.size()) {
      auto *sep = new QFrame(m_serverListContainer);
      sep->setObjectName("server-list-sep");
      sep->setFrameShape(QFrame::NoFrame);
      sep->setFixedHeight(kServerRowSepHeight);
      m_serverListLayout->addWidget(sep);
    }
  }
  m_serverListLayout->addStretch();

  //
  // 高度按行数收缩：不超过 kMaxVisibleServerRows 行时不滚动；超过则封顶，
  // 多出来的行交给 QScrollArea 滚（滚动条本身即是"还能往下"的提示）。
  const int shown =
      qBound(0, static_cast<int>(servers.size()), kMaxVisibleServerRows);
  if (shown == 0) {
    m_serverPanel->hide();
  } else {
    m_serverScroll->setFixedHeight(shown * kServerRowHeight +
                                   (shown - 1) * kServerRowSepHeight);
    m_serverPanel->show();
  }

  // 强制先跑完一轮布局，让滚动区的内容高度 / range 尽快刷新到新值；
  // 下面延迟定位读到的 maximum 才是可信的。
  m_serverListLayout->activate();

  //
  // 滚动定位：必须等布局生效之后再设。此刻 scrollbar 的 range 还是清空阶段
  // 被夹成的旧值（maximum 可能仍是 0），立刻定位会失效 —— 这正是"上/下移
  // 一台服务器后视角跳回列表顶部"的原因。
  //
  // 行 widget 上都挂了 serverId 属性，所以这里不必再捕获整个 servers 列表。
  const QString lastId =
      ConfigStore::instance()->get<QString>(ConfigKeys::LastSelectedServerId);
  QPointer<LoginView> guard(this);
  QTimer::singleShot(0, this, [guard, intent, previousScroll, lastId]() {
    if (!guard || !guard->m_serverScroll) {
      return;
    }
    QScrollBar *bar = guard->m_serverScroll->verticalScrollBar();
    if (!bar) {
      return;
    }

    // 一行占用的总高度（行本体 + 它下面的 1px 分隔线）。
    constexpr int kRowStep = kServerRowHeight + kServerRowSepHeight;

    switch (intent) {
    case RowScrollIntent::Top:
      bar->setValue(0);
      return;
    case RowScrollIntent::Bottom:
      bar->setValue(bar->maximum());
      return;
    // 删除一行后用：停在原位置，只在新范围内夹取。被删的那行消失后
    // 下面的行自然补位，其余行不该跳。
    case RowScrollIntent::Preserve:
      bar->setValue(qBound(0, previousScroll, bar->maximum()));
      return;
    case RowScrollIntent::StepUp:
      bar->setValue(qBound(0, previousScroll - kRowStep, bar->maximum()));
      return;
    case RowScrollIntent::StepDown:
      bar->setValue(qBound(0, previousScroll + kRowStep, bar->maximum()));
      return;
    case RowScrollIntent::KeepSelected:
      break;
    }

    // 沿用旧行为：把上次使用的服务器滚进可见区域，免得服务器一多就找不到。
    QVBoxLayout *layout = guard->m_serverListLayout;
    if (lastId.isEmpty() || !layout) {
      return;
    }
    for (int i = 0; i < layout->count(); ++i) {
      QLayoutItem *rowItem = layout->itemAt(i);
      QWidget *row = rowItem ? rowItem->widget() : nullptr;
      if (row && row->property("serverId").toString() == lastId) {
        guard->m_serverScroll->ensureWidgetVisible(row, 0, kServerRowHeight);
        break;
      }
    }
  });
}

QWidget *LoginView::createServerRow(const ServerProfile &server) {
  auto *row = new QWidget(m_serverListContainer);
  row->setObjectName("server-list-row");
  // 挂上 id：重排后要按 id 找行做滚动定位（比按下标猜更可靠 ——
  // 重排当下 servers 的顺序已经变了，而重建是延迟布局的）。
  row->setProperty("serverId", server.id);
  row->setAttribute(Qt::WA_StyledBackground, true);
  // 普通 QWidget 默认收不到 hover 事件，QSS 的 :hover 就不会生效
  //（项目里 server-switcher-row 为此改用动态属性，这里直接开 WA_Hover 更省事）。
  row->setAttribute(Qt::WA_Hover, true);
  row->setFixedHeight(kServerRowHeight);
  row->setCursor(Qt::PointingHandCursor);

  auto *rowLayout = new QHBoxLayout(row);
  rowLayout->setContentsMargins(12, 0, 12, 0);
  rowLayout->setSpacing(0);

  auto *iconLabel = new QLabel(row);
  iconLabel->setFixedSize(46, 46);
  iconLabel->setScaledContents(true);
  iconLabel->setAlignment(Qt::AlignCenter);
  if (server.type == ServerProfile::Jellyfin) {
    iconLabel->setPixmap(QPixmap(":/svg/jellyfin.svg"));
  } else if (!server.iconBase64.isEmpty()) {
    QPixmap pix;
    pix.loadFromData(QByteArray::fromBase64(server.iconBase64.toUtf8()));
    iconLabel->setPixmap(pix);
  } else {
    iconLabel->setPixmap(QPixmap(":/svg/emby.svg"));
  }
  rowLayout->addWidget(iconLabel);
  rowLayout->addSpacing(16);

  auto *infoLayout = new QVBoxLayout();
  infoLayout->setSpacing(2);

  auto *nameLabel = new QLabel(row);
  nameLabel->setObjectName("server-name-label");
  QFont nameFont = nameLabel->font();
  nameFont.setPixelSize(15);
  nameFont.setBold(true);
  nameLabel->setFont(nameFont);
  // 水平方向忽略 sizeHint：长服务器名 / URL 不会把列表撑出横向滚动条。
  // 注意：Ignored 会让 QWidgetItem::sizeHint().width() 直接变成 0，所以这一行
  // 必须靠下面的 addLayout(infoLayout, 1) 拿到宽度 —— 不能用 addStretch()，
  // 否则信息区会被压成 0 宽、名字和地址整个不显示。
  nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  nameLabel->setText(QFontMetrics(nameFont).elidedText(
      server.name, Qt::ElideRight, kServerRowTextWidth));

  auto *urlLabel = new QLabel(row);
  urlLabel->setObjectName("server-url-label");
  QFont urlFont = urlLabel->font();
  urlFont.setPixelSize(12);
  urlLabel->setFont(urlFont);
  urlLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  // 地址用中间省略：保住 host 和结尾，比只裁尾巴可读。
  // 「隐藏地址」打开时换成固定个数的圆点占位 —— 用途是截图分享时不泄露
  // 自己的服务器地址（U+2022 实心圆点，不是星号：星号在字体里偏细偏上，
  // 打出来发虚）。
  const QString urlText = m_hideServerUrls
                              ? QString(kMaskedUrlDots, QChar(0x2022))
                              : server.url;
  urlLabel->setText(QFontMetrics(urlFont).elidedText(
      urlText, Qt::ElideMiddle, kServerRowTextWidth));

  infoLayout->addWidget(nameLabel);
  infoLayout->addWidget(urlLabel);
  rowLayout->addLayout(infoLayout, 1);

  // 行尾「⋮」：编辑 / 排序 / 删除都收进菜单，行本身只负责登录。
  auto *menuBtn = new QPushButton(row);
  menuBtn->setObjectName("server-menu-btn");
  menuBtn->setIcon(QIcon(getThemeSvgPath("more-line.svg")));
  menuBtn->setIconSize(QSize(16, 16));
  menuBtn->setFixedSize(28, 28);
  menuBtn->setCursor(Qt::PointingHandCursor);
  menuBtn->setToolTip(tr("More"));
  connect(menuBtn, &QPushButton::clicked, this,
          [this, id = server.id, menuBtn]() { showServerMenu(id, menuBtn); });
  rowLayout->addWidget(menuBtn);

  row->installEventFilter(new RowClickFilter(
      [this, id = server.id]() { onServerCardClicked(id); }, row));

  return row;
}

void LoginView::showServerMenu(const QString &serverId, QWidget *anchor) {
  const QList<ServerProfile> servers = m_core->serverManager()->servers();
  int index = -1;
  for (int i = 0; i < servers.size(); ++i) {
    if (servers[i].id == serverId) {
      index = i;
      break;
    }
  }
  if (index < 0 || !anchor) {
    return;
  }
  const int lastIndex = static_cast<int>(servers.size()) - 1;

  QMenu menu(this);

  QAction *editAction = menu.addAction(tr("Edit"));
  connect(editAction, &QAction::triggered, this,
          [this, serverId]() { onEditServerClicked(serverId); });

  QAction *iconAction = menu.addAction(tr("Change Icon"));
  connect(iconAction, &QAction::triggered, this, [this, serverId]() {
    // 此刻还在 QMenu::exec() 的嵌套事件循环里，直接再开一个模态对话框
    // 会让菜单来不及收起，所以推到下一个事件循环再开。
    QTimer::singleShot(0, this,
                       [this, serverId]() { onChangeIconRequested(serverId); });
  });

  QAction *libraryAction = menu.addAction(tr("Library Info"));
  connect(libraryAction, &QAction::triggered, this,
          [this, serverId]() { onLibraryInfoRequested(serverId); });

  menu.addSeparator();

  // 两端的排序项置灰，省掉"点了没反应"。
  // 重排后滚动条的落点跟动作对应：上/下移挪一行（被移动的服务器在视口里
  // 的位置保持不变），置顶滚到顶、置底拉到底，这样一眼能看到它去了哪。
  QAction *upAction = menu.addAction(tr("Move Up"));
  upAction->setEnabled(index > 0);
  connect(upAction, &QAction::triggered, this, [this, serverId, index]() {
    moveServerTo(serverId, index - 1, RowScrollIntent::StepUp);
  });

  QAction *downAction = menu.addAction(tr("Move Down"));
  downAction->setEnabled(index < lastIndex);
  connect(downAction, &QAction::triggered, this, [this, serverId, index]() {
    moveServerTo(serverId, index + 1, RowScrollIntent::StepDown);
  });

  QAction *topAction = menu.addAction(tr("Move to Top"));
  topAction->setEnabled(index > 0);
  connect(topAction, &QAction::triggered, this, [this, serverId]() {
    moveServerTo(serverId, 0, RowScrollIntent::Top);
  });

  QAction *bottomAction = menu.addAction(tr("Move to Bottom"));
  bottomAction->setEnabled(index < lastIndex);
  connect(bottomAction, &QAction::triggered, this, [this, serverId, lastIndex]() {
    moveServerTo(serverId, lastIndex, RowScrollIntent::Bottom);
  });

  menu.addSeparator();

  QAction *delAction = menu.addAction(tr("Delete"));
  connect(delAction, &QAction::triggered, this,
          [this, serverId]() { onRemoveServerClicked(serverId); });

  menu.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
}

void LoginView::toggleServerUrlVisibility() {
  m_hideServerUrls = !m_hideServerUrls;

  // 这是跨会话的偏好：记下来，下次启动沿用（用户要求"关掉重开也要记住"）。
  ConfigStore::instance()->set(ConfigKeys::ServerListHideUrls, m_hideServerUrls);

  if (m_toggleUrlBtn) {
    m_toggleUrlBtn->setText(m_hideServerUrls ? tr("Show") : tr("Hide"));
  }

  // 只换各行的地址文本，滚动位置保持不动 —— 否则点一下列表会跳回顶部。
  rebuildServerRows(RowScrollIntent::Preserve);
}

void LoginView::onChangeIconRequested(const QString &serverId) {
  IconPickerDialog dialog(this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }

  if (dialog.defaultIconRequested()) {
    // 「使用默认图标」= 清空自定义图标，列表会回落到内置的 emby / jellyfin 图。
    m_core->serverManager()->updateServerProfile(
        serverId, [](ServerProfile &profile) { profile.iconBase64.clear(); });
    rebuildServerRows(RowScrollIntent::Preserve);
    return;
  }

  const QByteArray iconData = dialog.selectedIconData();
  if (iconData.isEmpty()) {
    return;
  }

  // 列表页读的是 base64 文本（base64 -> QByteArray -> QPixmap），照旧存。
  const QString encoded = QString::fromLatin1(iconData.toBase64());
  m_core->serverManager()->updateServerProfile(
      serverId,
      [&encoded](ServerProfile &profile) { profile.iconBase64 = encoded; });

  // 让行里的图标立刻换掉，同时保持原来的滚动位置。
  rebuildServerRows(RowScrollIntent::Preserve);
}

void LoginView::onLibraryInfoRequested(const QString &serverId) {
  // 非模态：同一台已经开着就把它提到前面，不重复开窗。
  if (m_libraryInfoDialog && m_libraryInfoServerId == serverId) {
    m_libraryInfoDialog->show();
    m_libraryInfoDialog->raise();
    m_libraryInfoDialog->activateWindow();
    return;
  }
  // 换了一台：旧窗口里的数字属于上一台，必须换掉而不是复用。
  if (m_libraryInfoDialog) {
    m_libraryInfoDialog->close();
    m_libraryInfoDialog = nullptr;
  }

  auto *dialog = new LibraryInfoDialog(m_core, serverId, this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->show();
  dialog->raise();
  dialog->activateWindow();
  m_libraryInfoDialog = dialog;
  m_libraryInfoServerId = serverId;
}

void LoginView::moveServerTo(const QString &serverId, int newIndex,
                             RowScrollIntent intent) {
  m_core->serverManager()->moveServer(serverId, newIndex);
  rebuildServerRows(intent);
}

void LoginView::showAddPage() {
  m_editingServerId.clear();
  
  m_pendingProxy = ProxyConfig{};
  m_pendingUseGlobalProxy = false;
  refreshServerProxyTooltip();
  m_protocolInput->setCurrentIndex(0);
  m_serverAddressInput->clear();
  if (m_portInput) {
    m_portInput->clear();
  }
  m_usernameInput->clear();
  m_passwordInput->clear();
  if (m_userAgentInput) {
    m_userAgentInput->clear();
  }
  if (m_ignoreSslSwitch) {
    m_ignoreSslSwitch->setChecked(false);
  }
  updateSslOptionsVisibility();

  m_passwordInput->setEchoMode(QLineEdit::Password);

  
  if (m_togglePwdAction) {
    m_togglePwdAction->setChecked(false);
    m_togglePwdAction->setIcon(QIcon(getThemeSvgPath("eye.svg")));
  }

  m_loginButton->setText(tr("Login"));
  m_errorLabel->hide();
  if (m_testResultLabel) {
    m_testResultLabel->hide();
  }
  m_pageSwitcher->setCurrentWidget(m_addPage);

  
  
  m_serverAddressInput->setFocus();
}

void LoginView::showListPage() {
  // 列表为空时也允许停在列表页（页面里就有「添加新服务器」按钮）；
  // 早退会让新装 / 删光服务器后的「取消」变成一个点不动的死按钮。
  rebuildServerRows();
  m_pageSwitcher->setCurrentWidget(m_listPage);
}

void LoginView::onEditServerClicked(const QString &serverId) {
  QList<ServerProfile> servers = m_core->serverManager()->servers();
  for (const auto &server : servers) {
    if (server.id == serverId) {
      m_editingServerId = serverId;

      QUrl parsedUrl(server.url);
      if (parsedUrl.isValid() && !parsedUrl.scheme().isEmpty()) {
        applyServerUrlToForm(parsedUrl, server.ignoreSslVerification);
      } else {
        m_protocolInput->setCurrentIndex(
            server.url.startsWith("https://", Qt::CaseInsensitive) ? 1 : 0);
        m_serverAddressInput->setText(server.url);
        if (m_portInput) {
          m_portInput->clear();
        }
        if (m_ignoreSslSwitch) {
          m_ignoreSslSwitch->setChecked(server.ignoreSslVerification);
        }
        updateSslOptionsVisibility();
      }

      m_usernameInput->setText(server.userName);
      m_passwordInput->clear();
      if (m_userAgentInput) {
        m_userAgentInput->setText(server.customUserAgent);
      }

      m_passwordInput->setEchoMode(QLineEdit::Password);
      if (m_togglePwdAction) {
        m_togglePwdAction->setChecked(false);
        m_togglePwdAction->setIcon(QIcon(getThemeSvgPath("eye.svg")));
      }

      
      m_pendingProxy = server.proxy;
      m_pendingUseGlobalProxy = server.useGlobalProxy;
      refreshServerProxyTooltip();

      m_loginButton->setText(tr("Save & Login"));
      m_errorLabel->hide();
      if (m_testResultLabel) {
        m_testResultLabel->hide();
      }
      m_pageSwitcher->setCurrentWidget(m_addPage);

      
      m_passwordInput->setFocus();
      break;
    }
  }
}

void LoginView::onRemoveServerClicked(const QString &serverId) {
  
  
  bool confirm =
      ModernMessageBox::question(this, tr("Remove Server"),
                                 tr("Are you sure you want to remove this "
                                    "server?\nThis action cannot be undone."),
                                 tr("Remove"), 
                                 tr("Cancel"), 
                                 ModernMessageBox::Danger);
  if (!confirm) {
    return;
  }

  // 删掉的正是「上次使用的服务器」时，这条记忆也一并清掉：
  // ServerManager::removeServer 只清理 server/<id>/ 前缀下的键，而这个键
  // 不在其中，留着就成了指向已删除服务器的脏数据 —— 之后任何走
  // KeepSelected 的重建都会"找不到目标"而停在列表顶部。
  ConfigStore *config = ConfigStore::instance();
  if (config &&
      config->get<QString>(ConfigKeys::LastSelectedServerId) == serverId) {
    config->remove(ConfigKeys::LastSelectedServerId);
  }

  m_core->serverManager()->removeServer(serverId);
  // 保持删除前的滚动位置：被删的那行消失后下面的行补位，其余行不该跳。
  refreshServerList(RowScrollIntent::Preserve);
}


QCoro::Task<void> LoginView::onServerCardClicked(const QString &serverId) {
  
  
  
  QString safeServerId = serverId;

  
  QPointer<LoginView> guard(this);

  
  m_loadingOverlay->start();

  try {
    co_await m_core->authService()->validateSession(safeServerId);
    if (!guard)
      co_return;

    m_loadingOverlay->stop();
    m_loadingOverlay->hide();

    ConfigStore::instance()->set(ConfigKeys::LastSelectedServerId, safeServerId);
    Q_EMIT loginCompleted();
  } catch (const std::exception &e) {
    if (!guard)
      co_return;

    m_loadingOverlay->stop();
    refreshServerList();
    ModernMessageBox::warning(this, tr("Connection Failed"),
                              tr("Failed to connect or session expired. Please "
                                 "log in again.\nError: ") +
                                  QString::fromStdString(e.what()));
    onEditServerClicked(safeServerId);
  }
}


QCoro::Task<void> LoginView::onLoginClicked() {

  QPointer<LoginView> guard(this);

  QString urlError;
  auto maybeUrl = validateServerUrl(&urlError);
  if (!maybeUrl) {
    m_errorLabel->setText(urlError);
    m_errorLabel->show();
    co_return;
  }
  const QUrl normalizedUrl = *maybeUrl;

  const QString user = m_usernameInput->text().trimmed();
  const QString pass = m_passwordInput->text();
  const QString userAgent =
      m_userAgentInput ? m_userAgentInput->text().trimmed() : QString();

  const bool ignoreSslVerification =
      normalizedUrl.scheme().compare("https", Qt::CaseInsensitive) == 0 &&
      m_ignoreSslSwitch != nullptr && m_ignoreSslSwitch->isChecked();
  applyServerUrlToForm(normalizedUrl, ignoreSslVerification);

  const QString fullUrl = normalizedUrl.toString(QUrl::FullyEncoded);
  qDebug() << "[LoginView] Attempting login"
           << "| url:" << fullUrl
           << "| ignoreSslVerification:" << ignoreSslVerification;

  m_errorLabel->hide();
  m_loginButton->setEnabled(false);
  m_loginButton->setText(tr("Connecting..."));

  m_loadingOverlay->start();

  try {
    ServerProfile profile =
        co_await m_core->authService()->login(fullUrl, user, pass,
                                              ignoreSslVerification,
                                              userAgent);

    if (!guard)
      co_return;

    m_loadingOverlay->stop();
    m_loadingOverlay->hide(); 

    ConfigStore::instance()->set(ConfigKeys::LastSelectedServerId, profile.id);

    if (!m_editingServerId.isEmpty()) {
      m_core->serverManager()->removeServer(m_editingServerId);
      m_editingServerId.clear();
    }

    
    
    if (m_pendingProxy != ProxyConfig{} || m_pendingUseGlobalProxy) {
      m_core->serverManager()->updateServerProxy(
          profile.id, m_pendingProxy, m_pendingUseGlobalProxy);
      qInfo() << "[LoginView] applied draft proxy to new profile"
              << "| id:" << profile.id
              << "| useGlobal:" << m_pendingUseGlobalProxy
              << "| proxy:" << m_pendingProxy.summary();
    }
    
    m_pendingProxy = ProxyConfig{};
    m_pendingUseGlobalProxy = false;

    m_loginButton->setEnabled(true);
    m_loginButton->setText(tr("Login"));
    Q_EMIT loginCompleted();

  } catch (const std::exception &e) {
    if (!guard)
      co_return;

    m_loadingOverlay->stop();
    m_loginButton->setEnabled(true);
    m_loginButton->setText(m_editingServerId.isEmpty() ? tr("Login")
                                                       : tr("Save & Login"));
    m_errorLabel->setText(QString::fromStdString(e.what()));
    m_errorLabel->show();
  }
}

QCoro::Task<void> LoginView::onTestConnectionClicked() {
  QPointer<LoginView> guard(this);

  QString urlError;
  auto maybeUrl = validateServerUrl(&urlError);
  if (!maybeUrl) {
    m_testResultLabel->setText(tr("✗ %1").arg(urlError));
    m_testResultLabel->setProperty("state", "error");
    m_testResultLabel->style()->unpolish(m_testResultLabel);
    m_testResultLabel->style()->polish(m_testResultLabel);
    m_testResultLabel->show();
    co_return;
  }
  const QUrl normalizedUrl = *maybeUrl;

  const QString user = m_usernameInput->text().trimmed();
  const QString pass = m_passwordInput->text();
  const QString userAgent =
      m_userAgentInput ? m_userAgentInput->text().trimmed() : QString();
  const bool ignoreSsl =
      normalizedUrl.scheme().compare("https", Qt::CaseInsensitive) == 0 &&
      m_ignoreSslSwitch != nullptr && m_ignoreSslSwitch->isChecked();
  const QString fullUrl = normalizedUrl.toString(QUrl::FullyEncoded);

  m_testConnButton->setEnabled(false);
  m_testConnButton->setText(tr("Testing..."));
  m_testResultLabel->hide();

  try {
    const QString serverName = co_await m_core->authService()->testConnection(
        fullUrl, user, pass, ignoreSsl, userAgent);

    if (!guard)
      co_return;

    m_testResultLabel->setText(
        tr("✓ Connection succeeded (server: %1)").arg(serverName));
    m_testResultLabel->setProperty("state", "success");
  } catch (const std::exception &e) {
    if (!guard)
      co_return;

    // 剥除 AuthService 加的包装前缀, 显示真实错误更直观
    QString raw = QString::fromStdString(e.what());
    const QString prefix = QStringLiteral("Test connection failed: ");
    if (raw.startsWith(prefix)) {
      raw = raw.mid(prefix.size());
    }
    m_testResultLabel->setText(tr("✗ %1").arg(raw));
    m_testResultLabel->setProperty("state", "error");
  } catch (...) {
    if (!guard)
      co_return;
    m_testResultLabel->setText(tr("✗ Unknown error"));
    m_testResultLabel->setProperty("state", "error");
  }

  if (!guard)
    co_return;

  m_testResultLabel->style()->unpolish(m_testResultLabel);
  m_testResultLabel->style()->polish(m_testResultLabel);
  m_testResultLabel->show();

  m_testConnButton->setEnabled(true);
  m_testConnButton->setText(tr("Test Connection"));
}





void LoginView::refreshServerProxyTooltip() {
  if (!m_serverProxyBtn) {
    return;
  }
  
  
  QString state;
  if (m_pendingUseGlobalProxy) {
    state = tr("Using global proxy");
  } else {
    switch (m_pendingProxy.mode) {
    case ProxyConfig::None:
      state = tr("No proxy");
      break;
    case ProxyConfig::System:
      state = tr("System proxy");
      break;
    case ProxyConfig::Custom: {
      const QString typeLabel =
          (m_pendingProxy.type == ProxyConfig::Socks5)
              ? QStringLiteral("SOCKS5")
              : QStringLiteral("HTTP");
      if (m_pendingProxy.host.isEmpty() || m_pendingProxy.port == 0) {
        state = tr("Custom (incomplete)");
      } else {
        state = QStringLiteral("%1 %2:%3")
                    .arg(typeLabel, m_pendingProxy.host,
                         QString::number(m_pendingProxy.port));
      }
      break;
    }
    }
  }
  m_serverProxyBtn->setToolTip(
      tr("Server Proxy — %1").arg(state));
}

void LoginView::openProxyDialogForCurrentEntry() {
  
  
  if (!m_editingServerId.isEmpty()) {
    auto *dlg = ProxySettingsDialog::createForServer(
        m_core->serverManager(), m_editingServerId, this);
    if (dlg->exec() == QDialog::Accepted) {
      
      const auto servers = m_core->serverManager()->servers();
      for (const auto &s : servers) {
        if (s.id == m_editingServerId) {
          m_pendingProxy = s.proxy;
          m_pendingUseGlobalProxy = s.useGlobalProxy;
          break;
        }
      }
      refreshServerProxyTooltip();
    }
    dlg->deleteLater();
    return;
  }

  
  auto *dlg = ProxySettingsDialog::createForDraft(m_pendingProxy,
                                                  m_pendingUseGlobalProxy, this);
  if (dlg->exec() == QDialog::Accepted) {
    m_pendingProxy = dlg->resultConfig();
    m_pendingUseGlobalProxy = dlg->resultUseGlobal();
    qInfo() << "[LoginView] draft proxy updated"
            << "| useGlobal:" << m_pendingUseGlobalProxy
            << "| proxy:" << m_pendingProxy.summary();
    refreshServerProxyTooltip();
  }
  dlg->deleteLater();
}


