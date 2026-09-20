#include "apppaths.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

namespace {

QString g_dataRoot;
bool g_portable = false;

// 实际写一个探测文件来判定可写性（比 QFileInfo::isWritable 可靠：ACL、
// 只读属性、Program Files 权限都会如实反映）。
bool directoryWritable(const QString &dir)
{
    if (dir.isEmpty() || !QDir().mkpath(dir)) {
        return false;
    }
    const QString probePath = dir + QStringLiteral("/.reemby-write-probe");
    QFile probe(probePath);
    if (!probe.open(QIODevice::WriteOnly)) {
        return false;
    }
    probe.write("ok");
    probe.close();
    QFile::remove(probePath);
    return true;
}

QString defaultDataRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

} // namespace

void AppPaths::initialize(const QStringList &arguments)
{
    // 1) --data-dir=<path> 显式指定（最高优先级）。
    for (const QString &arg : arguments) {
        if (arg.startsWith(QLatin1String("--data-dir="))) {
            const QString dir = QDir::cleanPath(arg.mid(11).trimmed());
            if (!dir.isEmpty() && directoryWritable(dir)) {
                g_dataRoot = dir;
                g_portable = false;
                qInfo().noquote() << "[AppPaths] data root (--data-dir):" << g_dataRoot;
                return;
            }
            qWarning().noquote() << "[AppPaths] --data-dir not usable, ignored:" << dir;
        }
    }

    // 2) 便携模式：<exe目录>/config 可写则用它（绿色包解压即用，拷走即走）。
    const QString portableDir =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("config"));
    if (directoryWritable(portableDir)) {
        g_dataRoot = portableDir;
        g_portable = true;
        qInfo().noquote() << "[AppPaths] data root (portable):" << g_dataRoot;
        return;
    }

    // 3) 系统默认位置（安装版/无写权限）。
    g_dataRoot = defaultDataRoot();
    g_portable = false;
    QDir().mkpath(g_dataRoot);
    qInfo().noquote() << "[AppPaths] data root (default):" << g_dataRoot;
}

QString AppPaths::dataRoot()
{
    if (g_dataRoot.isEmpty()) {
        // 未显式初始化（例如单元测试/提前访问）：退化为默认位置，保证不崩。
        g_dataRoot = defaultDataRoot();
        QDir().mkpath(g_dataRoot);
    }
    return g_dataRoot;
}

QString AppPaths::cacheDir()
{
    return dataRoot() + QStringLiteral("/cache");
}

bool AppPaths::isPortable()
{
    return g_portable;
}
