#include "iconpickerdialog.h"

#include "modernmessagebox.h"
#include "textinputdialog.h"
#include "../managers/thememanager.h"

#include <QAction>
#include <QComboBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>
#include <qcorofuture.h>
#include <qcorotask.h>

#include <utility>

namespace {

// 网格尺寸：对话框内容宽 500，按 94 一格正好排 5 列。
constexpr int kIconSize = 64;
constexpr int kCellWidth = 94;
constexpr int kCellHeight = 104;

// 同时进行的图标下载数。一个源有 500+ 张图，全部并发会把连接数和内存打满；
// 但 4 又明显喂不饱带宽（单张 2–60KB、一屏 20 张），实测是加载慢的主因。
// 8 是在"吃满带宽"和"不把连接数打满"之间取的折中（HTTP/2 已开，见
// IconPackService::buildRequest 的 Http2AllowedAttribute）。
constexpr int kMaxConcurrentIconLoads = 8;

// 滚动时 valueChanged 触发很密，攒一下再算可见区。
constexpr int kLoadDebounceMs = 60;

// 可见区上下各多预取一行，滚动时不会先看到空白再补图。
constexpr int kPrefetchRows = 1;

constexpr int kIconUrlRole = Qt::UserRole + 1;
constexpr int kIconLoadedRole = Qt::UserRole + 2;

QString themeSvgPath(const QString &iconName)
{
    return ThemeManager::instance()->isDarkMode() ? (":/svg/dark/" + iconName)
                                                  : (":/svg/light/" + iconName);
}

}  // namespace

IconPickerDialog::IconPickerDialog(QWidget *parent)
    : ModernDialogBase(parent)
    , m_service(IconPackService::instance())
{
    setupUi();
    reloadSources();
}

void IconPickerDialog::setupUi()
{
    setTitle(tr("Choose Icon"));
    setMinimumSize(480, 520);
    resize(540, 620);

    QVBoxLayout *root = contentLayout();
    root->setSpacing(10);

    // ---------------- 图标源下拉 + 添加入口 + ⋮ ----------------
    auto *sourceRow = new QHBoxLayout();
    sourceRow->setSpacing(8);

    m_sourceCombo = new QComboBox(this);
    m_sourceCombo->setObjectName("icon-source-combo");
    connect(m_sourceCombo, &QComboBox::currentIndexChanged, this,
            [this](int index) { loadSource(index); });
    sourceRow->addWidget(m_sourceCombo, 1);

    m_addSourceButton = new QPushButton(tr("Add Icon Source"), this);
    m_addSourceButton->setObjectName("icon-add-source-btn");
    m_addSourceButton->setCursor(Qt::PointingHandCursor);
    connect(m_addSourceButton, &QPushButton::clicked, this,
            &IconPickerDialog::onAddSourceRequested);
    sourceRow->addWidget(m_addSourceButton);

    m_sourceMenuButton = new QPushButton(this);
    m_sourceMenuButton->setObjectName("icon-source-menu-btn");
    m_sourceMenuButton->setCursor(Qt::PointingHandCursor);
    m_sourceMenuButton->setFixedSize(28, 28);
    m_sourceMenuButton->setToolTip(tr("Icon source options"));
    m_sourceMenuButton->setIcon(QIcon(themeSvgPath(QStringLiteral("more-line.svg"))));
    m_sourceMenuButton->setIconSize(QSize(16, 16));
    connect(m_sourceMenuButton, &QPushButton::clicked, this,
            &IconPickerDialog::onSourceMenuRequested);
    sourceRow->addWidget(m_sourceMenuButton);

    root->addLayout(sourceRow);

    // ---------------- 当前源的说明（图标源 JSON 的 description） ----------------
    m_descriptionLabel = new QLabel(this);
    m_descriptionLabel->setObjectName("icon-source-description");
    m_descriptionLabel->setWordWrap(false);
    root->addWidget(m_descriptionLabel);

    // ---------------- 搜索 ----------------
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setObjectName("icon-search-edit");
    m_searchEdit->setPlaceholderText(tr("Search icons..."));
    m_searchEdit->setClearButtonEnabled(true);
    connect(m_searchEdit, &QLineEdit::textChanged, this,
            [this](const QString &text) { applyFilter(text); });
    root->addWidget(m_searchEdit);

    // ---------------- 网格 / 状态 两页 ----------------
    m_stack = new QStackedWidget(this);
    m_stack->setObjectName("icon-stack");

    m_grid = new QListWidget(m_stack);
    m_grid->setObjectName("icon-grid");
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);
    m_grid->setUniformItemSizes(true);
    // 只对可见区域做布局。默认的 SinglePass 会在每次滚动时重算全部 item 的
    // rect —— 一个源有 500+ 个格子，快速拖到底时这里会造成明显卡顿。
    m_grid->setLayoutMode(QListView::Batched);
    m_grid->setBatchSize(80);
    m_grid->setIconSize(QSize(kIconSize, kIconSize));
    m_grid->setGridSize(QSize(kCellWidth, kCellHeight));
    m_grid->setSpacing(0);
    m_grid->setWordWrap(false);
    m_grid->setTextElideMode(Qt::ElideRight);
    m_grid->setSelectionMode(QAbstractItemView::SingleSelection);
    m_grid->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_grid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    // 滚动条要有自己的 id —— 项目的全局 QScrollBar 只有 2px 手柄、基本看不见，
    // 而且 QSS 必须用 ID 直选器才盖得住全局规则。
    m_grid->verticalScrollBar()->setObjectName("iconGridScrollBar");
    connect(m_grid->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int) { scheduleIconLoads(); });
    connect(m_grid, &QListWidget::itemSelectionChanged, this, [this]() {
        const bool hasSelection = m_grid->currentItem() != nullptr;
        // 选中了具体图标就不再是"使用默认图标"。
        if (hasSelection) {
            m_defaultIconRequested = false;
        }
        // 没选中时"确认"没有意义，先置灰。
        if (m_confirmButton) {
            m_confirmButton->setEnabled(hasSelection);
        }
    });
    connect(m_grid, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *) { onConfirmClicked(); });
    m_stack->addWidget(m_grid);

    auto *statusPage = new QWidget(m_stack);
    statusPage->setObjectName("icon-status-page");
    auto *statusLayout = new QVBoxLayout(statusPage);
    statusLayout->setContentsMargins(24, 24, 24, 24);
    statusLayout->setSpacing(12);
    statusLayout->addStretch();

    m_statusLabel = new QLabel(statusPage);
    m_statusLabel->setObjectName("icon-status-label");
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setWordWrap(true);
    statusLayout->addWidget(m_statusLabel);

    m_retryButton = new QPushButton(tr("Retry"), statusPage);
    m_retryButton->setObjectName("icon-retry-btn");
    m_retryButton->setCursor(Qt::PointingHandCursor);
    connect(m_retryButton, &QPushButton::clicked, this,
            [this]() { loadSource(m_currentSource); });
    statusLayout->addWidget(m_retryButton, 0, Qt::AlignHCenter);

    statusLayout->addStretch();
    m_stack->addWidget(statusPage);

    root->addWidget(m_stack, 1);

    // ---------------- 底部按钮 ----------------
    auto *buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(12);

    // 复用对话框的通用按钮样式（dialog-btn-primary / -cancel），只把高度
    // 抬到和网格相配的尺寸。
    m_confirmButton = new QPushButton(tr("Confirm"), this);
    m_confirmButton->setObjectName("dialog-btn-primary");
    m_confirmButton->setMinimumHeight(36);
    m_confirmButton->setCursor(Qt::PointingHandCursor);
    // 还没选图标 => 置灰（点了也只会弹提示，提前告知）。
    m_confirmButton->setEnabled(false);
    connect(m_confirmButton, &QPushButton::clicked, this,
            &IconPickerDialog::onConfirmClicked);
    buttonRow->addWidget(m_confirmButton, 1);

    m_cancelButton = new QPushButton(tr("Cancel"), this);
    m_cancelButton->setObjectName("dialog-btn-cancel");
    m_cancelButton->setMinimumHeight(36);
    m_cancelButton->setCursor(Qt::PointingHandCursor);
    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    buttonRow->addWidget(m_cancelButton, 1);

    root->addLayout(buttonRow);

    m_loadDebounce = new QTimer(this);
    m_loadDebounce->setSingleShot(true);
    m_loadDebounce->setInterval(kLoadDebounceMs);
    connect(m_loadDebounce, &QTimer::timeout, this,
            &IconPickerDialog::requestVisibleIcons);
}

void IconPickerDialog::showEvent(QShowEvent *event)
{
    ModernDialogBase::showEvent(event);
    // 首帧之后 viewport 才有真实尺寸，这时才算得出可见区。
    scheduleIconLoads();
}

void IconPickerDialog::resizeEvent(QResizeEvent *event)
{
    ModernDialogBase::resizeEvent(event);
    // 宽度变了每行能放几格也就变了，可见区要重算。
    scheduleIconLoads();
}

void IconPickerDialog::reloadSources(int preferredIndex)
{
    const QString previousUrl =
        (m_currentSource >= 0 && m_currentSource < m_sources.size())
            ? m_sources.at(m_currentSource).url
            : QString();

    m_sources = m_service->sources();

    int index = preferredIndex;
    if (index < 0 || index >= m_sources.size()) {
        index = m_sources.isEmpty() ? -1 : 0;
        if (!previousUrl.isEmpty()) {
            for (int i = 0; i < m_sources.size(); ++i) {
                if (m_sources.at(i).url == previousUrl) {
                    index = i;
                    break;
                }
            }
        }
    }

    {
        // currentIndexChanged 会触发 loadSource，这里手动控一次就好。
        const QSignalBlocker blocker(m_sourceCombo);
        m_sourceCombo->clear();
        for (const IconPackSource &source : std::as_const(m_sources)) {
            m_sourceCombo->addItem(source.displayName);
        }
        if (index >= 0) {
            m_sourceCombo->setCurrentIndex(index);
        }
    }

    if (index < 0) {
        m_currentSource = -1;
        showStatus(tr("No icon source available."), false);
        return;
    }

    loadSource(index);
}

void IconPickerDialog::loadSource(int index)
{
    if (index < 0 || index >= m_sources.size()) {
        return;
    }

    m_currentSource = index;
    const quint64 generation = ++m_generation;

    // 换源等于换一批格子：清空网格与懒加载状态。正在飞的旧请求由
    // generation 丢弃（回调里仍会递减 m_activeLoads，见 onIconDownloaded）。
    m_requestedRows.clear();
    m_pendingRows.clear();
    m_selectedData.clear();
    m_selectedUrl.clear();
    m_defaultIconRequested = false;
    m_grid->clear();
    m_descriptionLabel->clear();

    if (!m_searchEdit->text().isEmpty()) {
        const QSignalBlocker blocker(m_searchEdit);
        m_searchEdit->clear();
    }
    if (m_sourceCombo->currentIndex() != index) {
        const QSignalBlocker blocker(m_sourceCombo);
        m_sourceCombo->setCurrentIndex(index);
    }

    showStatus(tr("Loading icon source..."), false);

    QPointer<IconPickerDialog> guard(this);
    QCoro::connect(m_service->loadPack(m_sources.at(index).url), this,
                   [guard, generation](const IconPack &pack) {
        if (!guard || generation != guard->m_generation) {
            return;
        }
        guard->applyPack(pack);
    });
}

void IconPickerDialog::applyPack(const IconPack &pack)
{
    if (!pack.succeeded()) {
        m_descriptionLabel->clear();
        showStatus(pack.errorMessage.isEmpty()
                       ? tr("Failed to load this icon source.")
                       : pack.errorMessage,
                   true);
        return;
    }

    // 自定义源：用 JSON 里的 name 刷新显示名（内置源的名字是固定文案）。
    if (m_currentSource >= 0 && m_currentSource < m_sources.size()) {
        const IconPackSource &source = m_sources.at(m_currentSource);
        if (!source.builtin && !pack.name.isEmpty() &&
            source.displayName != pack.name) {
            m_service->updateCustomSourceName(source.url, pack.name);
            const QSignalBlocker blocker(m_sourceCombo);
            m_sourceCombo->setItemText(m_currentSource, pack.name);
            m_sources[m_currentSource].displayName = pack.name;
        }
    }

    m_descriptionLabel->setText(pack.description);
    m_descriptionLabel->setVisible(!pack.description.isEmpty());

    m_grid->setUpdatesEnabled(false);
    for (const IconPackEntry &entry : pack.entries) {
        auto *item = new QListWidgetItem(entry.name, m_grid);
        item->setData(kIconUrlRole, entry.url);
        item->setData(kIconLoadedRole, false);
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        item->setToolTip(entry.name);
        // 显式给尺寸，setUniformItemSizes 才能生效、滚动条范围才准。
        item->setSizeHint(QSize(kCellWidth, kCellHeight));
    }
    m_grid->setUpdatesEnabled(true);

    // 全量补齐：整个图标源的格子一次性排进队列，可见区稍后由
    // requestVisibleIcons() 插队到最前。这样滚到哪都能立刻出图，不必等
    // "滚到了才开始下"；代价是首次打开会把整源下完（约 28MB）—— 走磁盘
    // 缓存，第二次打开基本免费。
    m_requestedRows.clear();
    m_pendingRows.clear();
    m_pendingRows.reserve(m_grid->count());
    for (int row = 0; row < m_grid->count(); ++row) {
        m_requestedRows.insert(row);
        m_pendingRows.append(row);
    }

    m_stack->setCurrentIndex(0);
    m_grid->scrollToTop();
    scheduleIconLoads();
}

void IconPickerDialog::showStatus(const QString &text, bool withRetry)
{
    m_statusLabel->setText(text);
    m_retryButton->setVisible(withRetry);
    m_stack->setCurrentIndex(1);
}

void IconPickerDialog::scheduleIconLoads()
{
    if (m_loadDebounce) {
        m_loadDebounce->start();
    }
}

void IconPickerDialog::requestVisibleIcons()
{
    if (!m_stack || m_stack->currentIndex() != 0 || m_grid->count() == 0) {
        return;
    }

    const QRect viewport = m_grid->viewport()->rect();
    const QModelIndex topIndex = m_grid->indexAt(viewport.topLeft());
    const QModelIndex bottomIndex =
        m_grid->indexAt(viewport.bottomRight() - QPoint(1, 1));

    int first = topIndex.isValid() ? topIndex.row() : 0;
    int last = bottomIndex.isValid() ? bottomIndex.row() : m_grid->count() - 1;
    if (last < first) {
        last = first;
    }

    const int perRow = qMax(1, viewport.width() / kCellWidth);
    first = qMax(0, first - perRow * kPrefetchRows);
    last = qMin(m_grid->count() - 1, last + perRow * kPrefetchRows);

    // 全量补齐 + 可见优先：队列里本来就排着整个源，这里只把可见区那几行
    // 提到队首，让它们先下载 —— 用户滚到哪，哪就先出图。滚过去的格子
    // 留在队里（不再丢弃），所以不会出现"回头一看还是空白"的情况。
    //
    // 倒序遍历是为了配合 prepend：这样队首最终仍是"从上到下"的顺序。
    for (int row = last; row >= first; --row) {
        if (m_pendingRows.isEmpty()) {
            break;
        }
        QListWidgetItem *item = m_grid->item(row);
        if (!item || item->data(kIconLoadedRole).toBool()) {
            continue;
        }
        const int at = m_pendingRows.indexOf(row);
        if (at > 0) {
            m_pendingRows.move(at, 0);
        } else if (at < 0 && !m_requestedRows.contains(row)) {
            m_requestedRows.insert(row);
            m_pendingRows.prepend(row);
        }
    }

    pumpIconQueue();
}

void IconPickerDialog::pumpIconQueue()
{
    while (m_activeLoads < kMaxConcurrentIconLoads && !m_pendingRows.isEmpty()) {
        // 从队首找第一个真正该下载的：
        //  · 已加载 / 格子已不存在 -> 直接从队列剔除（不再需要）
        //  · 被搜索过滤隐藏的     -> 跳过但留在队列里（清空搜索后还要用）
        int pick = -1;
        for (int i = 0; i < m_pendingRows.size(); ++i) {
            const int candidate = m_pendingRows.at(i);
            QListWidgetItem *candidateItem = m_grid->item(candidate);
            if (!candidateItem || candidateItem->data(kIconLoadedRole).toBool()) {
                m_pendingRows.removeAt(i);
                --i;
                continue;
            }
            if (candidateItem->isHidden()) {
                continue;
            }
            pick = i;
            break;
        }
        if (pick < 0) {
            // 队列里剩下的全被搜索过滤掉了，等搜索条件变化再说。
            break;
        }

        const int row = m_pendingRows.takeAt(pick);
        QListWidgetItem *item = m_grid->item(row);
        const QString url = item->data(kIconUrlRole).toString();
        if (url.isEmpty()) {
            continue;
        }

        ++m_activeLoads;
        const quint64 generation = m_generation;
        QPointer<IconPickerDialog> guard(this);
        QCoro::connect(m_service->downloadIcon(url), this,
                       [guard, row, generation](const QByteArray &data) {
            if (!guard) {
                return;
            }
            guard->onIconDownloaded(row, generation, data);
        });
    }
}

void IconPickerDialog::onIconDownloaded(int row, quint64 generation,
                                        const QByteArray &data)
{
    // 下载回来只是拿到了 PNG 字节，解码 + 缩放不能留在主线程：单张 58KB 的图
    // 解码加平滑缩放要 1–3ms，一屏 20 张就是几十毫秒的阻塞，快速滚动时累起来
    // 就是"界面发涩"。这里照抄项目图片流水线（MediaService 的取图流程）的写法
    // ——QtConcurrent 后台解码，回主线程才转 QPixmap。
    QCoro::connect(decodeIconAsync(row, generation, data), this, []() {});
}

QCoro::Task<void> IconPickerDialog::decodeIconAsync(int row, quint64 generation,
                                                    QByteArray data)
{
    QPointer<IconPickerDialog> guard(this);

    QImage image = co_await QtConcurrent::run([data = std::move(data)]() -> QImage {
        if (data.isEmpty()) {
            return QImage();
        }
        QImage decoded;
        if (!decoded.loadFromData(data)) {
            return QImage();
        }
        // 两阶段缩放：先把大图快速收到 2 倍目标尺寸，再平滑收到目标。
        // 图标原图常见 256×256，直接 SmoothTransformation 缩到 64 是纯浪费
        // （平滑插值的工作量按目标像素数算，先砍掉 3/4 的边长再平滑快得多）。
        constexpr int kPrescale = kIconSize * 2;
        if (decoded.width() > kPrescale || decoded.height() > kPrescale) {
            decoded = decoded.scaled(kPrescale, kPrescale, Qt::KeepAspectRatio,
                                     Qt::FastTransformation);
        }
        return decoded.scaled(kIconSize, kIconSize, Qt::KeepAspectRatio,
                              Qt::SmoothTransformation);
    });

    if (!guard) {
        co_return;  // 对话框已关，别再碰任何成员
    }

    // 必须无条件递减：换源时旧结果会被 generation 挡掉，若只在那时才递减，
    // 计数就会永久泄漏，之后一张图都加载不出来。
    if (m_activeLoads > 0) {
        --m_activeLoads;
    }

    if (generation == m_generation) {
        QListWidgetItem *item = m_grid->item(row);
        if (item) {
            if (!image.isNull()) {
                // QPixmap 只能在 GUI 线程构造，所以转换放在这里而不是后台。
                item->setIcon(QIcon(QPixmap::fromImage(image)));
            }
            // 失败也标记成已处理，避免滚动来回时反复请求同一张坏图。
            item->setData(kIconLoadedRole, true);
        }
    }

    pumpIconQueue();
}

void IconPickerDialog::applyFilter(const QString &keyword)
{
    const QString needle = keyword.trimmed();
    const int count = m_grid->count();
    for (int i = 0; i < count; ++i) {
        QListWidgetItem *item = m_grid->item(i);
        if (!item) {
            continue;
        }
        const bool matched =
            needle.isEmpty() ||
            item->text().contains(needle, Qt::CaseInsensitive);
        item->setHidden(!matched);
    }

    m_grid->scrollToTop();
    scheduleIconLoads();
}

void IconPickerDialog::onSourceMenuRequested()
{
    QMenu menu(this);

    QAction *defaultAction = menu.addAction(tr("Use Default Icon"));
    connect(defaultAction, &QAction::triggered, this,
            &IconPickerDialog::onUseDefaultIcon);

    menu.addSeparator();

    const bool custom =
        m_currentSource >= 0 && m_currentSource < m_sources.size() &&
        !m_sources.at(m_currentSource).builtin;
    QAction *deleteAction = menu.addAction(tr("Delete Icon Source"));
    deleteAction->setEnabled(custom);
    connect(deleteAction, &QAction::triggered, this,
            &IconPickerDialog::onDeleteCurrentSource);

    menu.exec(m_sourceMenuButton->mapToGlobal(
        QPoint(0, m_sourceMenuButton->height())));
}

void IconPickerDialog::onAddSourceRequested()
{
    bool ok = false;
    const QString input = TextInputDialog::getText(
        this, tr("Add Icon Source"), tr("Icon source JSON URL"), QString(),
        QStringLiteral("https://"), tr("Add"), &ok);
    if (!ok) {
        return;
    }

    const QString url = input.trimmed();
    if (url.isEmpty()) {
        return;
    }
    if (!m_service->addCustomSource(url)) {
        ModernMessageBox::warning(
            this, tr("Add Icon Source"),
            tr("This address is invalid, or the source already exists."));
        return;
    }

    // 新源追加在末尾，直接切过去看效果。
    reloadSources(static_cast<int>(m_service->sources().size()) - 1);
}

void IconPickerDialog::onDeleteCurrentSource()
{
    if (m_currentSource < 0 || m_currentSource >= m_sources.size()) {
        return;
    }
    const IconPackSource source = m_sources.at(m_currentSource);
    if (source.builtin) {
        return;
    }

    const bool confirmed = ModernMessageBox::question(
        this, tr("Delete Icon Source"),
        tr("Remove \"%1\" from the icon source list?").arg(source.displayName),
        tr("Delete"), tr("Cancel"), ModernMessageBox::Danger);
    if (!confirmed) {
        return;
    }

    m_service->removeCustomSource(source.url);
    reloadSources(m_currentSource - 1);
}

void IconPickerDialog::onUseDefaultIcon()
{
    m_defaultIconRequested = true;
    m_selectedData.clear();
    m_selectedUrl.clear();
    m_grid->setCurrentItem(nullptr);
    accept();
}

void IconPickerDialog::onConfirmClicked()
{
    if (m_defaultIconRequested) {
        m_selectedData.clear();
        m_selectedUrl.clear();
        accept();
        return;
    }

    QListWidgetItem *item = m_grid->currentItem();
    if (!item || item->isHidden()) {
        ModernMessageBox::information(this, tr("Choose Icon"),
                                      tr("Please pick an icon first."));
        return;
    }

    const QString url = item->data(kIconUrlRole).toString();
    if (url.isEmpty()) {
        accept();
        return;
    }

    // 网格里加载的是缩放后的预览图，写进配置的要是原始字节。
    // 预览那一步已经下过同一张图，这里走磁盘缓存基本瞬时返回。
    m_confirmButton->setEnabled(false);
    const quint64 generation = m_generation;
    QPointer<IconPickerDialog> guard(this);
    QCoro::connect(m_service->downloadIcon(url), this,
                   [guard, generation, url](const QByteArray &data) {
        if (!guard) {
            return;
        }
        guard->m_confirmButton->setEnabled(true);
        if (generation != guard->m_generation) {
            return;
        }
        if (data.isEmpty()) {
            ModernMessageBox::warning(guard, tr("Choose Icon"),
                                      tr("Failed to download this icon."));
            return;
        }
        guard->m_selectedUrl = url;
        guard->m_selectedData = data;
        guard->accept();
    });
}
