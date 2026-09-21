#include "libraryinfodialog.h"

#include "modernmessagebox.h"

#include <qembycore.h>
#include <services/media/mediaservice.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>
#include <qcorotask.h>

namespace {

constexpr int kDialogWidth = 470;
constexpr int kCardHeight = 66;

// 一张统计卡片：左边分类名，右边数字（右对齐）。
QWidget *createStatCard(const QString &title, QLabel **valueOut,
                        QWidget *parent)
{
    auto *card = new QWidget(parent);
    card->setObjectName("library-stat-card");
    card->setAttribute(Qt::WA_StyledBackground, true);
    card->setFixedHeight(kCardHeight);

    auto *layout = new QHBoxLayout(card);
    layout->setContentsMargins(16, 0, 16, 0);
    layout->setSpacing(12);

    auto *titleLabel = new QLabel(title, card);
    titleLabel->setObjectName("library-stat-title");
    layout->addWidget(titleLabel);

    layout->addStretch();

    auto *valueLabel = new QLabel(QStringLiteral("—"), card);
    valueLabel->setObjectName("library-stat-value");
    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(valueLabel);

    if (valueOut) {
        *valueOut = valueLabel;
    }
    return card;
}

}  // namespace

LibraryInfoDialog::LibraryInfoDialog(QEmbyCore *core, QWidget *parent)
    : ModernDialogBase(parent)
    , m_core(core)
{
    setupUi();
}

void LibraryInfoDialog::setupUi()
{
    setTitle(tr("Library Info"));
    setFixedWidth(kDialogWidth);

    QVBoxLayout *root = contentLayout();
    root->setSpacing(8);

    root->addWidget(createStatCard(tr("Movies"), &m_movieValue, this));
    root->addWidget(createStatCard(tr("TV Shows"), &m_seriesValue, this));
    root->addWidget(createStatCard(tr("Episodes"), &m_episodeValue, this));

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName("library-info-status");
    m_statusLabel->setWordWrap(true);
    m_statusLabel->hide();
    root->addWidget(m_statusLabel);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(12);

    // 失败（含未登录）时用它重试，所以留在界面上。
    m_refreshButton = new QPushButton(tr("Refresh"), this);
    m_refreshButton->setObjectName("dialog-btn-cancel");
    m_refreshButton->setMinimumHeight(36);
    m_refreshButton->setCursor(Qt::PointingHandCursor);
    connect(m_refreshButton, &QPushButton::clicked, this,
            &LibraryInfoDialog::reload);
    buttonRow->addWidget(m_refreshButton, 1);

    m_closeButton = new QPushButton(tr("Close"), this);
    m_closeButton->setObjectName("dialog-btn-primary");
    m_closeButton->setMinimumHeight(36);
    m_closeButton->setCursor(Qt::PointingHandCursor);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::reject);
    buttonRow->addWidget(m_closeButton, 1);

    root->addLayout(buttonRow);
}

void LibraryInfoDialog::showEvent(QShowEvent *event)
{
    ModernDialogBase::showEvent(event);

    if (!m_loaded) {
        m_loaded = true;
        reload();
    }
}

void LibraryInfoDialog::reload()
{
    if (!m_core || !m_core->mediaService()) {
        return;
    }

    const quint64 generation = ++m_generation;

    m_statusLabel->setText(tr("Loading..."));
    m_statusLabel->show();
    m_refreshButton->setEnabled(false);
    setValueLabel(m_movieValue, 0, false);
    setValueLabel(m_seriesValue, 0, false);
    setValueLabel(m_episodeValue, 0, false);

    QPointer<LibraryInfoDialog> guard(this);
    QCoro::connect(m_core->mediaService()->getLibraryStats(), this,
                   [guard, generation](const LibraryStats &stats) {
        if (!guard || generation != guard->m_generation) {
            return;
        }
        guard->applyStats(stats);
    });
}

void LibraryInfoDialog::applyStats(const LibraryStats &stats)
{
    m_refreshButton->setEnabled(true);

    if (!stats.succeeded()) {
        // 服务器列表里"从没成功登录过"的条目会走到这里（没有可用 accessToken）。
        m_statusLabel->setText(stats.errorMessage);
        m_statusLabel->show();
        return;
    }

    setValueLabel(m_movieValue, stats.movieCount, true);
    setValueLabel(m_seriesValue, stats.seriesCount, true);
    setValueLabel(m_episodeValue, stats.episodeCount, true);
    m_statusLabel->hide();
}

void LibraryInfoDialog::setValueLabel(QLabel *label, int value, bool valid)
{
    if (!label) {
        return;
    }
    label->setText(valid ? QLocale().toString(value) : QStringLiteral("—"));
}
