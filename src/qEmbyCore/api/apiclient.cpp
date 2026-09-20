#include "apiclient.h"

#include <QCoreApplication>
#include <QSysInfo>
#include <utility>

ApiClient::ApiClient(const ServerProfile& profile, NetworkManager* nm, QObject* parent)
    : QObject(parent), m_profile(profile), m_network(nm) {}

QMap<QString, QString> ApiClient::getAuthHeaders() const {
    QMap<QString, QString> headers;

    const QString userAgent = m_profile.effectiveUserAgent();
    if (!userAgent.isEmpty()) {
        const QString clientName = userAgent.section('/', 0, 0).trimmed();
        const QString version = userAgent.section('/', 1).section(' ', 0, 0).trimmed();
        const QString device = QSysInfo::machineHostName();

        QString auth = QString("Emby Client=\"%1\", Device=\"%2\", "
                               "DeviceId=\"%3\", Version=\"%4\"")
                           .arg(clientName.isEmpty() ? QStringLiteral("ReEmby") : clientName,
                                device,
                                m_profile.deviceId,
                                version.isEmpty() ? QStringLiteral("1.0") : version);
        if (!m_profile.accessToken.isEmpty()) {
            auth += QString(", Token=\"%1\"").arg(m_profile.accessToken);
        }
        headers.insert("X-Emby-Authorization", auth);
        headers.insert("User-Agent", userAgent);
        return headers;
    }

    QString auth = QString("MediaBrowser Client=\"ReEmby\", Device=\"%1\", "
                           "DeviceId=\"%2\", Version=\"%3\"")
                       .arg(QSysInfo::machineHostName(),
                            m_profile.deviceId,
                            QCoreApplication::applicationVersion());

    if (!m_profile.accessToken.isEmpty()) {
        auth += QString(", Token=\"%1\"").arg(m_profile.accessToken);
    }

    headers.insert("X-Emby-Authorization", auth);
    return headers;
}

NetworkRequestOptions ApiClient::requestOptions() const {
    NetworkRequestOptions options;
    options.ignoreSslErrors = m_profile.ignoreSslVerification;
    return options;
}

QCoro::Task<QJsonObject> ApiClient::get(const QString& path) {
    QString fullUrl = m_profile.url + path;
    co_return co_await m_network->get(fullUrl, getAuthHeaders(),
                                      requestOptions());
}

QCoro::Task<QJsonObject> ApiClient::get(const QString& path, int timeoutMs) {
    QString fullUrl = m_profile.url + path;
    NetworkRequestOptions options = requestOptions();
    if (timeoutMs > 0)
        options.timeoutMs = timeoutMs;
    co_return co_await m_network->get(fullUrl, getAuthHeaders(), options);
}

QCoro::Task<QString> ApiClient::getText(const QString& path) {
    QString fullUrl = m_profile.url + path;
    co_return co_await m_network->getText(fullUrl, getAuthHeaders(),
                                          requestOptions());
}

QCoro::Task<QJsonObject> ApiClient::post(const QString& path, const QJsonObject& payload,
                                         int timeoutMs) {
    QString fullUrl = m_profile.url + path;
    NetworkRequestOptions options = requestOptions();
    if (timeoutMs > 0)
        options.timeoutMs = timeoutMs;
    co_return co_await m_network->post(fullUrl, getAuthHeaders(), payload, options);
}

QCoro::Task<QJsonObject> ApiClient::postArray(const QString& path, const QJsonArray& payload) {
    QString fullUrl = m_profile.url + path;
    co_return co_await m_network->postArray(fullUrl, getAuthHeaders(), payload,
                                            requestOptions());
}

QCoro::Task<QJsonObject> ApiClient::postBytes(const QString& path,
                                              QByteArray payload,
                                              QString contentType) {
    QString fullUrl = m_profile.url + path;
    co_return co_await m_network->postBytes(fullUrl, getAuthHeaders(),
                                            std::move(payload),
                                            std::move(contentType),
                                            requestOptions());
}

QCoro::Task<QJsonObject> ApiClient::postForm(const QString& path,
                                             const QUrlQuery& formData,
                                             int timeoutMs) {
    QString fullUrl = m_profile.url + path;
    NetworkRequestOptions options = requestOptions();
    if (timeoutMs > 0)
        options.timeoutMs = timeoutMs;
    co_return co_await m_network->postForm(fullUrl, getAuthHeaders(), formData,
                                           options);
}

QCoro::Task<QJsonObject> ApiClient::deleteResource(const QString& path) {
    QString fullUrl = m_profile.url + path;
    co_return co_await m_network->deleteResource(fullUrl, getAuthHeaders(),
                                                 requestOptions());
}
