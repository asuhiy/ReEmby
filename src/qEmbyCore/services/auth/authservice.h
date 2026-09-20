#ifndef AUTHSERVICE_H
#define AUTHSERVICE_H

#include "../../qEmbyCore_global.h"
#include "../../api/networkmanager.h"
#include "../../api/apiclient.h"
#include "../manager/servermanager.h"
#include "../../models/profile/serverprofile.h"
#include <QObject>
#include <QString>
#include <qcorotask.h>

class QEMBYCORE_EXPORT AuthService : public QObject
{
    Q_OBJECT
public:
    explicit AuthService(NetworkManager* networkManager, ServerManager* serverManager, QObject *parent = nullptr);

    
    
    

    
    QCoro::Task<ServerProfile> login(const QString& serverUrl,
                                     const QString& username,
                                     const QString& password,
                                     bool ignoreSslVerification,
                                     const QString& userAgent = QString());

    // 验证服务器可达 + 可选地验证凭据; 不写 server manager, 不下载 icon.
    // 成功返回服务端 ServerName; 失败抛 std::runtime_error.
    QCoro::Task<QString> testConnection(const QString& serverUrl,
                                        const QString& username,
                                        const QString& password,
                                        bool ignoreSslVerification,
                                        const QString& userAgent = QString());

    QCoro::Task<ServerProfile> validateSession(const QString& serverId);

    
    void logout();

Q_SIGNALS:
    
    void userLoggedOut();

private:
    NetworkManager* m_networkManager;
    ServerManager* m_serverManager;
};

#endif 
