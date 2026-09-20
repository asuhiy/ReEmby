#ifndef AGGREGATESERVERSDIALOG_H
#define AGGREGATESERVERSDIALOG_H

#include "moderndialogbase.h"

#include <QList>
#include <QString>

class QEmbyCore;
class ModernSwitch;

// "参与聚合的服务"选择对话框（设置 → 媒体库）：
// 列出所有已添加的服务器，各自一个开关控制是否参与聚合搜索 /
// 聚合历史 / 聚合收藏。保存时逐台写入 server/<id>/aggregate_enabled。
class AggregateServersDialog : public ModernDialogBase
{
    Q_OBJECT

public:
    explicit AggregateServersDialog(QEmbyCore *core, QWidget *parent = nullptr);

protected:
    void accept() override; // 保存并关闭

private:
    struct Row
    {
        QString serverId;
        ModernSwitch *toggle = nullptr;
    };

    QEmbyCore *m_core = nullptr;
    QList<Row> m_rows;
};

#endif // AGGREGATESERVERSDIALOG_H
