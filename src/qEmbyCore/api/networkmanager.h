#ifndef NETWORKMANAGER_H
#define NETWORKMANAGER_H

#include "../qEmbyCore_global.h"
#include <QByteArray>
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonDocument>
#include <QList>
#include <QMap>
#include <QSslError>
#include <QUrlQuery>
#include <qcorotask.h>

struct NetworkRequestOptions {
    bool ignoreSslErrors = false;
    int timeoutMs = 0;
};

struct NetworkJsonGetRequest {
    QString url;
    QMap<QString, QString> headers;
    NetworkRequestOptions options;
};

struct NetworkJsonResult {
    QJsonObject object;
    QString errorMessage;

    bool succeeded() const { return errorMessage.isEmpty(); }
};

class QEMBYCORE_EXPORT NetworkManager : public QObject
{
    Q_OBJECT
public:
    explicit NetworkManager(QObject *parent = nullptr);
    ~NetworkManager();

    static void applyRequestOptions(QNetworkRequest& request,
                                    const NetworkRequestOptions& options);
    static void attachReplyHandlers(QNetworkReply* reply,
                                    const NetworkRequestOptions& options,
                                    const QString& requestKind);
    static QString buildReplyErrorMessage(QNetworkReply* reply,
                                          int httpStatus);

    
    QCoro::Task<QJsonObject> get(const QString& url,
                                 const QMap<QString, QString>& headers,
                                 const NetworkRequestOptions& options = {});
    QCoro::Task<QList<NetworkJsonResult>> getBatch(
        QList<NetworkJsonGetRequest> requests);

    
    QCoro::Task<QString> getText(const QString& url,
                                 const QMap<QString, QString>& headers,
                                 const NetworkRequestOptions& options = {});

    
    QCoro::Task<QByteArray> getBytes(const QString& url,
                                     const QMap<QString, QString>& headers,
                                     const NetworkRequestOptions& options = {});
    QCoro::Task<QByteArray> getBytesLimited(
        QString url,
        QMap<QString, QString> headers,
        qint64 maximumBytes,
        NetworkRequestOptions options = {});

    
    QCoro::Task<QJsonObject> post(const QString& url,
                                  const QMap<QString, QString>& headers,
                                  const QJsonObject& payload,
                                  const NetworkRequestOptions& options = {});
    QCoro::Task<QJsonObject> postArray(const QString& url,
                                       const QMap<QString, QString>& headers,
                                       const QJsonArray& payload,
                                       const NetworkRequestOptions& options = {});
    QCoro::Task<QJsonObject> postBytes(const QString& url,
                                       const QMap<QString, QString>& headers,
                                       QByteArray payload,
                                       QString contentType,
                                       const NetworkRequestOptions& options = {});
    QCoro::Task<QJsonObject> postForm(const QString& url,
                                      const QMap<QString, QString>& headers,
                                      const QUrlQuery& formData,
                                      const NetworkRequestOptions& options = {});

    
    QCoro::Task<QJsonObject> deleteResource(const QString& url,
                                            const QMap<QString, QString>& headers,
                                            const NetworkRequestOptions& options = {});

signals:
    // 服务器不可用（HTTP 5xx，如 504 Gateway Time-out）或请求超时时发出，
    // 供 UI 层弹出全局提示，避免用户把"服务端故障"误判成客户端问题。
    // 内置节流（见 kServerUnavailableThrottleMs），批量请求同时失败时只提示一次。
    void serverUnavailable(int httpStatus);

private:
    QNetworkAccessManager* m_networkManager;

    
    void applyHeaders(QNetworkRequest& request, const QMap<QString, QString>& headers);

    
    QJsonObject parseReply(QNetworkReply* reply);
    QString parseReplyAsText(QNetworkReply* reply);
    QByteArray parseReplyAsBytes(QNetworkReply* reply, const QString& requestKind);

    // 服务器不可用（HTTP 5xx / 请求超时）时发出 serverUnavailable 信号；
    // 内置 30s 节流（跨请求类型共享），批量请求同时失败只提示一次。
    void notifyServerUnavailableIfNeeded(QNetworkReply* reply, int httpStatus);
};

#endif 
