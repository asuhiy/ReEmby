#include "authservice.h"
#include <QJsonObject>
#include <QUuid>
#include <QUrl>
#include <qcoronetwork.h>
#include <stdexcept>

namespace {

bool resolveContentDownloadPermission(const QJsonObject& policyObj)
{
    return policyObj["EnableContentDownloading"].toBool(
        policyObj["EnableMediaDownloading"].toBool(true));
}

} 

AuthService::AuthService(NetworkManager* networkManager, ServerManager* serverManager, QObject *parent)
    : QObject(parent), m_networkManager(networkManager), m_serverManager(serverManager)
{
}

QCoro::Task<ServerProfile> AuthService::login(const QString& serverUrl,
                                              const QString& username,
                                              const QString& password,
                                              bool ignoreSslVerification,
                                              const QString& userAgent)
{
    QString cleanUrl = serverUrl;
    if (cleanUrl.endsWith('/')) {
        cleanUrl.chop(1);
    }

    NetworkRequestOptions requestOptions;
    requestOptions.ignoreSslErrors = ignoreSslVerification;

    qDebug() << "[AuthService] Login start"
             << "| url:" << cleanUrl
             << "| ignoreSslVerification:" << ignoreSslVerification;

    // Strict-UA servers reject the very first request, so the public info
    // probe must already carry the resolved UA (explicit or built-in default).
    const QString explicitUa = userAgent.trimmed();
    const QString effectiveUa =
        explicitUa.isEmpty() ? ServerProfile::defaultUserAgent() : explicitUa;
    QMap<QString, QString> infoHeaders;
    infoHeaders.insert("User-Agent", effectiveUa);

    QJsonObject infoResp = co_await m_networkManager->get(
        cleanUrl + "/System/Info/Public", infoHeaders,
        requestOptions);


    ServerProfile tempProfile;
    tempProfile.url = cleanUrl;
    tempProfile.deviceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    tempProfile.ignoreSslVerification = ignoreSslVerification;
    // Persist only the explicitly configured override; the built-in default
    // is applied at request time so future default changes propagate.
    tempProfile.customUserAgent = explicitUa;

    
    QString productName = infoResp["ProductName"].toString();
    if (productName.contains("Jellyfin", Qt::CaseInsensitive)) {
        tempProfile.type = ServerProfile::Jellyfin;
    } else {
        tempProfile.type = ServerProfile::Emby;
    }

    
    QString realName = infoResp["ServerName"].toString();
    if (!realName.isEmpty()) {
        tempProfile.name = realName;
    } else {
        QUrl parsedUrl(cleanUrl);
        tempProfile.name = parsedUrl.host() + (parsedUrl.port() != -1 ? ":" + QString::number(parsedUrl.port()) : "");
    }

    
    ApiClient tempClient(tempProfile, m_networkManager);
    QJsonObject payload;
    payload["Username"] = username;
    payload["Pw"] = password;

    
    QJsonObject authResp = co_await tempClient.post("/Users/AuthenticateByName", payload);

    
    tempProfile.accessToken = authResp["AccessToken"].toString();

    QJsonObject userObj = authResp["User"].toObject();
    tempProfile.userId = userObj["Id"].toString();
    tempProfile.userName = userObj["Name"].toString();

    QJsonObject policyObj = userObj["Policy"].toObject();
    tempProfile.isAdmin = policyObj["IsAdministrator"].toBool();
    tempProfile.canDownloadMedia = resolveContentDownloadPermission(policyObj);

    // Keep an encrypted copy of the login password so the settings account
    // page can supply CurrentPw for self-service password changes.
    tempProfile.setStoredPassword(password);

    
    
    {
        QStringList iconCandidates;
        if (tempProfile.type == ServerProfile::Jellyfin) {
            iconCandidates = {
                QStringLiteral("/web/icon-transparent.png"),
                QStringLiteral("/web/touchicon.png"),
                QStringLiteral("/web/favicon.png"),
                QStringLiteral("/web/favicon.ico"),
            };
        } else {
            iconCandidates = {
                QStringLiteral("/web/touchicon.png"),
                QStringLiteral("/web/touchicon144.png"),
                QStringLiteral("/web/favicon.png"),
                QStringLiteral("/web/favicon.ico"),
                QStringLiteral("/emby/web/touchicon.png"),
                QStringLiteral("/emby/web/favicon.png"),
            };
        }

        qDebug() << "[AuthService] Attempting to fetch server icon"
                 << "| type:" << (tempProfile.type == ServerProfile::Jellyfin ? "Jellyfin" : "Emby")
                 << "| candidates:" << iconCandidates.size();

        for (const QString &iconPath : iconCandidates) {
            try {
                const QByteArray iconBytes = co_await m_networkManager->getBytes(
                    tempProfile.url + iconPath, infoHeaders,
                    requestOptions);
                if (!iconBytes.isEmpty()) {
                    tempProfile.iconBase64 =
                        QString::fromUtf8(iconBytes.toBase64());
                    qDebug() << "[AuthService] Server icon fetched successfully"
                             << "| path:" << iconPath
                             << "| size:" << iconBytes.size() << "bytes";
                    break;
                }
            } catch (const std::exception &e) {
                qDebug() << "[AuthService] Icon fetch failed, trying next"
                         << "| path:" << iconPath
                         << "| error:" << e.what();
            } catch (...) {
                qDebug() << "[AuthService] Icon fetch failed (unknown error)"
                         << "| path:" << iconPath;
            }
        }

        if (tempProfile.iconBase64.isEmpty()) {
            qWarning() << "[AuthService] All icon fetch attempts failed"
                       << "| url:" << tempProfile.url
                       << "| type:" << (tempProfile.type == ServerProfile::Jellyfin ? "Jellyfin" : "Emby");
        }
    }

    
    m_serverManager->addServer(tempProfile);
    m_serverManager->setActiveServer(tempProfile.id);

    
    co_return tempProfile;
}

QCoro::Task<QString> AuthService::testConnection(const QString& serverUrl,
                                                 const QString& username,
                                                 const QString& password,
                                                 bool ignoreSslVerification,
                                                 const QString& userAgent)
{
    // 测试连接 = 复用 login 的可达性 + 凭据校验两步, 但跳过 server manager
    // 写入与 icon 下载. 调用方负责不发生副作用, 用户可放心试用不保存.
    QString cleanUrl = serverUrl;
    if (cleanUrl.endsWith('/')) {
        cleanUrl.chop(1);
    }

    NetworkRequestOptions requestOptions;
    requestOptions.ignoreSslErrors = ignoreSslVerification;

    try {
        // Step 1: 不需要鉴权的服务端公开信息, 验证 URL/SSL/端口可达.
        // 严格 UA 白名单的服务器连首个探测请求都会拦, 所以这里就带解析后的 UA.
        const QString explicitUa = userAgent.trimmed();
        const QString effectiveUa =
            explicitUa.isEmpty() ? ServerProfile::defaultUserAgent() : explicitUa;
        QMap<QString, QString> infoHeaders;
        infoHeaders.insert("User-Agent", effectiveUa);
        const QJsonObject infoResp = co_await m_networkManager->get(
            cleanUrl + QStringLiteral("/System/Info/Public"),
            infoHeaders, requestOptions);

        QString serverName = infoResp.value(QStringLiteral("ServerName")).toString();
        if (serverName.isEmpty()) {
            serverName = infoResp.value(QStringLiteral("Name")).toString();
        }
        if (serverName.isEmpty()) {
            QUrl parsedUrl(cleanUrl);
            serverName = parsedUrl.host().isEmpty()
                             ? QStringLiteral("(unknown)")
                             : parsedUrl.host();
        }

        // Step 2: 可选地验证账号密码. 用户没填账号就跳过这一步
        // (只验可达性, 让用户能验证 URL 对不对后再补账号).
        if (!username.trimmed().isEmpty()) {
            ServerProfile tempProfile;
            tempProfile.url = cleanUrl;
            tempProfile.ignoreSslVerification = ignoreSslVerification;
            tempProfile.deviceId =
                QUuid::createUuid().toString(QUuid::WithoutBraces);
            // Persist only the explicit override; default applies at runtime.
            tempProfile.customUserAgent = explicitUa;

            ApiClient tempClient(tempProfile, m_networkManager);
            QJsonObject payload;
            payload.insert(QStringLiteral("Username"), username.trimmed());
            payload.insert(QStringLiteral("Pw"), password);

            // 单纯验证身份, 不读返回字段
            co_await tempClient.post(QStringLiteral("/Users/AuthenticateByName"),
                                     payload);
        }

        co_return serverName;
    } catch (const std::exception &e) {
        // 重新包装, 让 UI 层 message 跟 login 格式一致
        throw std::runtime_error(std::string("Test connection failed: ") +
                                 e.what());
    } catch (...) {
        throw std::runtime_error("Test connection failed: unknown error");
    }
}

void AuthService::logout()
{
    
    m_serverManager->clearActiveSession();

    
    Q_EMIT userLoggedOut();
}

QCoro::Task<ServerProfile> AuthService::validateSession(const QString& serverId)
{
    
    QList<ServerProfile> servers = m_serverManager->servers();
    ServerProfile targetProfile;
    bool found = false;
    for (const auto& p : servers) {
        if (p.id == serverId) {
            targetProfile = p;
            found = true;
            break;
        }
    }

    if (!found) {
        throw std::runtime_error(tr("本地存储中找不到该服务器配置。").toStdString());
    }

    
    ApiClient tempClient(targetProfile, m_networkManager);

    
    
    co_await tempClient.get("/System/Info");

    try {
        const QJsonObject userResp =
            co_await tempClient.get(QStringLiteral("/Users/%1").arg(targetProfile.userId));
        const QJsonObject policyObj = userResp.value(QStringLiteral("Policy")).toObject();
        targetProfile.userName = userResp.value(QStringLiteral("Name")).toString(
            targetProfile.userName);
        targetProfile.isAdmin = policyObj.value(QStringLiteral("IsAdministrator"))
                                    .toBool(targetProfile.isAdmin);
        targetProfile.canDownloadMedia =
            resolveContentDownloadPermission(policyObj);
        m_serverManager->addServer(targetProfile);
    } catch (const std::exception& e) {
        qWarning() << "[AuthService] Failed to refresh current user policy during"
                      " session validation"
                   << "| userId:" << targetProfile.userId
                   << "| error:" << e.what();
    }

    
    m_serverManager->setActiveServer(targetProfile.id);

    co_return targetProfile;
}
