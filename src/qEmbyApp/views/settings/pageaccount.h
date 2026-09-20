#ifndef PAGEACCOUNT_H
#define PAGEACCOUNT_H

#include "settingspagebase.h"
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

class PageAccount : public SettingsPageBase
{
    Q_OBJECT

public:
    explicit PageAccount(QEmbyCore *core, QWidget *parent = nullptr);
    ~PageAccount() override = default;

private slots:
    void onSaveClicked();

private:
    void setBusy(bool busy);
    void revealCurrentPasswordField();

    QLabel *m_accountInfoLabel = nullptr;
    QLineEdit *m_newPasswordEdit = nullptr;
    QLineEdit *m_confirmEdit = nullptr;
    // Fallback for profiles created before password storage existed (or
    // when the stored cipher no longer decrypts): the server rejects the
    // change without CurrentPw, so we reveal this field once.
    QWidget *m_currentPasswordSection = nullptr;
    QLineEdit *m_currentPasswordEdit = nullptr;
    QLabel *m_hintLabel = nullptr;
    QPushButton *m_saveBtn = nullptr;
    bool m_busy = false;
};

#endif // PAGEACCOUNT_H
