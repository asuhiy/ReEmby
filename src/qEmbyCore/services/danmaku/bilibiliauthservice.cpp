#include "bilibiliauthservice.h"

#include "../../config/config_keys.h"
#include "../../config/configstore.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <qcoronetwork.h>
#include <stdexcept>

namespace {

constexpr auto kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";
constexpr auto kReferer = "https://www.bilibili.com/";
constexpr int kRequestTimeoutMs = 15000;

QString cookieValue(const QMap<QString, QString> &cookies, const QString &name)
{
    return cookies.value(name).trimmed();
}

} // namespace

BiliBiliAuthService::BiliBiliAuthService(QObject *parent)
    : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
}

BiliBiliAuthService *BiliBiliAuthService::instance()
{
    static BiliBiliAuthService service;
    return &service;
}

bool BiliBiliAuthService::isLoggedIn() const
{
    return !ConfigStore::instance()
                ->get<QString>(ConfigKeys::BilibiliSessData, QString())
                .trimmed()
                .isEmpty();
}

QString BiliBiliAuthService::userName() const
{
    return ConfigStore::instance()->get<QString>(ConfigKeys::BilibiliUname, QString());
}

QString BiliBiliAuthService::cookieHeader() const
{
    auto *store = ConfigStore::instance();
    QStringList parts;
    const QString buvid3 =
        store->get<QString>(ConfigKeys::BilibiliBuvid3, QString()).trimmed();
    if (!buvid3.isEmpty()) {
        parts.append(QStringLiteral("buvid3=%1").arg(buvid3));
    }
    const QString sessData =
        store->get<QString>(ConfigKeys::BilibiliSessData, QString()).trimmed();
    if (!sessData.isEmpty()) {
        parts.append(QStringLiteral("SESSDATA=%1").arg(sessData));
    }
    const QString jct =
        store->get<QString>(ConfigKeys::BilibiliJct, QString()).trimmed();
    if (!jct.isEmpty()) {
        parts.append(QStringLiteral("bili_jct=%1").arg(jct));
    }
    return parts.join(QStringLiteral("; "));
}

QCoro::Task<void> BiliBiliAuthService::ensureBuvid3()
{
    if (!ConfigStore::instance()
             ->get<QString>(ConfigKeys::BilibiliBuvid3, QString())
             .trimmed()
             .isEmpty()) {
        co_return;
    }

    const RawReply reply = co_await getRaw(QStringLiteral("https://www.bilibili.com/"));
    const QString buvid3 = cookieValue(reply.cookies, QStringLiteral("buvid3"));
    if (!buvid3.isEmpty()) {
        ConfigStore::instance()->set(ConfigKeys::BilibiliBuvid3, buvid3);
        qDebug().noquote() << "[BiliBili] buvid3 acquired";
    }
}

QCoro::Task<QString> BiliBiliAuthService::generateLoginUrl()
{
    co_await ensureBuvid3();
    m_qrcodeKey.clear();

    const RawReply reply = co_await getRaw(
        QStringLiteral("https://passport.bilibili.com/x/passport-login/web/qrcode/generate"));
    if (reply.status != 200) {
        throw std::runtime_error(
            QStringLiteral("Bilibili QR generate failed (HTTP %1)").arg(reply.status).toStdString());
    }
    const QJsonObject body = QJsonDocument::fromJson(reply.body).object();
    const QJsonObject data = body.value(QStringLiteral("data")).toObject();
    const QString qrcodeKey = data.value(QStringLiteral("qrcode_key")).toString();
    const QString url = data.value(QStringLiteral("url")).toString();
    if (qrcodeKey.isEmpty() || url.isEmpty()) {
        throw std::runtime_error("Bilibili QR generate response is invalid");
    }
    m_qrcodeKey = qrcodeKey;
    co_return url;
}

QCoro::Task<int> BiliBiliAuthService::pollLogin()
{
    if (m_qrcodeKey.isEmpty()) {
        co_return -1;
    }

    const RawReply reply = co_await getRaw(
        QStringLiteral("https://passport.bilibili.com/x/passport-login/web/qrcode/poll?qrcode_key=%1")
            .arg(m_qrcodeKey));
    if (reply.status != 200) {
        qWarning().noquote() << "[BiliBili] poll HTTP" << reply.status;
        co_return -1;
    }
    const QJsonObject body = QJsonDocument::fromJson(reply.body).object();
    if (body.value(QStringLiteral("code")).toInt(-1) != 0) {
        co_return -1;
    }
    // Bilibili nests the real login status under data.code:
    //   86101 = not scanned, 86090 = scanned (not confirmed), 86038 = expired,
    //   0 = success. The outer `code:0` just signals the HTTP call worked.
    const int dataCode = body.value(QStringLiteral("data"))
                             .toObject()
                             .value(QStringLiteral("code"))
                             .toInt(-1);
    if (dataCode != 0) {
        co_return dataCode;
    }

    auto *store = ConfigStore::instance();
    const QString sessData = cookieValue(reply.cookies, QStringLiteral("SESSDATA"));
    const QString jct = cookieValue(reply.cookies, QStringLiteral("bili_jct"));
    const QString uid = cookieValue(reply.cookies, QStringLiteral("DedeUserID"));
    if (!sessData.isEmpty()) {
        store->set(ConfigKeys::BilibiliSessData, sessData);
    }
    if (!jct.isEmpty()) {
        store->set(ConfigKeys::BilibiliJct, jct);
    }
    if (!uid.isEmpty()) {
        store->set(ConfigKeys::BilibiliUid, uid);
    }

    // Best-effort profile fetch for the display name.
    const RawReply nav = co_await getRaw(
        QStringLiteral("https://api.bilibili.com/x/web-interface/nav"));
    const QJsonObject navBody = QJsonDocument::fromJson(nav.body).object();
    const QJsonObject navData = navBody.value(QStringLiteral("data")).toObject();
    const QString uname =
        navData.value(QStringLiteral("uname")).toString().trimmed();
    if (!uname.isEmpty()) {
        store->set(ConfigKeys::BilibiliUname, uname);
    }

    qDebug().noquote() << "[BiliBili] QR login success | user:" << uname
                       << "| sessData set:" << !sessData.isEmpty();
    co_return 0;
}

void BiliBiliAuthService::signOut()
{
    auto *store = ConfigStore::instance();
    store->set(ConfigKeys::BilibiliSessData, QString());
    store->set(ConfigKeys::BilibiliJct, QString());
    store->set(ConfigKeys::BilibiliUid, QString());
    store->set(ConfigKeys::BilibiliUname, QString());
    m_qrcodeKey.clear();
    qDebug().noquote() << "[BiliBili] Signed out";
}

void BiliBiliAuthService::onCookieAdded(const QNetworkCookie &cookie)
{
    const QString name = QString::fromLatin1(cookie.name());
    const QString value = QString::fromLatin1(cookie.value());
    if (name.isEmpty() || value.isEmpty()) {
        return;
    }
    const QString domain = cookie.domain();
    if (!domain.contains(QLatin1String("bilibili.com"))) {
        return;
    }
    auto *store = ConfigStore::instance();
    if (name == QLatin1String("SESSDATA")) {
        store->set(ConfigKeys::BilibiliSessData, value);
        qDebug().noquote() << "[BiliBili] SESSDATA captured via WebView";
    } else if (name == QLatin1String("bili_jct")) {
        store->set(ConfigKeys::BilibiliJct, value);
    } else if (name == QLatin1String("DedeUserID")) {
        store->set(ConfigKeys::BilibiliUid, value);
    }
}

QCoro::Task<void> BiliBiliAuthService::fetchProfile()
{
    if (!isLoggedIn()) {
        co_return;
    }
    const RawReply nav = co_await getRaw(
        QStringLiteral("https://api.bilibili.com/x/web-interface/nav"));
    const QJsonObject navBody = QJsonDocument::fromJson(nav.body).object();
    const QJsonObject navData = navBody.value(QStringLiteral("data")).toObject();
    const QString uname =
        navData.value(QStringLiteral("uname")).toString().trimmed();
    if (!uname.isEmpty()) {
        ConfigStore::instance()->set(ConfigKeys::BilibiliUname, uname);
    }
}

QCoro::Task<BiliBiliAuthService::RawReply> BiliBiliAuthService::getRaw(const QString &url)
{
    QNetworkRequest request((QUrl(url)));
    request.setRawHeader("User-Agent", kUserAgent);
    request.setRawHeader("Referer", kReferer);
    request.setRawHeader("Accept", "*/*");
    request.setTransferTimeout(kRequestTimeoutMs);

    QNetworkReply *reply = m_nam->get(request);
    co_await reply;

    RawReply result;
    result.status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.body = reply->readAll();
    const QVariant cookieVariant =
        reply->header(QNetworkRequest::SetCookieHeader);
    const QList<QNetworkCookie> cookies =
        cookieVariant.value<QList<QNetworkCookie>>();
    for (const QNetworkCookie &cookie : cookies) {
        result.cookies.insert(QString::fromLatin1(cookie.name()),
                              QString::fromLatin1(cookie.value()));
    }
    reply->deleteLater();
    co_return result;
}
