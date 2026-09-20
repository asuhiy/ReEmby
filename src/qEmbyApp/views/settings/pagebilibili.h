#ifndef PAGEBILIBILI_H
#define PAGEBILIBILI_H

#include "settingspagebase.h"
#include <qcorotask.h>

class QPushButton;
class QFrame;

class PageBilibili : public SettingsPageBase
{
    Q_OBJECT
public:
    PageBilibili(QEmbyCore *core, QWidget *parent = nullptr);

private:
    void refreshAccountUi();
    void updateStatusText(const QString &text);
    QCoro::Task<void> startLogin();
    void signOut();

    QPushButton *m_accountBtn = nullptr;
    QFrame *m_accountCard = nullptr;
    bool m_loginInProgress = false;
};

#endif // PAGEBILIBILI_H
