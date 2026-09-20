#ifndef APPPATHS_H
#define APPPATHS_H

#include <QString>
#include <QStringList>
#include <qembycore_global.h>

// 应用数据根的统一入口。所有"应用私有数据"（config.ini、servers.json、
// secret-box-key.bin、缓存、弹幕、日志等）都必须经过这里取路径——不要再直接
// 使用 QStandardPaths，否则便携模式（数据放 exe 旁 config 文件夹）会漏掉。
//
// 根目录的选定规则（AppPaths::initialize，在 main() 早期调用一次）：
//   1. 命令行 --data-dir=<path>（可写时生效）
//   2. <exe目录>/config（绿色包/便携场景：目录可写即采用）
//   3. 系统默认位置 %LOCALAPPDATA%/<org>/<app>（安装版 / 无写权限时）
namespace AppPaths {

// 选定数据根并创建目录。必须在任何持久化访问（ConfigStore 等）之前调用。
QEMBYCORE_EXPORT void initialize(const QStringList &arguments);

// 数据根（"配置相关的东西"统一放这里，含 cache/danmaku 子目录）。
QEMBYCORE_EXPORT QString dataRoot();

// dataRoot()/cache —— 缓存类数据的家（不存在时由调用方按需 mkpath）。
QEMBYCORE_EXPORT QString cacheDir();

// 是否使用了 exe 旁的便携目录。
QEMBYCORE_EXPORT bool isPortable();

} // namespace AppPaths

#endif // APPPATHS_H
