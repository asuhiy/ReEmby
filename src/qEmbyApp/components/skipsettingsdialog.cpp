#include "skipsettingsdialog.h"

#include "modernslider.h"

#include <config/config_keys.h>
#include <config/configstore.h>
#include <services/skip/skipsegmentsstore.h>

#include <QDebug>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <cmath>
#include <utility>

namespace
{

QString formatSeconds(int seconds)
{
    seconds = qMax(0, seconds);
    return QStringLiteral("%1:%2")
        .arg(seconds / 60)
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

void styleSlider(ModernSlider *slider)
{
    slider->setFixedHeight(24);
    slider->setMinimumWidth(200);
    slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

} // namespace

SkipSettingsDialog::SkipSettingsDialog(const QString &seriesId,
                                       const QString &itemId,
                                       const QString &scopeName,
                                       std::function<double()> positionProvider,
                                       std::function<double()> durationProvider,
                                       QWidget *parent)
    : PlayerOverlayDialog(parent)
    , m_seriesId(seriesId)
    , m_itemId(itemId)
    , m_scopeName(scopeName)
    , m_positionProvider(std::move(positionProvider))
    , m_durationProvider(std::move(durationProvider))
{
    setSurfaceObjectName("skipSettingsDialog");
    setTitle(tr("Skip Intro & Outro"));
    buildUi();
}

void SkipSettingsDialog::buildUi()
{
    auto *store = SkipSegmentsStore::instance();
    const SkipSegmentsStore::Lengths current = store->resolve(m_seriesId, m_itemId);
    const SkipSegmentsStore::Lengths globalDefaults = store->global();

    auto *box = new QVBoxLayout();
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(12);

    // 单行："用当前位置"取实时播放进度（片头=已播秒数；片尾=剩余秒数）。
    const auto addSliderRow = [this, box](const QString &title, int initialSeconds,
                                          ModernSlider *&sliderOut,
                                          QLabel *&valueOut,
                                          const std::function<int()> &positionValue) {
        auto *titleLabel = new QLabel(title, this);
        titleLabel->setObjectName("skipSettingsSectionTitle");
        box->addWidget(titleLabel);

        auto *row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(12);

        auto *slider = new ModernSlider(Qt::Horizontal, this);
        styleSlider(slider);
        slider->setRange(0, kMaxSeconds);
        slider->setValue(qBound(0, initialSeconds, kMaxSeconds));
        row->addWidget(slider, 1);

        auto *valueLabel = new QLabel(formatSeconds(slider->value()), this);
        valueLabel->setObjectName("skipSettingsValue");
        valueLabel->setMinimumWidth(52);
        valueLabel->setAlignment(Qt::AlignCenter);
        row->addWidget(valueLabel);

        auto *usePositionBtn = new QPushButton(tr("Use Current"), this);
        usePositionBtn->setObjectName("dialog-btn-cancel");
        usePositionBtn->setCursor(Qt::PointingHandCursor);
        row->addWidget(usePositionBtn);

        connect(slider, &QSlider::valueChanged, this,
                [this, valueLabel](int value) {
                    valueLabel->setText(formatSeconds(value));
                    // 手动调整后不再视为"恢复默认"。
                    m_restoreRequested = false;
                });
        connect(usePositionBtn, &QPushButton::clicked, this,
                [slider, positionValue]() {
                    const int seconds = positionValue();
                    if (seconds <= 0) {
                        qInfo() << "[SkipSettingsDialog] no usable playback position";
                        return;
                    }
                    slider->setValue(qBound(0, seconds, kMaxSeconds));
                });

        box->addLayout(row);
        sliderOut = slider;
        valueOut = valueLabel;
    };

    addSliderRow(tr("Intro Length"), current.introSec, m_introSlider,
                 m_introValueLabel, [this]() { return currentPositionSeconds(); });
    addSliderRow(tr("Outro Length"), current.outroSec, m_outroSlider,
                 m_outroValueLabel, [this]() { return remainingSeconds(); });

    auto *scopeHint = new QLabel(this);
    scopeHint->setObjectName("skipSettingsHint");
    scopeHint->setWordWrap(true);
    scopeHint->setText(
        m_seriesId.trimmed().isEmpty()
            ? tr("Applied to the current video. Set 0 to disable skipping.")
            : tr("Applied to all episodes of \"%1\". Set 0 to disable skipping.")
                  .arg(m_scopeName));
    box->addWidget(scopeHint);

    auto *globalHint = new QLabel(this);
    globalHint->setObjectName("skipSettingsHint");
    globalHint->setWordWrap(true);
    globalHint->setText(tr("Global default — intro %1 s · outro %2 s")
                            .arg(globalDefaults.introSec)
                            .arg(globalDefaults.outroSec));
    box->addWidget(globalHint);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setContentsMargins(0, 4, 0, 0);
    buttonRow->setSpacing(10);

    const bool hasOverride =
        (!m_seriesId.trimmed().isEmpty() && store->seriesEntry(m_seriesId).has_value()) ||
        (m_seriesId.trimmed().isEmpty() && store->itemEntry(m_itemId).has_value());
    auto *restoreBtn = new QPushButton(tr("Restore Default"), this);
    restoreBtn->setObjectName("dialog-btn-cancel");
    restoreBtn->setCursor(Qt::PointingHandCursor);
    restoreBtn->setEnabled(hasOverride);
    buttonRow->addWidget(restoreBtn);
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
    contentLayout()->setContentsMargins(20, 8, 20, 18);

    const int contentHeight = box->sizeHint().height();
    setSurfacePreferredSize(QSize(540, contentHeight + 34 + 26));

    connect(cancelBtn, &QPushButton::clicked, this, &PlayerOverlayDialog::reject);
    connect(saveBtn, &QPushButton::clicked, this, &SkipSettingsDialog::saveAndClose);
    connect(restoreBtn, &QPushButton::clicked, this, &SkipSettingsDialog::restoreDefaults);
}

void SkipSettingsDialog::saveAndClose()
{
    auto *store = SkipSegmentsStore::instance();
    const SkipSegmentsStore::Lengths lengths{m_introSlider->value(),
                                             m_outroSlider->value()};

    const bool hasScope = !m_seriesId.trimmed().isEmpty();
    if (m_restoreRequested) {
        if (hasScope) {
            store->clearSeriesEntry(m_seriesId);
        } else {
            store->clearItemEntry(m_itemId);
        }
    } else if (hasScope) {
        store->setSeriesEntry(m_seriesId, lengths);
    } else {
        store->setItemEntry(m_itemId, lengths);
    }

    // 时长 > 0 视为"用户要跳"：确保总开关打开（否则设置了却无效果）。
    auto *config = ConfigStore::instance();
    if (lengths.introSec > 0) {
        config->set(ConfigKeys::PlayerSkipIntro, true);
    }
    if (lengths.outroSec > 0) {
        config->set(ConfigKeys::PlayerSkipOutro, true);
    }

    qInfo() << "[SkipSettingsDialog] saved"
            << "| series:" << m_seriesId
            << "| item:" << m_itemId
            << "| intro:" << lengths.introSec
            << "| outro:" << lengths.outroSec
            << "| restore:" << m_restoreRequested;

    emit settingsSaved();
    accept();
}

void SkipSettingsDialog::restoreDefaults()
{
    const SkipSegmentsStore::Lengths globalDefaults =
        SkipSegmentsStore::instance()->global();
    m_introSlider->setValue(qBound(0, globalDefaults.introSec, kMaxSeconds));
    m_outroSlider->setValue(qBound(0, globalDefaults.outroSec, kMaxSeconds));
    // setValue 会触发 valueChanged（将 m_restoreRequested 置回 false），
    // 因此标志必须在最后设置。
    m_restoreRequested = true;
}

int SkipSettingsDialog::currentPositionSeconds() const
{
    const double position = m_positionProvider ? m_positionProvider() : 0.0;
    if (!std::isfinite(position) || position <= 0.0) {
        return 0;
    }
    return static_cast<int>(std::lround(position));
}

int SkipSettingsDialog::remainingSeconds() const
{
    const double position = m_positionProvider ? m_positionProvider() : 0.0;
    const double duration = m_durationProvider ? m_durationProvider() : 0.0;
    if (!std::isfinite(position) || !std::isfinite(duration) ||
        duration <= 0.0 || position <= 0.0 || position >= duration) {
        return 0;
    }
    return static_cast<int>(std::lround(duration - position));
}
