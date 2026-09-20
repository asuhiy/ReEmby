#include "settingsview.h"
#include "../../components/slidingstackedwidget.h"
#include "../../managers/thememanager.h" 
#include "pageabout.h"
#include "pageaccount.h"
#include "pageappearance.h"
#include "pagebilibili.h"
#include "pagegeneral.h"
#include "pageplayer.h"
#include "pagelibrary.h"
#include "pagetrakt.h"
#include "config/config_keys.h"
#include "config/configstore.h"
#include <QApplication>
#include <QTimer>
#include <QEvent>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWheelEvent>


SettingsView::SettingsView(QEmbyCore *core, QWidget *parent)
    : BaseView(core, parent) {
  
  setAttribute(Qt::WA_StyledBackground, true);
  setObjectName("SettingsRootView");

  setupUi();
  setupConnections();

  
  
  // 恢复上次离开设置页时停留的页签与滚动位置（无记录时落在第 0 页）
  restoreNavigationState();
}

void SettingsView::setupUi() {
  auto *mainLayout = new QHBoxLayout(this);
  
  mainLayout->setContentsMargins(0, 0, 0, 0);
  mainLayout->setSpacing(0);

  
  m_leftPanel = new QWidget(this);
  m_leftPanel->setFixedWidth(260);
  m_leftPanel->setObjectName("SettingsLeftPanel");
  auto *leftLayout = new QVBoxLayout(m_leftPanel);
  
  leftLayout->setContentsMargins(32, 32, 32, 32);
  leftLayout->setSpacing(24);

  
  m_titleLabel = new QLabel(tr("Settings"), m_leftPanel);
  m_titleLabel->setObjectName("SettingsMainTitle");

  
  m_navMenu = new QListWidget(m_leftPanel);
  m_navMenu->setObjectName("SettingsNavMenu");
  m_navMenu->setFocusPolicy(Qt::NoFocus);
  
  m_navMenu->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_navMenu->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

  
  auto *itemGeneral = new QListWidgetItem(
      ThemeManager::getAdaptiveIcon(":/svg/dark/general.svg"), tr(" General"));
  itemGeneral->setData(Qt::UserRole, ":/svg/dark/general.svg");

  auto *itemAppearance = new QListWidgetItem(
      ThemeManager::getAdaptiveIcon(":/svg/dark/appearance.svg"),
      tr(" Appearance"));
  itemAppearance->setData(Qt::UserRole, ":/svg/dark/appearance.svg");

  auto *itemAccount = new QListWidgetItem(
      ThemeManager::getAdaptiveIcon(":/svg/dark/user.svg"), tr(" Account"));
  itemAccount->setData(Qt::UserRole, ":/svg/dark/user.svg");

  auto *itemPlayer = new QListWidgetItem(
      ThemeManager::getAdaptiveIcon(":/svg/dark/player.svg"), tr(" Player"));
  itemPlayer->setData(Qt::UserRole, ":/svg/dark/player.svg");

  auto *itemLibrary = new QListWidgetItem(
      ThemeManager::getAdaptiveIcon(":/svg/dark/library.svg"), tr(" Library"));
  itemLibrary->setData(Qt::UserRole, ":/svg/dark/library.svg");

  auto *itemTrakt = new QListWidgetItem(
      ThemeManager::getAdaptiveIcon(":/svg/dark/user.svg"), tr(" Trakt"));
  itemTrakt->setData(Qt::UserRole, ":/svg/dark/user.svg");

  auto *itemBilibili = new QListWidgetItem(
      ThemeManager::getAdaptiveIcon(":/svg/dark/user.svg"), tr(" BiliBili"));
  itemBilibili->setData(Qt::UserRole, ":/svg/dark/user.svg");

  auto *itemAbout = new QListWidgetItem(
      ThemeManager::getAdaptiveIcon(":/svg/dark/about.svg"), tr(" About"));
  itemAbout->setData(Qt::UserRole, ":/svg/dark/about.svg");

  
  itemGeneral->setSizeHint(QSize(220, 44));
  itemAppearance->setSizeHint(QSize(220, 44));
  itemAccount->setSizeHint(QSize(220, 44));
  itemPlayer->setSizeHint(QSize(220, 44));
  itemLibrary->setSizeHint(QSize(220, 44));
  itemTrakt->setSizeHint(QSize(220, 44));
  itemBilibili->setSizeHint(QSize(220, 44));
  itemAbout->setSizeHint(QSize(220, 44));

  m_navMenu->addItem(itemGeneral);
  m_navMenu->addItem(itemAppearance);
  m_navMenu->addItem(itemAccount);
  m_navMenu->addItem(itemLibrary);
  m_navMenu->addItem(itemPlayer);
  m_navMenu->addItem(itemTrakt);
  m_navMenu->addItem(itemBilibili);
  m_navMenu->addItem(itemAbout);

  leftLayout->addWidget(m_titleLabel);
  leftLayout->addWidget(m_navMenu);

  
  m_stack = new SlidingStackedWidget(this);
  m_stack->setObjectName("SettingsStack");

  
  
  
  
  const int kPageCount = 8;
  m_scrollAreas.reserve(kPageCount);
  m_scrollAnims.reserve(kPageCount);
  m_scrollTargets.reserve(kPageCount);
  m_pages.reserve(kPageCount);
  for (int i = 0; i < kPageCount; ++i) {
    auto *placeholder = new QWidget(m_stack);
    placeholder->setAttribute(Qt::WA_StyledBackground, true);
    placeholder->setObjectName("SettingsPagePlaceholder");
    m_stack->addWidget(placeholder);

    m_scrollAreas.append(nullptr);
    m_scrollAnims.append(nullptr);
    m_scrollTargets.append(0);
    m_pages.append(QPointer<QWidget>());
  }

  mainLayout->addWidget(m_leftPanel);
  mainLayout->addWidget(m_stack, 1);

  
  qApp->postEvent(this, new QEvent(QEvent::StyleChange));
}

QScrollArea *SettingsView::wrapInScrollArea(QWidget *page, int row) {
  
  page->setAttribute(Qt::WA_StyledBackground, true);

  auto *scroll = new QScrollArea(m_stack);
  scroll->setObjectName("SettingsScrollArea");
  scroll->setWidget(page);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);

  
  scroll->viewport()->setAutoFillBackground(false);

  
  scroll->viewport()->installEventFilter(this);

  
  if (row >= 0 && row < m_scrollAreas.size()) {
    m_scrollAreas[row] = scroll;

    auto *anim =
        new QPropertyAnimation(scroll->verticalScrollBar(), "value", this);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->setDuration(450);
    m_scrollAnims[row] = anim;
    m_scrollTargets[row] = 0;
  }

  return scroll;
}

void SettingsView::ensurePageAt(int row) {
  if (row < 0 || row >= m_pages.size()) {
    return;
  }
  if (m_pages[row]) {
    
    return;
  }

  qDebug() << "[SettingsView] Lazy instantiate page row=" << row;

  QWidget *page = nullptr;
  switch (row) {
  case 0:
    page = new PageGeneral(m_core, m_stack);
    break;
  case 1:
    page = new PageAppearance(m_core, m_stack);
    break;
  case 2:
    page = new PageAccount(m_core, m_stack);
    break;
  case 3:
    page = new PageLibrary(m_core, m_stack);
    break;
  case 4:
    page = new PagePlayer(m_core, m_stack);
    break;
  case 5:
    page = new PageTrakt(m_core, m_stack);
    break;
  case 6:
    page = new PageBilibili(m_core, m_stack);
    break;
  case 7:
    page = new PageAbout(m_core, m_stack);
    break;
  default:
    return;
  }

  QScrollArea *scroll = wrapInScrollArea(page, row);

  
  
  QWidget *placeholder = m_stack->widget(row);
  const bool wasBlocked = m_stack->blockSignals(true);
  m_stack->insertWidget(row, scroll); 
  m_stack->removeWidget(placeholder);
  m_stack->blockSignals(wasBlocked);
  placeholder->deleteLater();

  m_pages[row] = scroll;
}

void SettingsView::setupConnections() {
  
  connect(m_navMenu, &QListWidget::currentRowChanged, this, [this](int row) {
    if (row >= 0) {
      ensurePageAt(row);
      m_stack->setCurrentIndex(row);
    }
  });

  
  connect(ThemeManager::instance(), &ThemeManager::themeChanged, this,
          &SettingsView::onThemeChanged);
}


void SettingsView::onThemeChanged() {
  for (int i = 0; i < m_navMenu->count(); ++i) {
    auto *item = m_navMenu->item(i);
    
    QString svgPath = item->data(Qt::UserRole).toString();
    if (!svgPath.isEmpty()) {
      
      item->setIcon(ThemeManager::getAdaptiveIcon(svgPath));
    }
  }
}

bool SettingsView::eventFilter(QObject *obj, QEvent *event) {
  if (event->type() == QEvent::Wheel) {
    
    for (int i = 0; i < m_scrollAreas.size(); ++i) {
      
      if (!m_scrollAreas[i]) {
        continue;
      }
      if (obj == m_scrollAreas[i]->viewport()) {
        auto *we   = static_cast<QWheelEvent *>(event);
        auto *vBar = m_scrollAreas[i]->verticalScrollBar();
        auto *anim = m_scrollAnims[i];

        if (vBar && anim) {
          int currentVal = vBar->value();

          
          if (anim->state() == QAbstractAnimation::Running) {
            currentVal = m_scrollTargets[i];
          }

          
          int step      = we->angleDelta().y();
          int newTarget = currentVal - step;

          
          newTarget = qBound(vBar->minimum(), newTarget, vBar->maximum());

          if (newTarget != vBar->value()) {
            m_scrollTargets[i] = newTarget;
            anim->stop();
            anim->setStartValue(vBar->value());
            anim->setEndValue(newTarget);
            anim->start();
          }
        }
        return true; 
      }
    }
  }

  return QWidget::eventFilter(obj, event);
}

void SettingsView::hideEvent(QHideEvent *event) {
  // 离开设置页（切回主界面）时记下当前位置
  saveNavigationState();
  BaseView::hideEvent(event);
}

void SettingsView::restoreNavigationState() {
  auto *cfg = ConfigStore::instance();
  const int savedRow = cfg->get<int>(ConfigKeys::SettingsLastTab, 0);
  const int targetRow =
      (savedRow >= 0 && savedRow < m_navMenu->count()) ? savedRow : 0;
  m_navMenu->setCurrentRow(targetRow);  // 触发 ensurePageAt + 切页

  const int savedPos = cfg->get<int>(ConfigKeys::SettingsLastScroll, 0);
  if (savedPos <= 0) {
    return;
  }
  m_pendingScrollRow     = targetRow;
  m_pendingScrollPos     = savedPos;
  m_pendingScrollRetries = 8;
  // 页面是懒加载的，构造当下布局还没跑完，滚动条 maximum 仍为 0
  QTimer::singleShot(0, this, [this]() { applyPendingScroll(); });
}

void SettingsView::saveNavigationState() {
  auto *cfg = ConfigStore::instance();
  const int navRow = m_navMenu ? m_navMenu->currentRow() : -1;
  if (navRow < 0) {
    return;
  }
  cfg->set(ConfigKeys::SettingsLastTab, navRow);
  if (navRow < m_scrollAreas.size() && m_scrollAreas[navRow]) {
    cfg->set(ConfigKeys::SettingsLastScroll,
             m_scrollAreas[navRow]->verticalScrollBar()->value());
  }
}

void SettingsView::applyPendingScroll() {
  if (m_pendingScrollRow < 0 || m_pendingScrollRow >= m_scrollAreas.size()) {
    m_pendingScrollRow = -1;
    return;
  }
  QScrollArea *area = m_scrollAreas[m_pendingScrollRow];
  if (!area) {
    m_pendingScrollRow = -1;
    return;
  }
  QScrollBar *bar = area->verticalScrollBar();
  if (!bar) {
    m_pendingScrollRow = -1;
    return;
  }
  // 布局尚未完成时 maximum 还不够大，等它涨上来再设置（最多重试几次）
  if (bar->maximum() < m_pendingScrollPos && m_pendingScrollRetries-- > 0) {
    QTimer::singleShot(50, this, [this]() { applyPendingScroll(); });
    return;
  }
  bar->setValue(qMin(m_pendingScrollPos, bar->maximum()));
  m_pendingScrollRow = -1;
}
