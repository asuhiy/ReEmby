#include "logmanager.h"
#include "config/config_keys.h"
#include "config/configstore.h"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <utils/apppaths.h>

static const char *kLoggerName = "reemby";
static const int kMaxFileSize = 5 * 1024 * 1024; 
static const int kMaxFiles = 3;


// 高产出、低价值的调试日志：一次播放可产生数千行，把真正有用的信息淹没
// （实测一轮播放 6.3k 行里有 ~4.5k 行属于下面几类）。它们都在 debug 级别，
// 且多数由上游模块打印，因此统一在日志出口处丢弃，而不是逐个去改各模块。
// 需要重新看到这些日志时，把对应前缀从下表移除即可。
static bool isHighVolumeDebugMessage(const QString &msg) {
  static const char *const kDroppedPrefixes[] = {
      "[ProxyManager] resolved",           // 每次 HTTP 请求前解析代理
      "[NetworkManager] JSON decoded",     // 每次 API 响应体解码
      "[MediaService] image request",      // 每张图片的排队 / 完成
      "[MediaListModel] image request",    // 每张图片入队
      "[MediaService] getLibraryItemsPage",// 每次媒体库分页拉取
      "[MediaService] decoded image cache hit",
      // MPV 转发的 libavcodec 告警：DV 片源几乎每帧一条（实测单次播放 2000+ 行）。
      // 已在 mpv 侧设 msg-level，这里再兜一层，避免 mpv 版本差异导致漏网。
      "Multiple Dolby Vision RPUs",
  };
  for (const char *prefix : kDroppedPrefixes) {
    if (msg.contains(QLatin1String(prefix)))
      return true;
  }
  return false;
}

static void spdlogMessageHandler(QtMsgType type, const QMessageLogContext &ctx,
                                  const QString &msg) {
  auto logger = spdlog::get(kLoggerName);
  if (!logger)
    return;

  // 只丢弃 debug 级：同名模块的 warning（如 [MediaService] slow image request）
  // 是真正的异常信号，必须保留。
  if (type == QtDebugMsg && isHighVolumeDebugMessage(msg))
    return;

  std::string m = msg.toStdString();
  std::string category = ctx.category ? ctx.category : "";
  std::string formatted =
      category.empty() ? m : fmt::format("[{}] {}", category, m);

  switch (type) {
  case QtDebugMsg:
    logger->debug("{}", formatted);
    break;
  case QtInfoMsg:
    logger->info("{}", formatted);
    break;
  case QtWarningMsg:
    logger->warn("{}", formatted);
    break;
  case QtCriticalMsg:
    logger->error("{}", formatted);
    break;
  case QtFatalMsg:
    logger->critical("{}", formatted);
    logger->flush();
    break;
  }
}

LogManager::LogManager(QObject *parent) : QObject(parent) {}

LogManager *LogManager::instance() {
  static LogManager inst;
  return &inst;
}

QString LogManager::logFilePath() const {
  return AppPaths::dataRoot() + QStringLiteral("/reemby.log");
}

void LogManager::init() {
  if (m_initialized)
    return;
  m_initialized = true;

  bool enabled =
      ConfigStore::instance()->get<bool>(ConfigKeys::LogEnable, false);
  if (enabled) {
    enable();
  }
  // 数据目录是所有配置/缓存/日志的根（便携模式下位于 exe 旁的 config 目录），
  // 启动时记录一次，便于定位文件位置。
  qInfo().noquote() << "[AppPaths] data root:" << AppPaths::dataRoot()
                    << "| portable:" << AppPaths::isPortable();
}

void LogManager::enable() {
  if (m_enabled)
    return;

  setupSpdlog();
  m_enabled = true;
  ConfigStore::instance()->set(ConfigKeys::LogEnable, true);
}

void LogManager::disable() {
  if (!m_enabled)
    return;

  teardownSpdlog();
  m_enabled = false;
  ConfigStore::instance()->set(ConfigKeys::LogEnable, false);
}

void LogManager::setupSpdlog() {
  
  QString logPath = logFilePath();
  QDir().mkpath(QFileInfo(logPath).absolutePath());

  try {
    
    auto fileSink =
        std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            logPath.toStdString(), kMaxFileSize, kMaxFiles);

    auto logger =
        std::make_shared<spdlog::logger>(kLoggerName, fileSink);
    logger->set_level(spdlog::level::debug);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    logger->flush_on(spdlog::level::warn); 

    spdlog::register_logger(logger);

    
    qInstallMessageHandler(spdlogMessageHandler);

    logger->info("=== ReEmby Logging Started ===");
  } catch (const spdlog::spdlog_ex &ex) {
    
    Q_UNUSED(ex);
  }
}

void LogManager::teardownSpdlog() {
  auto logger = spdlog::get(kLoggerName);
  if (logger) {
    logger->info("=== ReEmby Logging Stopped ===");
    logger->flush();
  }

  
  qInstallMessageHandler(nullptr);

  spdlog::drop(kLoggerName);
}

QString LogManager::logFileSize() const {
  QFileInfo info(logFilePath());
  if (!info.exists())
    return QStringLiteral("0 B");

  qint64 bytes = info.size();
  if (bytes < 1024)
    return QStringLiteral("%1 B").arg(bytes);
  if (bytes < 1024 * 1024)
    return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
  return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

void LogManager::clearLog() {
  
  bool wasEnabled = m_enabled;
  if (wasEnabled) {
    teardownSpdlog();
  }

  
  
  QString logPath = logFilePath();
  QFileInfo fi(logPath);
  QString baseName = fi.completeBaseName(); 
  QString suffix = fi.suffix();             
  QString dir = fi.absolutePath();

  QFile::remove(logPath);
  for (int i = 1; i <= kMaxFiles; ++i) {
    QString rotated = dir + "/" + baseName + "." + QString::number(i) + "." + suffix;
    QFile::remove(rotated);
  }

  
  if (wasEnabled) {
    m_enabled = false; 
    enable();
  }
}
