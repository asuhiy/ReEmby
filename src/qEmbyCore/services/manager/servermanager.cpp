#include "servermanager.h"
#include "../../api/embywebsocket.h"
#include "../../config/configstore.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QStandardPaths>
#include <QDir>
#include <QTimer>
#include <utils/apppaths.h>

ServerManager::ServerManager(NetworkManager* nm, QObject* parent)
    : QObject(parent), m_network(nm) {
    loadSettings();
}

void ServerManager::addServer(const ServerProfile& profile) {
    
    for (int i = 0; i < m_servers.size(); ++i) {
        if (m_servers[i].url == profile.url && m_servers[i].userId == profile.userId) {
            m_servers[i] = profile;
            saveSettings();
            Q_EMIT serversChanged();
            return;
        }
    }
    m_servers.append(profile);
    saveSettings();
    Q_EMIT serversChanged();
}

void ServerManager::removeServer(const QString& id) {
    for (int i = 0; i < m_servers.size(); ++i) {
        if (m_servers[i].id == id) {
            
            m_servers.removeAt(i);

            // 清理该服务器遗留的全部 per-server 配置（server/<id>/ 下的
            // 所有键：首页区块开关、排序记忆、库设置、聚合开关等）——
            // 服务器已删除，其设置不再保留。
            ConfigStore::instance()->removeByPrefix(
                QStringLiteral("server/%1").arg(id));

            
            saveSettings();
            Q_EMIT serversChanged();

            
            if (m_activeProfile.id == id) {

                disconnectWebSocket();

                m_activeProfile = ServerProfile();
                retireActiveClient();

                if (!m_servers.isEmpty()) {
                    setActiveServer(m_servers.first().id);
                } else {

                    Q_EMIT activeServerChanged(m_activeProfile);
                }
            }
            break; 
        }
    }
}

void ServerManager::setActiveServer(const QString& id) {
    for (const auto& profile : m_servers) {
        if (profile.id == id) {
            // Retire the old client with a grace period instead of destroying
            // it synchronously: coroutines all over the app capture the raw
            // ApiClient* returned by activeClient() across co_await suspension
            // points. Dropping the last shared reference right here would
            // delete the client while those coroutines are still in flight,
            // corrupting the heap when they resume and touch the freed object
            // (crash seen when switching servers while playback reporting was
            // still running). 30s comfortably outlives every request timeout
            // (10s) plus a follow-up request.
            QSharedPointer<ApiClient> retired;
            if (m_activeClient && m_activeProfile.id != profile.id) {
                retired = m_activeClient;
            }
            m_activeProfile = profile;
            m_activeClient = QSharedPointer<ApiClient>::create(profile, m_network);
            if (retired) {
                QTimer::singleShot(30000, this, [retired]() mutable {
                    retired.clear();
                });
            }
            Q_EMIT activeServerChanged(m_activeProfile);
            return;
        }
    }
}

void ServerManager::updateServerProxy(const QString& id,
                                      const ProxyConfig& proxy,
                                      bool useGlobalProxy) {
    bool found = false;
    bool isActive = false;
    for (int i = 0; i < m_servers.size(); ++i) {
        if (m_servers[i].id != id) {
            continue;
        }
        
        if (m_servers[i].proxy == proxy &&
            m_servers[i].useGlobalProxy == useGlobalProxy) {
            qDebug() << "[ServerManager] updateServerProxy skipped: no change"
                     << "| id:" << id;
            return;
        }
        m_servers[i].proxy = proxy;
        m_servers[i].useGlobalProxy = useGlobalProxy;

        
        
        if (m_activeProfile.id == id) {
            m_activeProfile = m_servers[i];
            isActive = true;
        }
        found = true;
        break;
    }

    if (!found) {
        qWarning() << "[ServerManager] updateServerProxy: server not found"
                   << "| id:" << id;
        return;
    }

    saveSettings();
    qInfo() << "[ServerManager] proxy updated"
            << "| id:" << id
            << "| useGlobalProxy:" << useGlobalProxy
            << "| proxy:" << proxy.summary();

    Q_EMIT serversChanged();
    Q_EMIT serverProxyChanged(id);
    if (isActive) {
        Q_EMIT activeServerChanged(m_activeProfile);
    }
}

void ServerManager::updateServerProfile(
    const QString& id, const std::function<void(ServerProfile&)>& mutator) {
    bool found = false;
    bool isActive = false;
    for (int i = 0; i < m_servers.size(); ++i) {
        if (m_servers[i].id != id) {
            continue;
        }
        mutator(m_servers[i]);
        if (m_activeProfile.id == id) {
            m_activeProfile = m_servers[i];
            isActive = true;
        }
        found = true;
        break;
    }

    if (!found) {
        qWarning() << "[ServerManager] updateServerProfile: server not found"
                   << "| id:" << id;
        return;
    }

    saveSettings();
    Q_EMIT serversChanged();
    if (isActive) {
        Q_EMIT activeServerChanged(m_activeProfile);
    }
}

void ServerManager::moveServer(const QString& id, int newIndex) {
    int from = -1;
    for (int i = 0; i < m_servers.size(); ++i) {
        if (m_servers[i].id == id) {
            from = i;
            break;
        }
    }
    if (from < 0) {
        qWarning() << "[ServerManager] moveServer: server not found"
                   << "| id:" << id;
        return;
    }

    newIndex = qBound(0, newIndex, m_servers.size() - 1);
    if (newIndex == from) {
        return;
    }

    m_servers.move(from, newIndex);
    saveSettings();
    Q_EMIT serversChanged();
}





void ServerManager::connectWebSocket()
{
    
    if (m_activeProfile.url.isEmpty() || m_activeProfile.accessToken.isEmpty()) return;

    
    if (m_activeWebSocket && m_activeWebSocket->isConnected()) return;

    
    if (m_activeWebSocket) {
        m_activeWebSocket->disconnectFromServer();
        m_activeWebSocket->deleteLater();
        m_activeWebSocket = nullptr;
    }

    
    m_activeWebSocket = new EmbyWebSocket(m_activeProfile, this);
    m_activeWebSocket->connectToServer();
}

void ServerManager::disconnectWebSocket()
{
    if (m_activeWebSocket) {
        m_activeWebSocket->disconnectFromServer();
        m_activeWebSocket->deleteLater();
        m_activeWebSocket = nullptr;
    }
}

EmbyWebSocket* ServerManager::activeWebSocket() const
{
    return m_activeWebSocket;
}





void ServerManager::saveSettings() {
    
    QJsonArray array;
    for (const auto& p : m_servers) {
        QJsonObject obj;
        obj["id"] = p.id;
        obj["name"] = p.name;
        obj["url"] = p.url;
        obj["type"] = (p.type == ServerProfile::Emby ? "Emby" : "Jellyfin");
        obj["ignoreSslVerification"] = p.ignoreSslVerification;
        obj["userId"] = p.userId;
        obj["userName"] = p.userName;
        obj["accessToken"] = p.accessToken;
        obj["deviceId"] = p.deviceId;
        obj["isAdmin"] = p.isAdmin;
        obj["canDownloadMedia"] = p.canDownloadMedia;
        obj["storedPassword"] = p.storedPassword;
        obj["iconBase64"] = p.iconBase64;
        obj["useGlobalProxy"] = p.useGlobalProxy;
        obj["proxy"] = p.proxy.toJson();
        obj["customUserAgent"] = p.customUserAgent;
        array.append(obj);
    }

    QString path = AppPaths::dataRoot();
    QDir().mkpath(path);
    QFile file(path + "/servers.json");
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(array).toJson());
    }
}

void ServerManager::loadSettings() {
    QString path = AppPaths::dataRoot();
    QFile file(path + "/servers.json");
    if (!file.open(QIODevice::ReadOnly)) return;

    QJsonArray array = QJsonDocument::fromJson(file.readAll()).array();
    m_servers.clear();
    for (auto val : array) {
        QJsonObject obj = val.toObject();
        ServerProfile p;
        p.id = obj["id"].toString();
        p.name = obj["name"].toString();
        p.url = obj["url"].toString();
        p.type = (obj["type"].toString() == "Emby" ? ServerProfile::Emby : ServerProfile::Jellyfin);
        p.ignoreSslVerification = obj["ignoreSslVerification"].toBool(false);
        p.userId = obj["userId"].toString();
        p.userName = obj["userName"].toString();
        p.accessToken = obj["accessToken"].toString();
        p.deviceId = obj["deviceId"].toString();
        p.isAdmin = obj["isAdmin"].toBool();
        p.canDownloadMedia = obj["canDownloadMedia"].toBool(false);
        p.storedPassword = obj["storedPassword"].toString();
        p.iconBase64 = obj["iconBase64"].toString();
        p.useGlobalProxy = obj["useGlobalProxy"].toBool(false);
        p.proxy = ProxyConfig::fromJson(obj["proxy"].toObject());
        p.customUserAgent = obj["customUserAgent"].toString();
        m_servers.append(p);
    }

    
    if (!m_servers.isEmpty()) {
        setActiveServer(m_servers.first().id);
    }
}

void ServerManager::clearActiveSession()
{
    disconnectWebSocket();
    m_activeProfile = ServerProfile();
    retireActiveClient();
    Q_EMIT activeServerChanged(m_activeProfile);
}

void ServerManager::retireActiveClient()
{
    // Grace-period retirement: keep the outgoing ApiClient alive for a while
    // so coroutines that captured the raw activeClient() pointer across
    // co_await suspension points finish their in-flight requests against
    // freed memory. See setActiveServer() for details.
    QSharedPointer<ApiClient> retired = m_activeClient;
    m_activeClient.reset();
    if (retired) {
        QTimer::singleShot(30000, this, [retired]() mutable {
            retired.clear();
        });
    }
}

