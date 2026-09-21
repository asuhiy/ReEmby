#ifndef ICONPICKERDIALOG_H
#define ICONPICKERDIALOG_H

#include "moderndialogbase.h"

#include <services/iconpack/iconpackservice.h>

#include <QByteArray>
#include <QList>
#include <QQueue>
#include <QSet>
#include <QString>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QStackedWidget;
class QTimer;
class QShowEvent;
class QResizeEvent;

// 「选择图标」对话框：从图标源里挑一个图标，作为某台服务器的图标。
//
// 图标是**懒加载**的：一个源有 500+ 个图标、单张 2~60KB，一次性拉全部既慢
// 又占内存，所以只加载当前可见区域的格子（滚动 / 改尺寸 / 切源时增量补齐）。
// 网络请求都走 IconPackService —— 它挂了 Qt 的磁盘缓存，重复滚动同一屏不会
// 反复下载，图标源本身也自带 304 复用。
class IconPickerDialog : public ModernDialogBase
{
    Q_OBJECT

public:
    explicit IconPickerDialog(QWidget *parent = nullptr);

    // 确认选中的图标原始字节（供调用方写进 ServerProfile::iconBase64）。
    QByteArray selectedIconData() const { return m_selectedData; }

    // 用户点了「使用默认图标」：调用方应清空 ServerProfile::iconBase64，
    // 让列表回落到内置的 emby / jellyfin 图标。
    bool defaultIconRequested() const { return m_defaultIconRequested; }

protected:
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void setupUi();

    // 重建下拉项（内置 + 自定义）。preferredIndex < 0 时尽量停在原先那个源上。
    void reloadSources(int preferredIndex = -1);
    void loadSource(int index);
    void applyPack(const IconPack &pack);
    // 状态页 = 网格区的第二页，用来显示"加载中 / 获取失败 + 重试"。
    void showStatus(const QString &text, bool withRetry);

    void scheduleIconLoads();
    void requestVisibleIcons();
    void enqueueIcon(int row);
    void pumpIconQueue();
    void onIconDownloaded(int row, quint64 generation, const QByteArray &data);

    void applyFilter(const QString &keyword);

    void onSourceMenuRequested();
    void onAddSourceRequested();
    void onDeleteCurrentSource();
    void onUseDefaultIcon();
    void onConfirmClicked();

    IconPackService *m_service = nullptr;

    QComboBox *m_sourceCombo = nullptr;
    QPushButton *m_addSourceButton = nullptr;
    QPushButton *m_sourceMenuButton = nullptr;
    QLabel *m_descriptionLabel = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QStackedWidget *m_stack = nullptr;
    QListWidget *m_grid = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_retryButton = nullptr;
    QPushButton *m_confirmButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QTimer *m_loadDebounce = nullptr;

    QList<IconPackSource> m_sources;
    int m_currentSource = -1;

    // 切换图标源时自增，用来丢弃上一个源还没回来的网络回调。
    quint64 m_generation = 0;
    // 已经排过队的行号，避免滚动时对同一格重复请求。
    QSet<int> m_requestedRows;
    QQueue<int> m_iconQueue;
    int m_activeLoads = 0;

    QByteArray m_selectedData;
    QString m_selectedUrl;
    bool m_defaultIconRequested = false;
};

#endif  // ICONPICKERDIALOG_H
