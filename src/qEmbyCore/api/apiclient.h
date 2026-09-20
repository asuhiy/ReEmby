#ifndef APICLIENT_H
#define APICLIENT_H

#include "../qEmbyCore_global.h"
#include "networkmanager.h"
#include "../models/profile/serverprofile.h"
#include <QByteArray>
#include <qcorotask.h>
#include <QJsonArray>
#include <QUrlQuery>

class QEMBYCORE_EXPORT ApiClient : public QObject {
    Q_OBJECT
public:
    explicit ApiClient(const ServerProfile& profile, NetworkManager* nm, QObject* parent = nullptr);

    // 所属 server profile（SearchAggregator 用于结果回调定位 server）。
    const ServerProfile& profile() const { return m_profile; }

    
    QCoro::Task<QJsonObject> get(const QString& path);
    // Same as get(path) but with an explicit transfer timeout (ms). Used for
    // endpoints that can be slow on the server side (e.g. Recommendations,
    // which scans playback history and computes similarity) so callers can
    // bound the wait and fall back to something cheaper.
    QCoro::Task<QJsonObject> get(const QString& path, int timeoutMs);
    QCoro::Task<QString> getText(const QString& path);
    QCoro::Task<QJsonObject> post(const QString& path, const QJsonObject& payload,
                                  int timeoutMs = 0);
    QCoro::Task<QJsonObject> postArray(const QString& path, const QJsonArray& payload);
    QCoro::Task<QJsonObject> postBytes(const QString& path, QByteArray payload,
                                       QString contentType);
    QCoro::Task<QJsonObject> postForm(const QString& path, const QUrlQuery& formData,
                                      int timeoutMs = 0);
    QCoro::Task<QJsonObject> deleteResource(const QString& path);

private:
    ServerProfile m_profile;
    NetworkManager* m_network;

    
    QMap<QString, QString> getAuthHeaders() const;
    NetworkRequestOptions requestOptions() const;
};

#endif
