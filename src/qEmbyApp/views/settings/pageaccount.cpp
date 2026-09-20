#include "pageaccount.h"

#include "../../components/moderntoast.h"
#include "../../utils/qcoroutil.h"
#include "qembycore.h"
#include "services/admin/adminservice.h"
#include "services/manager/servermanager.h"

#include <QHBoxLayout>
#include <QPointer>
#include <stdexcept>

namespace {

QString dashIfEmpty(const QString &value)
{
    return value.isEmpty() ? QStringLiteral("-") : value;
}

} // namespace

PageAccount::PageAccount(QEmbyCore *core, QWidget *parent)
    : SettingsPageBase(core, tr("Account"), parent)
{
    const ServerProfile profile =
        (m_core && m_core->serverManager()) ? m_core->serverManager()->activeProfile()
                                            : ServerProfile();

    m_accountInfoLabel = new QLabel(
        tr("Signed in as %1 on %2")
            .arg(dashIfEmpty(profile.userName), dashIfEmpty(profile.name)),
        this);
    m_accountInfoLabel->setObjectName("SettingsAccountInfo");
    m_mainLayout->addWidget(m_accountInfoLabel);

    auto makeFieldLabel = [this](const QString &text) {
        auto *label = new QLabel(text, this);
        label->setObjectName("SettingsFieldLabel");
        return label;
    };

    m_mainLayout->addSpacing(12);
    m_mainLayout->addWidget(makeFieldLabel(tr("New Password")));
    m_newPasswordEdit = new QLineEdit(this);
    m_newPasswordEdit->setEchoMode(QLineEdit::Password);
    m_newPasswordEdit->setPlaceholderText(tr("Enter the new password"));
    m_newPasswordEdit->setFixedWidth(320);
    m_mainLayout->addWidget(m_newPasswordEdit);

    m_mainLayout->addSpacing(8);
    m_mainLayout->addWidget(makeFieldLabel(tr("Confirm New Password")));
    m_confirmEdit = new QLineEdit(this);
    m_confirmEdit->setEchoMode(QLineEdit::Password);
    m_confirmEdit->setPlaceholderText(tr("Re-enter the new password"));
    m_confirmEdit->setFixedWidth(320);
    m_mainLayout->addWidget(m_confirmEdit);

    // Current password fallback: hidden by default (the stored password
    // supplies CurrentPw automatically); revealed on demand. Label and
    // field live in one container so both show/hide together.
    m_currentPasswordSection = new QWidget(this);
    auto *currentLayout = new QVBoxLayout(m_currentPasswordSection);
    currentLayout->setContentsMargins(0, 0, 0, 0);
    currentLayout->setSpacing(4);
    currentLayout->addWidget(makeFieldLabel(tr("Current Password")));
    m_currentPasswordEdit = new QLineEdit(m_currentPasswordSection);
    m_currentPasswordEdit->setEchoMode(QLineEdit::Password);
    m_currentPasswordEdit->setPlaceholderText(tr("Current password (required once)"));
    m_currentPasswordEdit->setFixedWidth(320);
    currentLayout->addWidget(m_currentPasswordEdit);
    m_currentPasswordSection->setVisible(false);
    m_mainLayout->addSpacing(8);
    m_mainLayout->addWidget(m_currentPasswordSection);

    m_mainLayout->addSpacing(14);
    auto *saveRow = new QWidget(this);
    auto *saveLayout = new QHBoxLayout(saveRow);
    saveLayout->setContentsMargins(0, 0, 0, 0);
    saveLayout->setSpacing(12);
    m_saveBtn = new QPushButton(tr("Save"), saveRow);
    m_saveBtn->setObjectName("SettingsCardButton");
    m_saveBtn->setCursor(Qt::PointingHandCursor);
    m_saveBtn->setFixedHeight(30);
    saveLayout->addWidget(m_saveBtn);
    m_hintLabel = new QLabel(tr("Changing the password does not affect the current sign-in."),
                             saveRow);
    m_hintLabel->setObjectName("SettingsAccountHint");
    saveLayout->addWidget(m_hintLabel, 1);
    m_mainLayout->addWidget(saveRow);

    m_mainLayout->addStretch();

    connect(m_saveBtn, &QPushButton::clicked, this, &PageAccount::onSaveClicked);
    connect(m_newPasswordEdit, &QLineEdit::returnPressed, this, &PageAccount::onSaveClicked);
    connect(m_confirmEdit, &QLineEdit::returnPressed, this, &PageAccount::onSaveClicked);
    connect(m_currentPasswordEdit, &QLineEdit::returnPressed, this,
            &PageAccount::onSaveClicked);
}

void PageAccount::setBusy(bool busy)
{
    m_busy = busy;
    m_saveBtn->setEnabled(!busy);
    m_saveBtn->setText(busy ? tr("Saving...") : tr("Save"));
}

void PageAccount::revealCurrentPasswordField()
{
    if (!m_currentPasswordSection->isVisible()) {
        m_currentPasswordSection->setVisible(true);
        m_currentPasswordEdit->setFocus();
    }
}

void PageAccount::onSaveClicked()
{
    if (m_busy)
        return;

    const ServerProfile profile =
        (m_core && m_core->serverManager()) ? m_core->serverManager()->activeProfile()
                                            : ServerProfile();
    if (!profile.isValid() || profile.userId.isEmpty()) {
        m_hintLabel->setText(tr("No signed-in server."));
        return;
    }

    const QString newPassword = m_newPasswordEdit->text();
    if (newPassword.isEmpty()) {
        m_hintLabel->setText(tr("New password cannot be empty."));
        return;
    }
    if (newPassword != m_confirmEdit->text()) {
        m_hintLabel->setText(tr("The two passwords do not match."));
        return;
    }

    QString currentPassword = profile.storedPasswordPlain();
    if (m_currentPasswordSection->isVisible()) {
        currentPassword = m_currentPasswordEdit->text();
    } else if (currentPassword.isEmpty()) {
        // Older profile without a stored password: the server needs the
        // current password once; reveal the field and ask for it.
        revealCurrentPasswordField();
        m_hintLabel->setText(tr("Please enter the current password once."));
        return;
    }

    setBusy(true);
    m_hintLabel->setText(tr("Saving..."));

    QPointer<PageAccount> guard(this);
    const QString serverId = profile.id;
    const QString userId = profile.userId;

    auto apply = [guard, serverId, newPassword]() {
        if (!guard)
            return;
        // Persist the new password so the next change can supply CurrentPw
        // automatically (updates the active profile copy too).
        guard->m_core->serverManager()->updateServerProfile(
            serverId,
            [&newPassword](ServerProfile &p) { p.setStoredPassword(newPassword); });
        guard->m_newPasswordEdit->clear();
        guard->m_confirmEdit->clear();
        guard->m_currentPasswordEdit->clear();
        guard->m_currentPasswordSection->setVisible(false);
        guard->setBusy(false);
        guard->m_hintLabel->setText(
            guard->tr("Password changed. The current sign-in is not affected."));
        ModernToast::showMessage(guard->tr("Password changed"), 2000);
    };

    auto fail = [guard](const QString &message) {
        if (!guard)
            return;
        guard->setBusy(false);
        guard->m_hintLabel->setText(message);
    };

    launchTask(
        [this, guard, userId, currentPassword, newPassword, apply, fail]()
            -> QCoro::Task<void> {
            try {
                co_await m_core->adminService()->updateUserPassword(
                    userId, currentPassword, newPassword);
                apply();
            } catch (const std::exception &e) {
                if (!guard)
                    co_return;
                const QString raw = QString::fromUtf8(e.what());
                // Timed-out / aborted requests mean the server never answered
                // (some reverse-proxy chains blackhole this endpoint) — a
                // current-password prompt would be misleading.
                const bool noResponse =
                    raw.contains(QStringLiteral("canceled"), Qt::CaseInsensitive)
                    || raw.contains(QStringLiteral("timed out"), Qt::CaseInsensitive)
                    || raw.contains(QStringLiteral("timeout"), Qt::CaseInsensitive)
                    || raw.contains(QStringLiteral("HTTP 0"));
                if (noResponse) {
                    fail(guard->tr("Request timed out: the server did not respond."));
                    co_return;
                }
                // Missing/wrong CurrentPw is the common failure: reveal the
                // field so the user can supply it once.
                if (!guard->m_currentPasswordSection->isVisible()) {
                    guard->revealCurrentPasswordField();
                }
                fail(guard->tr("Change failed: %1").arg(raw));
            } catch (...) {
                fail(PageAccount::tr("Change failed: unknown error"));
            }
            co_return;
        }(),
        this);
}
