#include "aggregateserversdialog.h"

#include "modernswitch.h"

#include <config/config_keys.h>
#include <config/configstore.h>
#include <qembycore.h>
#include <services/manager/servermanager.h>

#include <QDebug>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

AggregateServersDialog::AggregateServersDialog(QEmbyCore *core, QWidget *parent)
    : ModernDialogBase(parent)
    , m_core(core)
{
    setTitle(tr("Services in Aggregation"));
    setMinimumWidth(480);

    auto *box = new QVBoxLayout();
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(10);

    auto *hint = new QLabel(
        tr("Choose which servers take part in aggregated search, history and "
           "favorites."),
        this);
    hint->setObjectName("dialog-text");
    hint->setWordWrap(true);
    box->addWidget(hint);

    auto *store = ConfigStore::instance();
    if (m_core && m_core->serverManager()) {
        const QList<ServerProfile> servers = m_core->serverManager()->servers();
        for (const ServerProfile &profile : servers) {
            if (!profile.isValid()) {
                continue;
            }

            auto *rowWidget = new QWidget(this);
            auto *row = new QHBoxLayout(rowWidget);
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(10);

            const QString displayName =
                profile.name.isEmpty() ? profile.url : profile.name;
            const QString labelText =
                profile.userName.isEmpty()
                    ? displayName
                    : QStringLiteral("%1 (%2)").arg(displayName, profile.userName);
            auto *nameLabel = new QLabel(labelText, rowWidget);
            nameLabel->setObjectName("dialog-text");
            row->addWidget(nameLabel, 1);

            auto *toggle = new ModernSwitch(rowWidget);
            toggle->setChecked(store->get<bool>(
                ConfigKeys::forServer(profile.id, ConfigKeys::AggregateEnabled),
                true));
            row->addWidget(toggle);

            box->addWidget(rowWidget);
            m_rows.append({profile.id, toggle});
        }
    }

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setContentsMargins(0, 6, 0, 0);
    buttonRow->setSpacing(10);
    buttonRow->addStretch();

    auto *cancelBtn = new QPushButton(tr("Cancel"), this);
    cancelBtn->setObjectName("dialog-btn-cancel");
    cancelBtn->setCursor(Qt::PointingHandCursor);
    buttonRow->addWidget(cancelBtn);

    auto *saveBtn = new QPushButton(tr("Save"), this);
    saveBtn->setObjectName("dialog-btn-primary");
    saveBtn->setCursor(Qt::PointingHandCursor);
    buttonRow->addWidget(saveBtn);

    box->addLayout(buttonRow);
    contentLayout()->addLayout(box);

    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(saveBtn, &QPushButton::clicked, this, &QDialog::accept);
}

void AggregateServersDialog::accept()
{
    auto *store = ConfigStore::instance();
    for (const Row &row : std::as_const(m_rows)) {
        store->set(
            ConfigKeys::forServer(row.serverId, ConfigKeys::AggregateEnabled),
            row.toggle && row.toggle->isChecked());
    }
    qInfo() << "[AggregateServersDialog] saved"
            << "| servers:" << m_rows.size();
    QDialog::accept();
}
