#ifndef TRAKTLOGINDIALOG_H
#define TRAKTLOGINDIALOG_H

#include <QDialog>

class QWebEngineView;

// Embedded browser shown during the Trakt device login: loads the Trakt
// activation page with the user code pre-filled (verificationUrl/code), so
// the user just signs in and approves. The dialog is modeless — the caller
// polls for approval and closes it when the flow finishes.
class TraktLoginDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TraktLoginDialog(const QUrl &activateUrl, QWidget *parent = nullptr);

private:
    QWebEngineView *m_view = nullptr;
};

#endif // TRAKTLOGINDIALOG_H
