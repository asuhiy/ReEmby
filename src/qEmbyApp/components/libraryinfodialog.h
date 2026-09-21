#ifndef LIBRARYINFODIALOG_H
#define LIBRARYINFODIALOG_H

#include "moderndialogbase.h"

#include <services/media/mediaservice.h>

#include <QString>
#include <qcorotask.h>

class QLabel;
class QPushButton;
class QShowEvent;
class QWidget;

class QEmbyCore;

// 「媒体库信息」：显示当前服务器上 电影 / 电视剧 / 剧集 的数量。
//
// 数据来自 MediaService::getLibraryStats()（服务端 /Items/Counts）——
// 只做计数、不返回条目，所以很轻。**不做"去重后"的统计**：那需要把全量
// 条目拉下来才能算（剧集动辄几十万条），成本完全不成比例。
//
// 设计上是**非模态**的，可以开着它继续操作主窗口。
class LibraryInfoDialog : public ModernDialogBase
{
    Q_OBJECT

public:
    // serverId 指向要查询的服务器（服务器列表里的那一行）。留空则查当前
    // 活动服务器 —— 非空是关键：登录页点菜单时可能还没有活动服务器。
    LibraryInfoDialog(QEmbyCore *core, const QString &serverId,
                      QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void setupUi();
    void reload();
    void applyStats(const LibraryStats &stats);
    // valid=false 时显示占位符（未知 / 加载失败）。
    void setValueLabel(QLabel *label, int value, bool valid);

    QEmbyCore *m_core = nullptr;
    QString m_serverId;

    QLabel *m_movieValue = nullptr;
    QLabel *m_seriesValue = nullptr;
    QLabel *m_episodeValue = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_closeButton = nullptr;

    bool m_loaded = false;
    // 「刷新」连点时要丢掉前一次的旧结果。
    quint64 m_generation = 0;
};

#endif  // LIBRARYINFODIALOG_H
