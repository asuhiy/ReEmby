#include "mainwindow.h"

#include <QApplication>
#include <QIcon>
#include <QLocale>
#include <QSurfaceFormat>
#include <QThread>
#include <QTranslator>
#include "api/proxymanager.h"
#include "components/mpvcontroller.h"
#include "managers/languagemanager.h"
#include "managers/logmanager.h"
#include "managers/singleapplicationmanager.h"
#include "config/config_keys.h"
#include "config/configstore.h"
#include "utils/apppaths.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif
int main(int argc, char *argv[]) {
#ifdef Q_OS_WIN
  bool isRDP = GetSystemMetrics(SM_REMOTESESSION) != 0;
  if (isRDP) {
    
    
    qputenv("QT_OPENGL", "software");
  }
#endif

  
  QSurfaceFormat format;
  format.setVersion(3, 3); 
  format.setProfile(QSurfaceFormat::CoreProfile);
  format.setDepthBufferSize(24);  
  format.setStencilBufferSize(8); 
  format.setSwapInterval(1);      
  format.setSwapBehavior(QSurfaceFormat::DoubleBuffer); 
  QSurfaceFormat::setDefaultFormat(format);

  QGuiApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
  // QtWebEngine（Trakt 授权内嵌浏览器）与 mpv 的 QOpenGLWidget 混用 GL 时，
  // 必须在 QApplication 构造前开启全局 context 共享，否则 WebView 白屏。
  QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
  QApplication a(argc, argv);
  a.setApplicationName(APP_NAME);
  a.setApplicationVersion(APP_VERSION);
  // 数据目录由 AppPaths 决定（见下）：绿色包默认落到 <exe目录>/config，
  // 安装版（无写权限）回退 %LOCALAPPDATA%/asuhiy/ReEmby。从旧版本升级不会自动
  // 迁移数据：把旧目录的 config.ini、secret-box-key.bin、servers.json 拷到
  // 新数据目录即可，缓存可丢弃重建。
  a.setOrganizationName("asuhiy");
  a.setOrganizationDomain("github.com/asuhiy/ReEmby");

  // 数据根：优先 <exe目录>/config（绿色包便携，拷走即走），目录不可写时
  // 回退系统位置 %LOCALAPPDATA%/asuhiy/ReEmby；--data-dir=<path> 可显式覆盖。
  // 必须在任何持久化访问（ConfigStore/LogManager 等）之前决定。
  AppPaths::initialize(a.arguments());

  LogManager::instance()->init();

  SingleApplicationManager singleApplication;
  const auto singleApplicationResult = singleApplication.start(
      ConfigStore::instance()->get<bool>(ConfigKeys::SingleApplication, false));
  if (singleApplicationResult ==
      SingleApplicationManager::StartResult::SecondaryInstance) {
    return 0;
  }
#if !defined(Q_OS_MACOS) && !defined(Q_OS_MAC)
  QGuiApplication::setDesktopFileName(QStringLiteral("reemby"));
  const QIcon appIcon(QStringLiteral(":/svg/qemby_logo.svg"));
  a.setWindowIcon(appIcon);
#endif

  LanguageManager::instance()->init();

  
  
  ProxyManager::installApplicationFactory();

  MainWindow w;
  QObject::connect(&singleApplication,
                   &SingleApplicationManager::activationRequested, &w, [&w]() {
                     if (w.isMinimized()) {
                       w.setWindowState(w.windowState() & ~Qt::WindowMinimized);
                     }
                     w.show();
                     w.raise();
                     w.activateWindow();
                   });
#if !defined(Q_OS_MACOS) && !defined(Q_OS_MAC)
  w.setWindowIcon(appIcon);
#endif
  w.show();

  
  
  
  
  
  
  
  
  
  
  
  
  
  MpvController::warmupOnce();

  int ret = a.exec();
  
  return ret;
}
