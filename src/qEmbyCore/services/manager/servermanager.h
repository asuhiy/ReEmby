#ifndef SERVERMANAGER_H
#define SERVERMANAGER_H

#include "../../qEmbyCore_global.h"
#include "../../models/profile/serverprofile.h"
#include "../../api/apiclient.h"
#include "../../api/networkmanager.h"
#include <QObject>
#include <QList>
#include <QSharedPointer>
#include <functional>

class EmbyWebSocket;

class QEMBYCORE_EXPORT ServerManager : public QObject {
    Q_OBJECT
public:
    explicit ServerManager(NetworkManager* nm, QObject* parent = nullptr);

    
    void addServer(const ServerProfile& profile);
    void removeServer(const QString& id);
    void setActiveServer(const QString& id);

    
    NetworkManager* network() const { return m_network; }

    
    
    
    
    void updateServerProxy(const QString& id, const ProxyConfig& proxy,
                           bool useGlobalProxy);

    // Applies an in-place mutation to the server profile matched by id,
    // refreshes the active profile copy when it is the same server, then
    // persists to disk. No-op when no server matches.
    void updateServerProfile(const QString& id,
                             const std::function<void(ServerProfile&)>& mutator);

    // Moves the server matched by id so that it ends up at newIndex.
    // newIndex is clamped to [0, count - 1], so callers can pass a large
    // positive value to mean "move to the end". The list order is what the
    // server picker renders and saveSettings() writes it to disk as-is, so
    // this persists the new order. No-op when the id is unknown or the
    // server already sits at newIndex.
    void moveServer(const QString& id, int newIndex);

    
    QList<ServerProfile> servers() const { return m_servers; }
    ServerProfile activeProfile() const { return m_activeProfile; }
    ApiClient* activeClient() const { return m_activeClient.data(); }

    
    void connectWebSocket();
    void disconnectWebSocket();
    EmbyWebSocket* activeWebSocket() const;

    
    void loadSettings();
    void saveSettings();

    void clearActiveSession();

private:
    // Detaches the current ApiClient with a 30s grace period instead of
    // destroying it synchronously (in-flight coroutines hold raw pointers
    // to it across co_await suspension points).
    void retireActiveClient();

Q_SIGNALS:
    void serversChanged();
    void activeServerChanged(const ServerProfile& profile);

    
    void serverProxyChanged(const QString& serverId);

private:
    NetworkManager* m_network;
    QList<ServerProfile> m_servers;
    ServerProfile m_activeProfile;
    QSharedPointer<ApiClient> m_activeClient; 
    EmbyWebSocket* m_activeWebSocket = nullptr; 
};

#endif
