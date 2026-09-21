#include "libraryinfodialog.h"

#include "modernmessagebox.h"

#include <models/profile/serverprofile.h>
#include <qembycore.h>
#include <services/manager/servermanager.h>
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

// 只有两列数据（分类名 + 数字），窗口不需要那么宽 —— 原来是 470，
// 分类名和数字一左一右隔着一大片空白，看着很不舒服。
constexpr int kDialogWidth = 360;
constexpr int kCardHeight = 54;
// 卡片内的左右内边距；分类名与数字各占一半宽度（数字在右半区居中）。
constexpr int kCardPadding = 16;

// 一张统计卡片：左边分类名，右边数字（右对齐）。
QWidget *createStatCard(const QString &title, QLabel **valueOut,
                        QWidget *parent)
{
    auto *card = new QWidget(parent);
    card->setObjectName("library-stat-card");
    card->setAttribute(Qt::WA_StyledBackground, true);
    card->setFixedHeight(kCardHeight);

    auto *layout = new QHBoxLayout(card);
    layout->setContentsMargins(kCardPadding, 0, kCardPadding, 0);
    layout->setSpacing(0);

    auto *titleLabel = new QLabel(title, card);
    titleLabel->setObjectName("library-stat-title");
    titleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->addWidget(titleLabel, 1);

    // 数字落在「右半区的中心」（用户选定的方案）：左右各占一半宽度，
    // 右边这个在自身宽度里居中 —— 比贴右边缘看着平衡，数字长短变化时
    // 也不会左右晃。
    auto *valueLabel = new QLabel(QStringLiteral("—"), card);
    valueLabel->setObjectName("library-stat-value");
    valueLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(valueLabel, 1);

    if (valueOut) {
        *valueOut = valueLabel;
    }
    return card;
}

}  // namespace

LibraryInfoDialog::LibraryInfoDialog(QEmbyCore *core, const QString &serverId,
                                     QWidget *parent)
    : ModernDialogBase(parent)
    , m_core(core)
    , m_serverId(serverId)
{
    setupUi();
}

void LibraryInfoDialog::setupUi()
{
    setTitle(tr("Library Info"));
    setFixedWidth(kDialogWidth);

    QVBoxLayout *root = contentLayout();
    root->setSpacing(8);

    // 这些数字完全取决于"查的是哪台服务器"，所以把它的名字标出来 ——
    // 服务器列表里出现重名条目时（用户就有两台都叫"解忧杂货铺"），
    // 不标就无法判断数字属于谁。
    if (m_core && m_core->serverManager()) {
        const QList<ServerProfile> servers = m_core->serverManager()->servers();
        for (const ServerProfile &server : servers) {
            if (server.id != m_serverId) {
                continue;
            }
            auto *nameLabel = new QLabel(server.name, this);
            nameLabel->setObjectName("library-info-server");
            nameLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            root->addWidget(nameLabel);
            break;
        }
    }

    // 数据口径说明：告诉用户数字从哪来、为什么会跟别的客户端对不上，
    // 免得误以为是 bug。窗口窄，用 word wrap 兜住。
    auto *noteLabel = new QLabel(
        tr("Counts come from the server and may differ slightly by how they "
           "are tallied."),
        this);
    noteLabel->setObjectName("library-info-note");
    noteLabel->setWordWrap(true);
    root->addWidget(noteLabel);

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
    QCoro::connect(m_core->mediaService()->getLibraryStats(m_serverId), this,
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
