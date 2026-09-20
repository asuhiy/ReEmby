#include "bilibililogindialog.h"

#include <services/danmaku/bilibiliauthservice.h>

#include <QLocale>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEngineCookieStore>
#include <QWebEngineProfile>
#include <QWebEngineView>

BiliBiliLoginDialog::BiliBiliLoginDialog(const QUrl &loginUrl, QWidget *parent)
    : QDialog(parent, Qt::WindowCloseButtonHint | Qt::WindowTitleHint)
    , m_loginUrl(loginUrl)
{
    setWindowTitle(tr("BiliBili 登录"));
    resize(420, 520);

    // Match the qEmby UI language so the BiliBili login page is served in the
    // user's own language instead of defaulting to English.
    const QStringList locales = QLocale::system().uiLanguages();
    if (!locales.isEmpty()) {
        QWebEngineProfile::defaultProfile()->setHttpAcceptLanguage(
            locales.join(QLatin1Char(',')));
    }

    // Capture SESSDATA / bili_jct / DedeUserID when BiliBili's QR-confirm
    // page sets them in the WebView. Qt::UniqueConnection guards against the
    // dialog being recreated without disconnecting.
    connect(QWebEngineProfile::defaultProfile()->cookieStore(),
            &QWebEngineCookieStore::cookieAdded,
            BiliBiliAuthService::instance(),
            &BiliBiliAuthService::onCookieAdded,
            Qt::UniqueConnection);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_view = new QWebEngineView(this);
    layout->addWidget(m_view);

    // Pre-warm the WebView profile by hitting www.bilibili.com first. The
    // scan-confirm page checks for a device-fingerprint cookie (buvid3 /
    // buvid4) and disables its "确认" button if missing. Once the warm-up
    // page has set the cookies we load the real QR login URL.
    connect(m_view, &QWebEngineView::loadFinished, this,
            [this](bool) { navigateToLogin(); });
    m_view->load(QUrl(QStringLiteral("https://www.bilibili.com/")));
}

void BiliBiliLoginDialog::navigateToLogin()
{
    if (m_warmedUp || !m_view) {
        return;
    }
    m_warmedUp = true;
    disconnect(m_view, &QWebEngineView::loadFinished, this, nullptr);
    m_view->load(m_loginUrl);
}
