#include "iconpackservice.h"

#include "../../config/config_keys.h"
#include "../../config/configstore.h"
#include "../../utils/apppaths.h"

#include <QDebug>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <qcoronetwork.h>

#include <utility>

namespace {

// 图标源 JSON 只有几十 KB，给跨洋链路留足余量。
constexpr int kPackRequestTimeoutMs = 20000;

// 图标图片实测最大 ~60KB（白糖 / Softlyx 那几套），同样留余量。
constexpr int kIconRequestTimeoutMs = 30000;

// 磁盘缓存上限：6 个内置源合计约 2700 张图、平均 ~30KB，约 80MB；
// 留到 200MB 是为了容纳用户自己添加的源。
constexpr qint64 kIconPackCacheBytes = 200LL * 1024 * 1024;

QNetworkRequest buildRequest(const QString &url, int timeoutMs)
{
    QNetworkRequest request{QUrl(url)};
    // PreferCache：缓存新鲜就直接用、不发请求；过期则带
    // If-None-Match / If-Modified-Since 发条件请求，304 复用缓存内容。
    // （Qt 的 QNetworkDiskCache 负责这一切，上层拿到的永远是一份完整 body。）
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::PreferCache);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ReEmby"));
    request.setTransferTimeout(timeoutMs);
    return request;
}

}  // namespace

IconPackService::IconPackService(QObject *parent)
    : QObject(parent)
{
    m_manager = new QNetworkAccessManager(this);

    auto *diskCache = new QNetworkDiskCache(this);
    const QString cachePath =
        AppPaths::cacheDir() + QStringLiteral("/reEmby_IconPackCache");
    QDir().mkpath(cachePath);
    diskCache->setCacheDirectory(cachePath);
    diskCache->setMaximumCacheSize(kIconPackCacheBytes);
    m_manager->setCache(diskCache);

    loadCustomSources();
}

IconPackService *IconPackService::instance()
{
    static IconPackService service;
    return &service;
}

QList<IconPackSource> IconPackService::builtinSources()
{
    // 内置源的显示名是我们定的固定文案：同一个作者的多个 JSON 里，
    // Softlyx 那两个文件的 name 完全一样（都叫 "Fileball Icon"），
    // 白糖那三个带 "@baiitang" 后缀，都没法直接拿来当列表项标题。
    return {
        {QStringLiteral("https://raw.githubusercontent.com/lige47/"
                        "QuanX-icon-rule/main/lige-emby-icon.json"),
         tr("离歌Emby专用"), true},
        {QStringLiteral("https://raw.githubusercontent.com/baiitang/Sakura/"
                        "main/Fileball/Fang/tubiao.json"),
         tr("白糖（方）"), true},
        {QStringLiteral("https://raw.githubusercontent.com/baiitang/Sakura/"
                        "main/Fileball/Yuan/tubiao.json"),
         tr("白糖（圆）"), true},
        {QStringLiteral("https://raw.githubusercontent.com/baiitang/Sakura/"
                        "main/Fileball/Toun/tubiao.json"),
         tr("白糖（透明）"), true},
        {QStringLiteral("https://raw.githubusercontent.com/Softlyx/Fileball/"
                        "main/FANG/tubiao.json"),
         tr("Softlyx（方）"), true},
        {QStringLiteral("https://raw.githubusercontent.com/Softlyx/Fileball/"
                        "main/YUAN/tubiao.json"),
         tr("Softlyx（圆）"), true},
    };
}

QList<IconPackSource> IconPackService::sources() const
{
    QList<IconPackSource> all = builtinSources();
    all.append(m_customSources);
    return all;
}

bool IconPackService::isBuiltinSource(const QString &rawUrl) const
{
    const QString url = normalizeUrl(rawUrl);
    if (url.isEmpty()) {
        return false;
    }
    const QList<IconPackSource> builtins = builtinSources();
    for (const IconPackSource &source : builtins) {
        if (source.url == url) {
            return true;
        }
    }
    return false;
}

bool IconPackService::addCustomSource(const QString &rawUrl)
{
    const QString url = normalizeUrl(rawUrl);
    if (url.isEmpty()) {
        qWarning() << "[IconPackService] addCustomSource rejected invalid url";
        return false;
    }

    if (isBuiltinSource(url)) {
        qWarning() << "[IconPackService] addCustomSource rejected builtin url";
        return false;
    }

    for (const IconPackSource &source : std::as_const(m_customSources)) {
        if (source.url == url) {
            qWarning() << "[IconPackService] addCustomSource rejected duplicate";
            return false;
        }
    }

    IconPackSource source;
    source.url = url;
    // 先用域名占位，拉取成功后由 updateCustomSourceName 换成 JSON 里的 name。
    source.displayName = QUrl(url).host();
    source.builtin = false;
    m_customSources.append(source);
    saveCustomSources();

    qInfo() << "[IconPackService] custom source added"
            << "| host=" << source.displayName
            << "| total=" << m_customSources.size();
    Q_EMIT sourcesChanged();
    return true;
}

bool IconPackService::removeCustomSource(const QString &rawUrl)
{
    const QString url = normalizeUrl(rawUrl);
    if (url.isEmpty() || isBuiltinSource(url)) {
        return false;
    }

    const qsizetype removed = m_customSources.removeIf(
        [&url](const IconPackSource &source) { return source.url == url; });
    if (removed == 0) {
        return false;
    }

    saveCustomSources();
    qInfo() << "[IconPackService] custom source removed"
            << "| total=" << m_customSources.size();
    Q_EMIT sourcesChanged();
    return true;
}

void IconPackService::updateCustomSourceName(const QString &rawUrl,
                                             const QString &name)
{
    const QString url = normalizeUrl(rawUrl);
    const QString cleaned = cleanName(name);
    if (url.isEmpty() || cleaned.isEmpty()) {
        return;
    }

    for (IconPackSource &source : m_customSources) {
        if (source.url == url && source.displayName != cleaned) {
            source.displayName = cleaned;
            saveCustomSources();
            Q_EMIT sourcesChanged();
            return;
        }
    }
}

QCoro::Task<IconPack> IconPackService::loadPack(QString sourceUrl)
{
    IconPack pack;
    pack.sourceUrl = sourceUrl;

    const QString url = normalizeUrl(sourceUrl);
    if (url.isEmpty()) {
        pack.errorMessage = tr("图标源地址无效");
        co_return pack;
    }

    QNetworkReply *reply = m_manager->get(buildRequest(url, kPackRequestTimeoutMs));
    co_await reply;

    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString errorText = reply->errorString();
    const QByteArray payload = reply->readAll();
    reply->deleteLater();

    if (networkError != QNetworkReply::NoError || payload.isEmpty()) {
        qWarning() << "[IconPackService] loadPack failed"
                   << "| httpStatus=" << httpStatus
                   << "| error=" << errorText
                   << "| bytes=" << payload.size();
        pack.errorMessage = errorText.isEmpty()
                                ? tr("图标源获取失败（HTTP %1）").arg(httpStatus)
                                : errorText;
        co_return pack;
    }

    pack = parsePack(payload, sourceUrl);
    if (pack.succeeded()) {
        qInfo() << "[IconPackService] pack loaded"
                << "| name=" << pack.name
                << "| icons=" << pack.count()
                << "| bytes=" << payload.size();
    } else {
        qWarning() << "[IconPackService] pack parse failed"
                   << "| error=" << pack.errorMessage;
    }
    co_return pack;
}

QCoro::Task<QByteArray> IconPackService::downloadIcon(QString url)
{
    const QString normalized = normalizeUrl(url);
    if (normalized.isEmpty()) {
        co_return QByteArray();
    }

    QNetworkReply *reply = m_manager->get(buildRequest(normalized, kIconRequestTimeoutMs));
    co_await reply;

    const QNetworkReply::NetworkError networkError = reply->error();
    const QByteArray payload = reply->readAll();
    if (networkError != QNetworkReply::NoError) {
        qWarning() << "[IconPackService] downloadIcon failed"
                   << "| error=" << reply->errorString()
                   << "| url=" << normalized;
    }
    reply->deleteLater();
    co_return payload;
}

QString IconPackService::normalizeUrl(const QString &rawUrl)
{
    const QString trimmed = rawUrl.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }

    const QUrl url(trimmed);
    if (!url.isValid() || url.host().isEmpty()) {
        return QString();
    }

    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) {
        return QString();
    }
    return url.toString();
}

QString IconPackService::cleanName(const QString &rawName)
{
    QString name = rawName.trimmed();
    // 白糖那几套的 name 形如 "Emby图标库(方)@baiitang"，去掉作者后缀。
    const qsizetype at = name.indexOf(QLatin1Char('@'));
    if (at > 0) {
        name = name.left(at).trimmed();
    }
    return name;
}

IconPack IconPackService::parsePack(const QByteArray &payload,
                                    const QString &sourceUrl)
{
    IconPack pack;
    pack.sourceUrl = sourceUrl;

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        pack.errorMessage =
            tr("图标源不是合法的 JSON：%1").arg(parseError.errorString());
        return pack;
    }
    if (!document.isObject()) {
        pack.errorMessage = tr("图标源格式不正确：顶层不是 JSON 对象");
        return pack;
    }

    const QJsonObject root = document.object();
    pack.name = cleanName(root.value(QStringLiteral("name")).toString());
    pack.description =
        root.value(QStringLiteral("description")).toString().trimmed();

    const QJsonArray icons = root.value(QStringLiteral("icons")).toArray();
    pack.entries.reserve(icons.size());
    for (const QJsonValue &value : icons) {
        const QJsonObject object = value.toObject();
        IconPackEntry entry;
        entry.name = object.value(QStringLiteral("name")).toString().trimmed();
        entry.url = object.value(QStringLiteral("url")).toString().trimmed();
        if (entry.isValid()) {
            pack.entries.append(entry);
        }
    }

    if (pack.entries.isEmpty()) {
        pack.errorMessage = tr("图标源里没有可用的图标");
    }
    return pack;
}

void IconPackService::loadCustomSources()
{
    m_customSources.clear();

    ConfigStore *config = ConfigStore::instance();
    if (!config) {
        return;
    }

    const QString raw = config->get<QString>(
        QString::fromLatin1(ConfigKeys::IconPackCustomSources), QString());
    if (raw.isEmpty()) {
        return;
    }

    const QJsonDocument document =
        QJsonDocument::fromJson(raw.toUtf8());
    const QJsonArray array = document.array();
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        IconPackSource source;
        source.url = normalizeUrl(object.value(QStringLiteral("url")).toString());
        source.displayName =
            cleanName(object.value(QStringLiteral("name")).toString());
        if (source.displayName.isEmpty()) {
            source.displayName = QUrl(source.url).host();
        }
        source.builtin = false;
        if (source.isValid()) {
            m_customSources.append(source);
        }
    }

    qInfo() << "[IconPackService] custom sources loaded"
            << "| count=" << m_customSources.size();
}

void IconPackService::saveCustomSources()
{
    ConfigStore *config = ConfigStore::instance();
    if (!config) {
        return;
    }

    QJsonArray array;
    for (const IconPackSource &source : std::as_const(m_customSources)) {
        QJsonObject object;
        object[QStringLiteral("url")] = source.url;
        object[QStringLiteral("name")] = source.displayName;
        array.append(object);
    }

    config->set(QString::fromLatin1(ConfigKeys::IconPackCustomSources),
                QString::fromUtf8(
                    QJsonDocument(array).toJson(QJsonDocument::Compact)));
}
