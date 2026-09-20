#include "traktlogindialog.h"

#include <QLocale>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEngineProfile>
#include <QWebEngineView>

TraktLoginDialog::TraktLoginDialog(const QUrl &activateUrl, QWidget *parent)
    : QDialog(parent, Qt::WindowCloseButtonHint | Qt::WindowTitleHint)
{
    setWindowTitle(tr("Trakt Authorization"));
    resize(520, 720);

    // Match the qEmby UI language so the Trakt login page is served in the
    // user's own language instead of defaulting to English.
    const QStringList locales = QLocale::system().uiLanguages();
    if (!locales.isEmpty()) {
        QWebEngineProfile::defaultProfile()->setHttpAcceptLanguage(
            locales.join(QLatin1Char(',')));
    }

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_view = new QWebEngineView(this);
    layout->addWidget(m_view);
    m_view->load(activateUrl);
}
