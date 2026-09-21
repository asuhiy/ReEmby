#include "serverprofile.h"

#include <QCoreApplication>
#include <QSysInfo>

namespace {

// "x86_64" -> "x64" 等，贴近常见 User-Agent 里书写的平台标签。
QString cpuArchLabel()
{
    const QString arch = QSysInfo::currentCpuArchitecture();
    if (arch == QLatin1String("x86_64")) {
        return QStringLiteral("x64");
    }
    if (arch == QLatin1String("i386") || arch == QLatin1String("i686")) {
        return QStringLiteral("x86");
    }
    if (arch == QLatin1String("aarch64") || arch == QLatin1String("arm64")) {
        return QStringLiteral("arm64");
    }
    return arch;
}

QString platformLabel()
{
#ifdef Q_OS_WIN
    return QStringLiteral("Windows NT %1").arg(QSysInfo::kernelVersion());
#else
    return QSysInfo::prettyProductName();
#endif
}

} // namespace

// 默认客户端身份 = ReEmby 自己的名字 + 当前构建版本（APP_VERSION），
// 形如 "ReEmby/0.11.0 (Windows NT 10.0.26100; x64)"。服务器端会看到
// Client="ReEmby"、Version=<版本号>（不再伪装成第三方播放器）。
QString ServerProfile::defaultUserAgent()
{
    QString version = QCoreApplication::applicationVersion().trimmed();
    if (version.isEmpty()) {
        version = QStringLiteral("0");
    }
    return QStringLiteral("ReEmby/%1 (%2; %3)")
        .arg(version, platformLabel(), cpuArchLabel());
}
