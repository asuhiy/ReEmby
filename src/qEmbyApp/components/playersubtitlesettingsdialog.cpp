#include "playersubtitlesettingsdialog.h"

#include "moderncombobox.h"
#include "subtitleoptionslider.h"
#include "../managers/thememanager.h"
#include "../utils/subtitleoptionutils.h"

#include <config/config_keys.h>
#include <config/configstore.h>

#include <QComboBox>
#include <QCoreApplication>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace
{

QLabel *createAdaptiveIconLabel(QWidget *parent, const QString &iconPath)
{
    auto *iconLabel = new QLabel(parent);
    iconLabel->setObjectName("playerSubtitleTileIcon");
    iconLabel->setFixedSize(28, 28);
    iconLabel->setAlignment(Qt::AlignCenter);

    auto updateIcon = [iconLabel, iconPath]() {
        iconLabel->setPixmap(
            ThemeManager::getAdaptiveIcon(iconPath).pixmap(14, 14));
    };
    updateIcon();

    QObject::connect(ThemeManager::instance(), &ThemeManager::themeChanged,
                     iconLabel, [updateIcon](ThemeManager::Theme) {
                         updateIcon();
                     });
    return iconLabel;
}

ModernComboBox *createSubtitleFontComboBox(QWidget *parent,
                                           const QString &configKey,
                                           const QString &fallbackFont)
{
    auto *combo = new ModernComboBox(parent);
    combo->setObjectName("playerSubtitleFontCombo");
    combo->setProperty("flush-right-scrollbar", true);
    combo->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    combo->setMinimumWidth(248);
    combo->setMaximumWidth(320);
    combo->setMaxTextWidth(220);
    combo->addItem(QCoreApplication::translate("PlayerSubtitleSettingsDialog",
                                               "Auto"),
                   SubtitleOptionUtils::defaultFontFamily());

    const QStringList families = SubtitleOptionUtils::availableFontFamilies();
    for (const QString &family : families) {
        combo->addItem(family, family);
    }

    // 未单独设置时的回退：主字幕用默认字体、副字幕回退主字幕当前字体。
    const QString resolvedFallback =
        fallbackFont.isEmpty()
            ? SubtitleOptionUtils::defaultFontFamily()
            : SubtitleOptionUtils::normalizeFontFamily(fallbackFont);
    const QString currentFont = SubtitleOptionUtils::normalizeFontFamily(
        ConfigStore::instance()->get<QString>(configKey, resolvedFallback));
    if (combo->findData(currentFont) < 0) {
        combo->addItem(currentFont, currentFont);
    }

    const int currentIndex = combo->findData(currentFont);
    if (currentIndex >= 0) {
        combo->setCurrentIndex(currentIndex);
    }

    auto *store = ConfigStore::instance();
    QObject::connect(combo,
                     QOverload<int>::of(&QComboBox::currentIndexChanged), combo,
                     [combo, store, configKey](int index) {
                         store->set(configKey,
                                    combo->itemData(index).toString());
                     });
    QObject::connect(store, &ConfigStore::valueChanged, combo,
                     [combo, configKey](const QString &key,
                                        const QVariant &newValue) {
                         if (key != configKey || !combo) {
                             return;
                         }

                         const QString resolvedFont =
                             SubtitleOptionUtils::normalizeFontFamily(
                                 newValue.toString());
                         if (combo->findData(resolvedFont) < 0) {
                             combo->addItem(resolvedFont, resolvedFont);
                         }

                         QSignalBlocker blocker(combo);
                         const int index = combo->findData(resolvedFont);
                         if (index >= 0) {
                             combo->setCurrentIndex(index);
                         }
                     });

    return combo;
}

} 

PlayerSubtitleSettingsDialog::PlayerSubtitleSettingsDialog(QWidget *parent)
    : PlayerOverlayDialog(parent)
{
    // 副字幕参数组仅在"启用副字幕"全局开关开启时构建（多出一组参数的高度）。
    const bool secondaryEnabled = ConfigStore::instance()->get<bool>(
        ConfigKeys::PlayerSubtitleSecondaryEnabled, false);

    setSurfaceObjectName("playerSubtitleSettingsDialog");
    setTitle(tr("Subtitle Settings"));

    contentLayout()->setContentsMargins(0, 0, 0, 0);
    contentLayout()->setSpacing(0);

    // 内容放入滚动区：播放窗口较矮时对话框可用高度不足，参数卡片会被压缩
    // 裁切（surface 尺寸被可用空间 clamp）——滚动区让内容保持完整高度，
    // 超出部分滚动查看。surface 高度按内容自适应（见构造末尾）。
    auto *scrollArea = new QScrollArea(this);
    scrollArea->setObjectName("playerSubtitleSettingsScroll");
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->viewport()->setAutoFillBackground(false);

    auto *contentBoxHost = new QWidget(scrollArea);
    contentBoxHost->setObjectName("playerSubtitleSettingsScrollContent");
    auto *contentBox = new QVBoxLayout(contentBoxHost);
    contentBox->setContentsMargins(16, 8, 16, 16);
    contentBox->setSpacing(10);
    scrollArea->setWidget(contentBoxHost);
    contentLayout()->addWidget(scrollArea);

    auto createInfoTile =
        [this](const QString &iconPath, const QString &title,
               const QString &description, QWidget *control,
               const QString &objectName) {
            auto *tile = new QFrame(this);
            tile->setObjectName(objectName);
            tile->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

            auto *layout = new QVBoxLayout(tile);
            layout->setContentsMargins(14, 12, 14, 12);
            layout->setSpacing(8);

            auto *headerRow = new QHBoxLayout();
            headerRow->setContentsMargins(0, 0, 0, 0);
            headerRow->setSpacing(10);

            headerRow->addWidget(createAdaptiveIconLabel(tile, iconPath), 0,
                                 Qt::AlignTop);

            auto *textContainer = new QWidget(tile);
            auto *textLayout = new QVBoxLayout(textContainer);
            textLayout->setContentsMargins(0, 0, 0, 0);
            textLayout->setSpacing(description.isEmpty() ? 0 : 3);

            auto *titleLabel = new QLabel(title, textContainer);
            titleLabel->setObjectName("playerSubtitleTileTitle");
            titleLabel->setWordWrap(true);
            textLayout->addWidget(titleLabel);

            if (!description.isEmpty()) {
                auto *descLabel = new QLabel(description, textContainer);
                descLabel->setObjectName("playerSubtitleTileDesc");
                descLabel->setWordWrap(true);
                textLayout->addWidget(descLabel);
            }

            headerRow->addWidget(textContainer, 1, Qt::AlignVCenter);
            layout->addLayout(headerRow);

            if (control) {
                control->setParent(tile);
                layout->addWidget(control);
            }

            return tile;
        };

    auto createSliderTile =
        [this, &createInfoTile](const QString &iconPath, const QString &title,
                                SubtitleOptionUtils::SliderKind kind,
                                const QString &configKey, int fallbackValue = -1) {
            auto *control =
                new SubtitleOptionSlider(kind, configKey, this, fallbackValue);
            control->setMinimumWidth(0);
            control->setMaximumWidth(QWIDGETSIZE_MAX);
            control->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            return createInfoTile(iconPath, title, QString(), control,
                                  QStringLiteral("playerSubtitleSliderTile"));
        };

    auto *summaryCard = new QFrame(this);
    summaryCard->setObjectName("playerSubtitleSummaryCard");
    auto *summaryLayout = new QVBoxLayout(summaryCard);
    summaryLayout->setContentsMargins(14, 12, 14, 12);
    summaryLayout->setSpacing(0);

    auto *promptLabel = new QLabel(
        tr("Adjust subtitle rendering without leaving playback. Changes are saved and applied immediately."),
        summaryCard);
    promptLabel->setObjectName("playerSubtitleSummaryText");
    promptLabel->setWordWrap(true);
    summaryLayout->addWidget(promptLabel);
    contentBox->addWidget(summaryCard);

    auto *fontTile = new QFrame(this);
    fontTile->setObjectName("playerSubtitleInlineComboTile");
    fontTile->setToolTip(tr("Choose the font family used for subtitle rendering"));
    fontTile->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto *fontLayout = new QHBoxLayout(fontTile);
    fontLayout->setContentsMargins(14, 10, 14, 10);
    fontLayout->setSpacing(10);

    fontLayout->addWidget(createAdaptiveIconLabel(fontTile, ":/svg/dark/subtitle-lang.svg"),
                          0, Qt::AlignVCenter);

    auto *fontTitleLabel = new QLabel(tr("Subtitle Font"), fontTile);
    fontTitleLabel->setObjectName("playerSubtitleTileTitle");
    fontTitleLabel->setWordWrap(false);
    fontTitleLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    fontLayout->addWidget(fontTitleLabel, 0, Qt::AlignVCenter);

    fontLayout->addStretch();

    auto *fontCombo = createSubtitleFontComboBox(
        fontTile, ConfigKeys::PlayerSubtitleFont, QString());
    fontCombo->setToolTip(tr("Choose the font family used for subtitle rendering"));
    fontLayout->addWidget(fontCombo, 0, Qt::AlignVCenter);
    contentBox->addWidget(fontTile);

    auto *sliderGrid = new QGridLayout();
    sliderGrid->setContentsMargins(0, 0, 0, 0);
    sliderGrid->setHorizontalSpacing(10);
    sliderGrid->setVerticalSpacing(10);
    sliderGrid->setColumnStretch(0, 1);
    sliderGrid->setColumnStretch(1, 1);
    sliderGrid->setColumnStretch(2, 1);

    sliderGrid->addWidget(
        createSliderTile(":/svg/dark/danmaku-offset.svg", tr("Subtitle Timing"),
                         SubtitleOptionUtils::SliderKind::DelayMs,
                         ConfigKeys::PlayerSubtitleDelayMs),
        0, 0);
    sliderGrid->addWidget(
        createSliderTile(":/svg/dark/danmaku-size.svg", tr("Subtitle Size"),
                         SubtitleOptionUtils::SliderKind::FontSize,
                         ConfigKeys::PlayerSubtitleFontSize),
        0, 1);
    sliderGrid->addWidget(
        createSliderTile(":/svg/dark/player.svg", tr("Subtitle Position"),
                         SubtitleOptionUtils::SliderKind::Position,
                         ConfigKeys::PlayerSubtitlePosition),
        0, 2);
    sliderGrid->addWidget(
        createSliderTile(":/svg/dark/appearance-font-size.svg",
                         tr("Subtitle Scale"),
                         SubtitleOptionUtils::SliderKind::ScalePercent,
                         ConfigKeys::PlayerSubtitleScale),
        1, 0);
    sliderGrid->addWidget(
        createSliderTile(":/svg/dark/window-player.svg", tr("Outline Size"),
                         SubtitleOptionUtils::SliderKind::OutlineSize,
                         ConfigKeys::PlayerSubtitleOutlineSize),
        1, 1);
    sliderGrid->addWidget(
        createSliderTile(":/svg/dark/direct-stream.svg", tr("Shadow Offset"),
                         SubtitleOptionUtils::SliderKind::ShadowOffset,
                         ConfigKeys::PlayerSubtitleShadowOffset),
        1, 2);
    contentBox->addLayout(sliderGrid);

    // ---- 副字幕参数组（仅在"启用副字幕"全局开关开启时构建）----
    // 只保留 mpv secondary 通道真正支持独立控制的参数：时间/位置/缩放。
    // 字体/字号/描边/阴影跟随主字幕（secondary 通道没有这些属性，写
    // secondary-sub-font 等会报 property not found），故不提供 UI。
    // 每项独立保存，未单独设置时回退主字幕对应值（位置额外上移，见
    // SubtitleOptionUtils::kSecondaryPositionFallbackOffset），与播放侧
    // SubtitleStyleUtils 的回退逻辑保持一致。
    if (secondaryEnabled) {
        auto *secondaryTitle = new QLabel(tr("Secondary Subtitle"), this);
        secondaryTitle->setObjectName("playerSubtitleTileTitle");
        contentBox->addWidget(secondaryTitle);

        // 回退值 = 主字幕当前值（位置额外上移，避免与主字幕重叠）。
        const auto mainSliderValue =
            [](const char *key, SubtitleOptionUtils::SliderKind kind) {
                return ConfigStore::instance()->get<int>(
                    key,
                    SubtitleOptionUtils::sliderSpec(kind).defaultValue);
            };
        const int mainPosition = SubtitleOptionUtils::clampSliderValue(
            SubtitleOptionUtils::SliderKind::Position,
            mainSliderValue(ConfigKeys::PlayerSubtitlePosition,
                            SubtitleOptionUtils::SliderKind::Position));
        const int secondaryPositionFallback = qMax(
            SubtitleOptionUtils::sliderSpec(
                SubtitleOptionUtils::SliderKind::Position)
                .minimum,
            mainPosition -
                SubtitleOptionUtils::kSecondaryPositionFallbackOffset);

        auto *secondaryGrid = new QGridLayout();
        secondaryGrid->setContentsMargins(0, 0, 0, 0);
        secondaryGrid->setHorizontalSpacing(10);
        secondaryGrid->setVerticalSpacing(10);
        secondaryGrid->setColumnStretch(0, 1);
        secondaryGrid->setColumnStretch(1, 1);
        secondaryGrid->setColumnStretch(2, 1);

        secondaryGrid->addWidget(
            createSliderTile(
                ":/svg/dark/danmaku-offset.svg",
                tr("Secondary Subtitle Timing"),
                SubtitleOptionUtils::SliderKind::DelayMs,
                ConfigKeys::PlayerSubtitleSecondaryDelayMs,
                mainSliderValue(ConfigKeys::PlayerSubtitleDelayMs,
                                SubtitleOptionUtils::SliderKind::DelayMs)),
            0, 0);
        secondaryGrid->addWidget(
            createSliderTile(
                ":/svg/dark/player.svg", tr("Secondary Subtitle Position"),
                SubtitleOptionUtils::SliderKind::Position,
                ConfigKeys::PlayerSubtitleSecondaryPosition,
                secondaryPositionFallback),
            0, 1);
        secondaryGrid->addWidget(
            createSliderTile(
                ":/svg/dark/appearance-font-size.svg",
                tr("Secondary Subtitle Scale"),
                SubtitleOptionUtils::SliderKind::ScalePercent,
                ConfigKeys::PlayerSubtitleSecondaryScale,
                mainSliderValue(ConfigKeys::PlayerSubtitleScale,
                                SubtitleOptionUtils::SliderKind::ScalePercent)),
            0, 2);
        contentBox->addLayout(secondaryGrid);
    }

    contentBox->addStretch();

    // 对话框高度按内容自适应（+标题栏与边距）；超出播放窗口可用空间时由
    // updateSurfaceBounds 收缩、内容由滚动区呈现。
    setSurfacePreferredSize(QSize(620, contentBox->sizeHint().height() + 72));
}
