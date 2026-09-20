#ifndef BILIBILILOGINDIALOG_H
#define BILIBILILOGINDIALOG_H

#include <QDialog>
#include <QUrl>

class QWebEngineView;

// Modal-ish WebView that displays the Bilibili QR login page. The page shows
// the QR code; the caller polls BiliBiliAuthService for completion and closes
// this dialog on success/expiry.
class BiliBiliLoginDialog : public QDialog
{
    Q_OBJECT
public:
    explicit BiliBiliLoginDialog(const QUrl &loginUrl, QWidget *parent = nullptr);

private:
    void navigateToLogin();

    QWebEngineView *m_view = nullptr;
    QUrl m_loginUrl;
    bool m_warmedUp = false;
};

#endif // BILIBILILOGINDIALOG_H
