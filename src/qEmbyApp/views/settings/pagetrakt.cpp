#include "pagetrakt.h"

#include "../../components/elidedlabel.h"
#include "../../components/modernswitch.h"
#include "../../components/settingscard.h"
#include "../../utils/qcoroutil.h"
#include "traktlogindialog.h"
#include <config/config_keys.h>
#include <services/trakt/traktservice.h>

#include <QDateTime>
#include <QDebug>
#include <QFrame>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QWebEngineCookieStore>
#include <QWebEngineProfile>
#include <qcorotimer.h>
#include <stdexcept>

namespace {

QString statusLine(TraktService *service)
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

PageTrakt::PageTrakt(QEmbyCore *core, QWidget *parent)
    : SettingsPageBase(core, tr("Trakt"), parent)
{
    // ---- Account card: sign in / sign out -------------------------------
    m_accountBtn = new QPushButton(tr("Sign In"), this);
    m_accountBtn->setObjectName("SettingsCardButton");
    m_accountBtn->setCursor(Qt::PointingHandCursor);
    m_accountBtn->setFixedHeight(30);

    m_accountCard = new SettingsCard(":/svg/dark/user.svg", tr("Trakt Account"),
                                     statusLine(TraktService::instance()),
                                     m_accountBtn, QString(), this);
    m_mainLayout->addWidget(m_accountCard);

    // ---- Feature switches ------------------------------------------------
    m_mainLayout->addWidget(new SettingsCard(
        ":/svg/dark/player.svg", tr("Real-time Scrobble"),
        tr("Report watch progress to Trakt while playing"),
        new ModernSwitch(this), ConfigKeys::TraktScrobbleEnabled, this,
        QVariant(false)));

    m_mainLayout->addWidget(new SettingsCard(
        ":/svg/dark/heart.svg", tr("Sync Watched Status"),
        tr("Show a Trakt sync button on detail pages"),
        new ModernSwitch(this), ConfigKeys::TraktSyncButtonEnabled, this,
        QVariant(false)));

    m_mainLayout->addWidget(new SettingsCard(
        ":/svg/dark/refresh.svg", tr("Resume Check"),
        tr("Check Trakt progress on playback and offer to resume from it"),
        new ModernSwitch(this), ConfigKeys::TraktResumeCheckEnabled, this,
        QVariant(false)));

    m_mainLayout->addStretch();

    connect(m_accountBtn, &QPushButton::clicked, this, [this]() {
        if (m_loginInProgress) {
            return;
        }
        if (TraktService::instance()->isLoggedIn()) {
            signOut();
        } else {
            launchTask(startLogin(), this);
        }
    });
}

void PageTrakt::refreshAccountUi()
{
    TraktService *service = TraktService::instance();
    const bool loggedIn = service->isLoggedIn();
    m_accountBtn->setText(loggedIn ? tr("Sign Out") : tr("Sign In"));
    const auto labels = m_accountCard->findChildren<ElidedLabel *>("SettingsCardDesc");
    for (ElidedLabel *label : labels) {
        label->setFullText(statusLine(service));
    }
}

void PageTrakt::updateStatusText(const QString &text)
{
    const auto labels = m_accountCard->findChildren<ElidedLabel *>("SettingsCardDesc");
    for (ElidedLabel *label : labels) {
        label->setFullText(text);
    }
}

QCoro::Task<void> PageTrakt::startLogin()
{
    TraktService *service = TraktService::instance();
    m_loginInProgress = true;
    m_accountBtn->setEnabled(false);
    updateStatusText(tr("Requesting device code..."));

    TraktService::DeviceCode code;
    try {
        code = co_await service->requestDeviceCode();
    } catch (const std::exception &e) {
        updateStatusText(tr("Login failed: %1").arg(QString::fromUtf8(e.what())));
        m_loginInProgress = false;
        m_accountBtn->setEnabled(true);
        co_return;
    }

    // Embedded browser with the activation code pre-filled; the user just
    // signs in with their Trakt account and approves.
    const QString activateUrl = code.verificationUrl + QLatin1Char('/')
        + code.userCode;
    QPointer<PageTrakt> guard(this);
    auto *dialog = new TraktLoginDialog(QUrl(activateUrl), this);
    dialog->show();
    updateStatusText(tr("Waiting for approval... (code %1)")
                         .arg(code.userCode));

    const int intervalMs = qMax(2, code.intervalSeconds) * 1000;
    const QDateTime deadline =
        QDateTime::currentDateTime().addSecs(qMax(60, code.expiresInSeconds));
    QTimer waitTimer;
    waitTimer.setSingleShot(true);

    while (QDateTime::currentDateTime() < deadline) {
        if (!guard) {
            co_return;
        }
        if (!dialog->isVisible()) {
            // User closed the login dialog; treat as cancel and unblock
            // the button for the next attempt.
            m_loginInProgress = false;
            m_accountBtn->setEnabled(true);
            refreshAccountUi();
            co_return;
        }

        waitTimer.start(intervalMs);
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

        TraktPollStatus status = TraktPollStatus::Failed;
        try {
            status = co_await service->pollDeviceToken(code.deviceCode);
        } catch (const std::exception &e) {
            qWarning().noquote() << "[Trakt] Device poll error:" << e.what();
        }
        if (status == TraktPollStatus::Approved) {
            break;
        }
        if (status == TraktPollStatus::Expired) {
            updateStatusText(tr("Code expired, please try again"));
            dialog->close();
            m_loginInProgress = false;
            m_accountBtn->setEnabled(true);
            refreshAccountUi();
            co_return;
        }
        if (status == TraktPollStatus::Failed) {
            updateStatusText(tr("Login failed, please try again"));
            dialog->close();
            m_loginInProgress = false;
            m_accountBtn->setEnabled(true);
            refreshAccountUi();
            co_return;
        }
    }

    if (guard) {
        dialog->close();
        m_loginInProgress = false;
        m_accountBtn->setEnabled(true);
        refreshAccountUi();
    }
}

void PageTrakt::signOut()
{
    TraktService::instance()->signOut();
    // Fully sign out: also drop the Trakt session cookies kept by the
    // embedded browser, so the next sign-in asks for credentials again.
    QWebEngineProfile::defaultProfile()->cookieStore()->deleteAllCookies();
    refreshAccountUi();
}
