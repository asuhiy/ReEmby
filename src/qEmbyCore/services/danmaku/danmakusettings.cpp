#include "danmakusettings.h"

#include "../../config/config_keys.h"
#include "../../config/configstore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <utility>

namespace
{

constexpr auto kOfficialDandanplayServerId = "default";
constexpr auto kOfficialDandanplayProvider = "dandanplay";
constexpr auto kOfficialDandanplayName = "DandanPlay Open API";
constexpr auto kOfficialDandanplayBaseUrl = "https://api.dandanplay.net";
constexpr auto kOfficialDandanplayContentScope = "anime";


constexpr auto kBuiltInOfficialDandanplayAppId = "wyptw278x3";
constexpr auto kBuiltInOfficialDandanplayAppSecret = "ApQZbydpeafcDTe8pBX3EJxifAuhfkSn";

constexpr auto kBilibiliServerId = "bilibili";
constexpr auto kBilibiliProvider = "bilibili";
constexpr auto kBilibiliName = "Bilibili";
constexpr auto kBilibiliBaseUrl = "https://www.bilibili.com";

QString settingKey(const QString &serverId, const char *baseKey)
{
    return ConfigKeys::forServer(serverId.trimmed(), baseKey);
}

QString normalizeBaseUrl(QString baseUrl)
{
    baseUrl = baseUrl.trimmed();
    while (baseUrl.endsWith('/') && !baseUrl.endsWith(QStringLiteral("://")))
    {
        baseUrl.chop(1);
    }
    return baseUrl;
}

QString normalizedHost(const QString &baseUrl)
{
    const QUrl url = QUrl::fromUserInput(baseUrl.trimmed());
    return url.host().trimmed().toLower();
}

bool isOfficialDandanplayEndpoint(const QString &baseUrl)
{
    return normalizedHost(baseUrl) == QStringLiteral("api.dandanplay.net");
}

bool isBuiltInOfficialDandanplayServer(const DanmakuServerDefinition &server)
{
    // Scope the builtIn flag to the dandanplay provider so other built-in
    // sources (bilibili) are not mistaken for the official dandanplay server.
    if (server.provider != QLatin1String(kOfficialDandanplayProvider))
    {
        return false;
    }
    return server.builtIn ||
           (server.id == QLatin1String(kOfficialDandanplayServerId) && isOfficialDandanplayEndpoint(server.baseUrl));
}

bool isBuiltInBilibiliServer(const DanmakuServerDefinition &server)
{
    return server.builtIn && server.provider == QLatin1String(kBilibiliProvider);
}

QString defaultServerName(const QString &baseUrl)
{
    const QString normalized = normalizeBaseUrl(baseUrl);
    if (normalized.isEmpty() || normalized == QString::fromLatin1(kOfficialDandanplayBaseUrl))
    {
        return QString::fromLatin1(kOfficialDandanplayName);
    }

    const QUrl url = QUrl::fromUserInput(normalized);
    if (!url.host().trimmed().isEmpty())
    {
        return url.host().trimmed();
    }
    return normalized;
}

DanmakuServerDefinition makeBuiltInOfficialDandanplayServer()
{
    DanmakuServerDefinition server;
    server.id = QString::fromLatin1(kOfficialDandanplayServerId);
    server.name = QString::fromLatin1(kOfficialDandanplayName);
    server.provider = QString::fromLatin1(kOfficialDandanplayProvider);
    server.baseUrl = QString::fromLatin1(kOfficialDandanplayBaseUrl);
    server.appId = QString::fromLatin1(kBuiltInOfficialDandanplayAppId);
    server.appSecret = QString::fromLatin1(kBuiltInOfficialDandanplayAppSecret);
    server.contentScope = QString::fromLatin1(kOfficialDandanplayContentScope);
    server.builtIn = true;
    server.enabled = true;
    return server;
}

DanmakuServerDefinition makeBuiltInBilibiliServer()
{
    DanmakuServerDefinition server;
    server.id = QString::fromLatin1(kBilibiliServerId);
    server.name = QString::fromLatin1(kBilibiliName);
    server.provider = QString::fromLatin1(kBilibiliProvider);
    server.baseUrl = QString::fromLatin1(kBilibiliBaseUrl);
    server.builtIn = true;
    // Opt-in: the source requires a logged-in Bilibili account, so it is
    // disabled until the user enables it and signs in.
    server.enabled = false;
    return server;
}

void applyBuiltInOfficialDandanplayDefaults(DanmakuServerDefinition *server)
{
    if (!server || !isBuiltInOfficialDandanplayServer(*server))
    {
        return;
    }

    const DanmakuServerDefinition builtIn = makeBuiltInOfficialDandanplayServer();
    server->builtIn = true;
    server->id = builtIn.id;
    server->baseUrl = builtIn.baseUrl;
    if (server->provider.trimmed().isEmpty())
    {
        server->provider = builtIn.provider;
    }
    if (server->name.trimmed().isEmpty())
    {
        server->name = builtIn.name;
    }
    if (server->appId.trimmed().isEmpty())
    {
        server->appId = builtIn.appId;
    }
    if (server->appSecret.trimmed().isEmpty())
    {
        server->appSecret = builtIn.appSecret;
    }
    if (server->contentScope.trimmed().isEmpty())
    {
        server->contentScope = builtIn.contentScope;
    }
}

DanmakuServerDefinition legacyServerDefinition(const QString &serverId)
{
    auto *store = ConfigStore::instance();

    DanmakuServerDefinition server = makeBuiltInOfficialDandanplayServer();
    server.provider = store->get<QString>(settingKey(serverId, ConfigKeys::DanmakuProvider), server.provider);
    server.baseUrl =
        normalizeBaseUrl(store->get<QString>(settingKey(serverId, ConfigKeys::DanmakuProviderBaseUrl), server.baseUrl));
    const QString legacyAppId = store->get<QString>(settingKey(serverId, ConfigKeys::DanmakuProviderAppId));
    const QString legacyAppSecret = store->get<QString>(settingKey(serverId, ConfigKeys::DanmakuProviderAppSecret));
    const bool hasLegacyAppId = !legacyAppId.trimmed().isEmpty();
    const bool hasLegacyAppSecret = !legacyAppSecret.trimmed().isEmpty();
    const bool officialEndpoint = isOfficialDandanplayEndpoint(server.baseUrl);
    if (!officialEndpoint && hasLegacyAppId)
    {
        server.appId = legacyAppId.trimmed();
    }
    if (!officialEndpoint && hasLegacyAppSecret)
    {
        server.appSecret = legacyAppSecret.trimmed();
    }
    server.name = defaultServerName(server.baseUrl);
    if (!officialEndpoint)
    {
        server.contentScope.clear();
        if (!hasLegacyAppId)
        {
            server.appId.clear();
        }
        if (!hasLegacyAppSecret)
        {
            server.appSecret.clear();
        }
        server.builtIn = false;
    }
    applyBuiltInOfficialDandanplayDefaults(&server);
    server.enabled = true;
    return server;
}

int firstEnabledServerIndex(const QList<DanmakuServerDefinition> &servers)
{
    for (int index = 0; index < servers.size(); ++index)
    {
        if (servers.at(index).enabled)
        {
            return index;
        }
    }
    return -1;
}

QList<DanmakuServerDefinition> normalizedServers(QList<DanmakuServerDefinition> servers)
{
    QSet<QString> usedIds;
    QList<DanmakuServerDefinition> normalized;
    normalized.reserve(servers.size());

    for (DanmakuServerDefinition &server : servers)
    {
        server.id = server.id.trimmed();
        server.name = server.name.trimmed();
        server.provider = server.provider.trimmed().toLower();
        server.baseUrl = normalizeBaseUrl(server.baseUrl);
        server.description = server.description.trimmed();
        server.appId = server.appId.trimmed();
        server.appSecret = server.appSecret.trimmed();
        server.accessToken = server.accessToken.trimmed();
        server.contentScope = server.contentScope.trimmed().toLower();
        server.builtIn = isBuiltInOfficialDandanplayServer(server) || isBuiltInBilibiliServer(server);

        if (server.provider.isEmpty())
        {
            server.provider = QString::fromLatin1(kOfficialDandanplayProvider);
        }
        if (server.provider == QLatin1String("danmu_api"))
        {
            server.appId.clear();
            server.appSecret.clear();
            if (server.contentScope.isEmpty())
            {
                server.contentScope = QStringLiteral("general");
            }
        }
        else
        {
            server.accessToken.clear();
        }
        if (server.id.isEmpty())
        {
            server.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        applyBuiltInOfficialDandanplayDefaults(&server);
        if (server.name.isEmpty())
        {
            server.name = defaultServerName(server.baseUrl);
        }
        if (server.baseUrl.isEmpty() || usedIds.contains(server.id))
        {
            continue;
        }

        usedIds.insert(server.id);
        normalized.append(server);
    }

    bool hasBuiltInOfficialServer = false;
    for (const DanmakuServerDefinition &server : std::as_const(normalized))
    {
        if (isBuiltInOfficialDandanplayServer(server))
        {
            hasBuiltInOfficialServer = true;
            break;
        }
    }
    if (!hasBuiltInOfficialServer)
    {
        normalized.prepend(makeBuiltInOfficialDandanplayServer());
    }

    // NOTE: the built-in bilibili server is intentionally NOT auto-
    // appended here. It is conditional on the Bilibili login state and
    // is added/removed dynamically by loadServers — signed in => present
    // (enabled), signed out => absent. Auto-appending it in the
    // normalisation step would resurrect it even when logged out.
    return normalized;
}

QList<DanmakuServerDefinition> parseServers(const QString &json)
{
    QList<DanmakuServerDefinition> servers;
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8());
    if (!document.isArray())
    {
        return servers;
    }

    const QJsonArray array = document.array();
    servers.reserve(array.size());
    for (const QJsonValue &value : array)
    {
        const QJsonObject object = value.toObject();
        DanmakuServerDefinition server;
        server.id = object.value(QStringLiteral("id")).toString().trimmed();
        server.name = object.value(QStringLiteral("name")).toString().trimmed();
        server.provider = object.value(QStringLiteral("provider")).toString().trimmed();
        server.baseUrl = object.value(QStringLiteral("baseUrl")).toString().trimmed();
        server.description = object.value(QStringLiteral("description")).toString().trimmed();
        server.appId = object.value(QStringLiteral("appId")).toString().trimmed();
        server.appSecret = object.value(QStringLiteral("appSecret")).toString().trimmed();
        server.accessToken = object.value(QStringLiteral("accessToken")).toString().trimmed();
        server.contentScope = object.value(QStringLiteral("contentScope")).toString().trimmed();
        server.builtIn = object.value(QStringLiteral("builtIn")).toBool(false);
        server.enabled = object.value(QStringLiteral("enabled")).toBool(true);
        servers.append(server);
    }
    return normalizedServers(servers);
}

QJsonArray toJsonArray(const QList<DanmakuServerDefinition> &servers)
{
    QJsonArray array;
    for (const DanmakuServerDefinition &server : servers)
    {
        DanmakuServerDefinition savedServer = server;
        if (isBuiltInOfficialDandanplayServer(savedServer) || isBuiltInBilibiliServer(savedServer))
        {
            savedServer.builtIn = true;
            savedServer.description.clear();
            savedServer.appId.clear();
            savedServer.appSecret.clear();
            savedServer.accessToken.clear();
        }

        QJsonObject object;
        object.insert(QStringLiteral("id"), savedServer.id);
        object.insert(QStringLiteral("name"), savedServer.name);
        object.insert(QStringLiteral("provider"), savedServer.provider);
        object.insert(QStringLiteral("baseUrl"), savedServer.baseUrl);
        object.insert(QStringLiteral("description"), savedServer.description);
        object.insert(QStringLiteral("appId"), savedServer.appId);
        object.insert(QStringLiteral("appSecret"), savedServer.appSecret);
        object.insert(QStringLiteral("accessToken"), savedServer.accessToken);
        object.insert(QStringLiteral("contentScope"), savedServer.contentScope);
        object.insert(QStringLiteral("builtIn"), savedServer.builtIn);
        object.insert(QStringLiteral("enabled"), savedServer.enabled);
        array.append(object);
    }
    return array;
}

} 

DanmakuServerDefinition DanmakuSettings::builtInOfficialDandanplayServer()
{
    return makeBuiltInOfficialDandanplayServer();
}

DanmakuServerDefinition DanmakuSettings::builtInBilibiliServer()
{
    return makeBuiltInBilibiliServer();
}

QList<DanmakuServerDefinition> DanmakuSettings::loadServers(QString serverId)
{
    // Shared global config: ONE danmaku source list for ALL Emby servers.
    // The user manages the sources once (in any server's danmaku settings
    // page) and every Emby server shares the same list — no per-server
    // duplication. When the global list is empty we return the dandanplay
    // built-in as the default so the UI has something to show; the user
    // then adds their own sources once and they apply everywhere.
    Q_UNUSED(serverId);
    QList<DanmakuServerDefinition> servers;
    const QString globalJson = ConfigStore::instance()->get<QString>(
        QString::fromLatin1(ConfigKeys::DanmakuServers));
    if (!globalJson.trimmed().isEmpty())
    {
        servers = parseServers(globalJson);
    }
    if (servers.isEmpty())
    {
        servers = {builtInOfficialDandanplayServer()};
    }

    // The bilibili source is conditional on the Bilibili login state:
    //   signed in  -> the built-in bilibili source is always present
    //                 (auto-attached, enabled) so the user can search it
    //                 without manually adding it per server;
    //   signed out -> the built-in bilibili source is dropped entirely,
    //                 even if it was stored in the global list.
    const bool bilibiliLoggedIn = !ConfigStore::instance()
                                      ->get<QString>(ConfigKeys::BilibiliSessData, QString())
                                      .trimmed()
                                      .isEmpty();
    bool hasBilibili = false;
    for (const DanmakuServerDefinition &server : std::as_const(servers))
    {
        if (isBuiltInBilibiliServer(server))
        {
            hasBilibili = true;
            break;
        }
    }
    if (bilibiliLoggedIn && !hasBilibili)
    {
        DanmakuServerDefinition bili = builtInBilibiliServer();
        bili.enabled = true;
        servers.append(bili);
    }
    if (!bilibiliLoggedIn)
    {
        servers.erase(
            std::remove_if(servers.begin(), servers.end(),
                           [](const DanmakuServerDefinition &s) {
                               return isBuiltInBilibiliServer(s);
                           }),
            servers.end());
    }
    return servers;
}

DanmakuServerDefinition DanmakuSettings::selectedServer(QString serverId)
{
    const QList<DanmakuServerDefinition> servers = loadServers(serverId);
    const QString selectedId = selectedServerId(serverId);
    for (const DanmakuServerDefinition &server : servers)
    {
        if (server.id == selectedId)
        {
            return server;
        }
    }

    const int enabledIndex = firstEnabledServerIndex(servers);
    if (enabledIndex >= 0)
    {
        return servers.at(enabledIndex);
    }
    return servers.isEmpty() ? builtInOfficialDandanplayServer() : servers.first();
}

QString DanmakuSettings::selectedServerId(QString serverId)
{
    const QList<DanmakuServerDefinition> servers = loadServers(serverId);
    if (servers.isEmpty())
    {
        return QString();
    }

    // Global selected server, shared across every Emby server (same
    // globalisation as loadServers).
    const QString globalSelectedId = ConfigStore::instance()->get<QString>(
        QString::fromLatin1(ConfigKeys::DanmakuSelectedServer));
    for (const DanmakuServerDefinition &server : servers)
    {
        if (server.id == globalSelectedId && server.enabled)
        {
            return globalSelectedId;
        }
    }

    const int enabledIndex = firstEnabledServerIndex(servers);
    if (enabledIndex >= 0)
    {
        return servers.at(enabledIndex).id;
    }
    return QString();
}

void DanmakuSettings::saveServers(QString serverId, QList<DanmakuServerDefinition> servers, QString selectedServerId)
{
    // The danmaku source list is shared across ALL Emby servers, so the
    // serverId parameter no longer scopes the storage — it is kept for
    // signature compatibility (UI call sites still pass their active
    // server id). Saving from any server updates the single global list.
    servers = normalizedServers(std::move(servers));
    if (selectedServerId.trimmed().isEmpty())
    {
        const int enabledIndex = firstEnabledServerIndex(servers);
        selectedServerId = enabledIndex >= 0 ? servers.at(enabledIndex).id : QString();
    }

    bool hasSelectedServer = false;
    if (!selectedServerId.trimmed().isEmpty())
    {
        for (const DanmakuServerDefinition &server : servers)
        {
            if (server.id == selectedServerId && server.enabled)
            {
                hasSelectedServer = true;
                break;
            }
        }
    }
    if (!hasSelectedServer)
    {
        const int enabledIndex = firstEnabledServerIndex(servers);
        selectedServerId = enabledIndex >= 0 ? servers.at(enabledIndex).id : QString();
    }

    auto *store = ConfigStore::instance();
    store->set(QString::fromLatin1(ConfigKeys::DanmakuServers),
               QString::fromUtf8(QJsonDocument(toJsonArray(servers)).toJson(QJsonDocument::Compact)));
    store->set(QString::fromLatin1(ConfigKeys::DanmakuSelectedServer), selectedServerId);
}
