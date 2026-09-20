#include "pagebilibili.h"

#include "../../components/elidedlabel.h"
#include "../../components/settingscard.h"
#include "../../utils/qcoroutil.h"
#include "bilibililogindialog.h"
#include <services/danmaku/bilibiliauthservice.h>

#include <QDateTime>
#include <QDebug>
#include <QFrame>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <qcorotimer.h>
#include <stdexcept>

namespace {

QString statusLine(BiliBiliAuthService *service)
{
    if (service->isLoggedIn()) {
        const QString user = service->userName();
        return user.isEmpty()
                   ? QObject::tr("Signed in")
                   : QObject::tr("Signed in as %1").arg(user);
    }
    return QObject::tr("Not signed in");
}

} // namespace

PageBilibili::PageBilibili(QEmbyCore *core, QWidget *parent)
    : SettingsPageBase(core, tr("BiliBili"), parent)
{
    m_accountBtn = new QPushButton(tr("Scan to Sign In"), this);
    m_accountBtn->setObjectName("SettingsCardButton");
    m_accountBtn->setCursor(Qt::PointingHandCursor);
    m_accountBtn->setFixedHeight(30);

    m_accountCard = new SettingsCard(":/svg/dark/user.svg", tr("BiliBili Account"),
                                     statusLine(BiliBiliAuthService::instance()),
                                     m_accountBtn, QString(), this);
    m_mainLayout->addWidget(m_accountCard);
    m_mainLayout->addStretch();

    refreshAccountUi();

    connect(m_accountBtn, &QPushButton::clicked, this, [this]() {
        if (m_loginInProgress) {
            return;
        }
        if (BiliBiliAuthService::instance()->isLoggedIn()) {
            signOut();
        } else {
            launchTask(startLogin(), this);
        }
    });
}

void PageBilibili::refreshAccountUi()
{
    BiliBiliAuthService *service = BiliBiliAuthService::instance();
    const bool loggedIn = service->isLoggedIn();
    m_accountBtn->setText(loggedIn ? tr("Sign Out") : tr("Scan to Sign In"));
    const auto labels = m_accountCard->findChildren<ElidedLabel *>("SettingsCardDesc");
    for (ElidedLabel *label : labels) {
        label->setFullText(statusLine(service));
    }
}

void PageBilibili::updateStatusText(const QString &text)
{
    const auto labels = m_accountCard->findChildren<ElidedLabel *>("SettingsCardDesc");
    for (ElidedLabel *label : labels) {
        label->setFullText(text);
    }
}

QCoro::Task<void> PageBilibili::startLogin()
{
    BiliBiliAuthService *service = BiliBiliAuthService::instance();
    m_loginInProgress = true;
    m_accountBtn->setEnabled(false);
    updateStatusText(tr("Generating QR code..."));

    QString loginUrl;
    try {
        loginUrl = co_await service->generateLoginUrl();
    } catch (const std::exception &e) {
        updateStatusText(tr("Login failed: %1").arg(QString::fromUtf8(e.what())));
        m_loginInProgress = false;
        m_accountBtn->setEnabled(true);
        co_return;
    }

    QPointer<PageBilibili> guard(this);
    auto *dialog = new BiliBiliLoginDialog(QUrl(loginUrl), this);
    dialog->show();
    updateStatusText(tr("Scan the QR code with the BiliBili app..."));

    QTimer waitTimer;
    waitTimer.setSingleShot(true);

    while (true) {
        if (!guard) {
            co_return;
        }
        if (!dialog->isVisible()) {
            // The user closed the login dialog manually; treat as cancel
            // and unblock the button for the next attempt.
            m_loginInProgress = false;
            m_accountBtn->setEnabled(true);
            refreshAccountUi();
            co_return;
        }

        waitTimer.start(2000);
        co_await waitTimer;
        if (!guard) {
            co_return;
        }
        if (!dialog->isVisible()) {
            m_loginInProgress = false;
            m_accountBtn->setEnabled(true);
            refreshAccountUi();
            co_return;
        }

        int code = -1;
        try {
            code = co_await service->pollLogin();
        } catch (const std::exception &e) {
            qWarning().noquote() << "[BiliBili] QR poll error:" << e.what();
        }

        // The SESSDATA cookie is delivered to the WebView profile, not our
        // m_nam, so cookieAdded -> onCookieAdded stores it asynchronously.
        // When the cookies are in place the login is complete even if the
        // poll has not yet flipped to code 0.
        if (code == 0 || service->isLoggedIn()) {
            // Make sure the display name is fetched: pollLogin only does it
            // inside its data.code==0 branch, but the cookie-first path may
            // break out before that branch runs.
            try {
                co_await service->fetchProfile();
            } catch (const std::exception &e) {
                qWarning().noquote() << "[BiliBili] fetchProfile error:" << e.what();
            }
            break;
        }
        if (code == 86038) { // expired
            updateStatusText(tr("QR code expired, please try again"));
            dialog->close();
            m_loginInProgress = false;
            m_accountBtn->setEnabled(true);
            refreshAccountUi();
            co_return;
        }
        if (code < 0 && !service->isLoggedIn()) {
            // network/parse failure, and cookies still missing
            updateStatusText(tr("Login failed, please try again"));
            dialog->close();
            m_loginInProgress = false;
            m_accountBtn->setEnabled(true);
            refreshAccountUi();
            co_return;
        }
        // 86101 (not scanned) / 86090 (scanned not confirmed): keep polling.
    }

    if (guard) {
        dialog->close();
        m_loginInProgress = false;
        m_accountBtn->setEnabled(true);
        refreshAccountUi();
    }
}

void PageBilibili::signOut()
{
    BiliBiliAuthService::instance()->signOut();
    refreshAccountUi();
}
