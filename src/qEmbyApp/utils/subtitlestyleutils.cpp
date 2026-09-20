#include "subtitlestyleutils.h"

#include "../components/mpvcontroller.h"
#include "subtitleoptionutils.h"

#include <QDebug>
#include <config/config_keys.h>
#include <config/configstore.h>

namespace SubtitleStyleUtils {

namespace {

int readConfigInt(const QString &key, int fallback)
{
    const QVariant value =
        ConfigStore::instance()->get<QVariant>(key, QVariant(fallback));

    bool ok = false;
    const int result = value.toInt(&ok);
    if (ok) {
        return result;
    }

    const int parsed = value.toString().trimmed().toInt(&ok);
    return ok ? parsed : fallback;
}

QString readSubtitleFontFamily()
{
    return SubtitleOptionUtils::normalizeFontFamily(
        ConfigStore::instance()->get<QString>(
            ConfigKeys::PlayerSubtitleFont,
            SubtitleOptionUtils::defaultFontFamily()));
}

// 副字幕未单独设置位置时的回退：主字幕位置上移（偏移量见
// SubtitleOptionUtils::kSecondaryPositionFallbackOffset，数值更小 = 更靠画面
// 上部），避免两条字幕叠在同一条线上。位置合法范围为 0-100。
constexpr int kSecondaryPositionFallbackMin = 0;

} 

bool isSubtitleStyleKey(const QString &key)
{
    return key == QLatin1String(ConfigKeys::PlayerSubtitleFont) ||
           key == QLatin1String(ConfigKeys::PlayerSubtitleDelayMs) ||
           key == QLatin1String(ConfigKeys::PlayerSubtitleFontSize) ||
           key == QLatin1String(ConfigKeys::PlayerSubtitlePosition) ||
           key == QLatin1String(ConfigKeys::PlayerSubtitleOutlineSize) ||
           key == QLatin1String(ConfigKeys::PlayerSubtitleShadowOffset) ||
           key == QLatin1String(ConfigKeys::PlayerSubtitleScale) ||
           key == QLatin1String(ConfigKeys::PlayerSubtitleSecondaryEnabled) ||
           key == QLatin1String(ConfigKeys::PlayerSubtitleSecondaryFont) ||
           key == QLatin1String(ConfigKeys::PlayerSubtitleSecondaryDelayMs) ||
           key == QLatin1String(ConfigKeys::PlayerSubtitleSecondaryFontSize) ||
           key == QLatin1String(ConfigKeys::PlayerSubtitleSecondaryPosition) ||
           key ==
               QLatin1String(ConfigKeys::PlayerSubtitleSecondaryOutlineSize) ||
           key ==
               QLatin1String(ConfigKeys::PlayerSubtitleSecondaryShadowOffset) ||
           key == QLatin1String(ConfigKeys::PlayerSubtitleSecondaryScale);
}

void applyToController(MpvController *controller,
                       bool protectPrimarySubtitleForDanmaku)
{
    if (!controller) {
        return;
    }

    const int delayMs = SubtitleOptionUtils::clampSliderValue(
        SubtitleOptionUtils::SliderKind::DelayMs,
        readConfigInt(
            QLatin1String(ConfigKeys::PlayerSubtitleDelayMs),
            SubtitleOptionUtils::sliderSpec(
                SubtitleOptionUtils::SliderKind::DelayMs)
                .defaultValue));
    const int fontSize = SubtitleOptionUtils::clampSliderValue(
        SubtitleOptionUtils::SliderKind::FontSize,
        readConfigInt(
            QLatin1String(ConfigKeys::PlayerSubtitleFontSize),
            SubtitleOptionUtils::sliderSpec(
                SubtitleOptionUtils::SliderKind::FontSize)
                .defaultValue));
    const int position = SubtitleOptionUtils::clampSliderValue(
        SubtitleOptionUtils::SliderKind::Position,
        readConfigInt(
            QLatin1String(ConfigKeys::PlayerSubtitlePosition),
            SubtitleOptionUtils::sliderSpec(
                SubtitleOptionUtils::SliderKind::Position)
                .defaultValue));
    const int outlineSize = SubtitleOptionUtils::clampSliderValue(
        SubtitleOptionUtils::SliderKind::OutlineSize,
        readConfigInt(
            QLatin1String(ConfigKeys::PlayerSubtitleOutlineSize),
            SubtitleOptionUtils::sliderSpec(
                SubtitleOptionUtils::SliderKind::OutlineSize)
                .defaultValue));
    const int shadowOffset = SubtitleOptionUtils::clampSliderValue(
        SubtitleOptionUtils::SliderKind::ShadowOffset,
        readConfigInt(
            QLatin1String(ConfigKeys::PlayerSubtitleShadowOffset),
            SubtitleOptionUtils::sliderSpec(
                SubtitleOptionUtils::SliderKind::ShadowOffset)
                .defaultValue));
    const int scalePercent = SubtitleOptionUtils::clampSliderValue(
        SubtitleOptionUtils::SliderKind::ScalePercent,
        readConfigInt(
            QLatin1String(ConfigKeys::PlayerSubtitleScale),
            SubtitleOptionUtils::sliderSpec(
                SubtitleOptionUtils::SliderKind::ScalePercent)
                .defaultValue));

    const QString fontFamily = readSubtitleFontFamily();
    const double delaySeconds = delayMs / 1000.0;
    const double outlinePixels = outlineSize / 10.0;
    const double shadowPixels = shadowOffset / 10.0;
    const double scaleFactor = scalePercent / 100.0;

    // 副字幕参数：延迟/位置/缩放独立读取，未单独设置时回退主字幕对应值；
    // 位置额外上移 kSecondaryPositionOffset，避免两条字幕叠在同一条线上。
    //
    // 注意：mpv 的 secondary 通道只有 secondary-sub-{scale,pos,delay,
    // ass-override,visibility} 这几个属性——字体/字号/描边/阴影**没有**独立
    // 参数（写 "secondary-sub-font" 等会报 property not found），副字幕的
    // 这些样式跟随主字幕的 sub-* 设置，属 mpv 内置限制。
    const int secondaryDelayMs = readConfigInt(
        QLatin1String(ConfigKeys::PlayerSubtitleSecondaryDelayMs), delayMs);
    const int secondaryPosition = readConfigInt(
        QLatin1String(ConfigKeys::PlayerSubtitleSecondaryPosition),
        qMax(kSecondaryPositionFallbackMin,
             position - SubtitleOptionUtils::kSecondaryPositionFallbackOffset));
    const int secondaryScalePercent = readConfigInt(
        QLatin1String(ConfigKeys::PlayerSubtitleSecondaryScale), scalePercent);

    // ass-track 弹幕模式（protectPrimarySubtitleForDanmaku）下弹幕占用 sid，
    // secondary 通道承载的其实是内容主字幕——此时 secondary-sub-* 必须使用
    // 主字幕参数，否则内容字幕会套用副字幕的独立参数；其余场景
    // secondary 通道才是真正的副字幕，使用副字幕独立参数。
    const int effectiveSecondaryDelayMs =
        protectPrimarySubtitleForDanmaku ? delayMs : secondaryDelayMs;
    const int effectiveSecondaryPosition =
        protectPrimarySubtitleForDanmaku ? position : secondaryPosition;
    const int effectiveSecondaryScalePercent = protectPrimarySubtitleForDanmaku
                                                   ? scalePercent
                                                   : secondaryScalePercent;

    const double secondaryDelaySeconds = effectiveSecondaryDelayMs / 1000.0;
    const double secondaryScaleFactor = effectiveSecondaryScalePercent / 100.0;

    controller->setProperty(QStringLiteral("sub-font"), fontFamily);
    controller->setProperty(QStringLiteral("sub-font-size"), fontSize);
    controller->setProperty(QStringLiteral("sub-scale"), scaleFactor);
    controller->setProperty(QStringLiteral("sub-outline-size"), outlinePixels);
    controller->setProperty(QStringLiteral("sub-shadow-offset"), shadowPixels);
    // 副字幕属性：mpv 的 secondary 通道只支持 scale/pos/delay/ass-override
    // （字体/字号/描边/阴影跟随主字幕，见上方注释）。
    controller->setProperty(QStringLiteral("secondary-sub-scale"),
                            secondaryScaleFactor);
    controller->setProperty(QStringLiteral("secondary-sub-ass-override"),
                            QStringLiteral("force"));
    controller->setProperty(QStringLiteral("secondary-sub-pos"),
                            effectiveSecondaryPosition);
    controller->setProperty(QStringLiteral("secondary-sub-delay"),
                            secondaryDelaySeconds);

    if (protectPrimarySubtitleForDanmaku) {
        controller->setProperty(QStringLiteral("sub-ass-override"),
                                QStringLiteral("no"));
        controller->setProperty(QStringLiteral("sub-pos"), 100);
        controller->setProperty(QStringLiteral("sub-delay"), 0.0);
    } else {
        controller->setProperty(QStringLiteral("sub-ass-override"),
                                QStringLiteral("force"));
        controller->setProperty(QStringLiteral("sub-pos"), position);
        controller->setProperty(QStringLiteral("sub-delay"), delaySeconds);
    }

    qDebug().noquote()
        << "[Subtitle][Player] Apply style"
        << "| font:" << fontFamily
        << "| delayMs:" << delayMs
        << "| fontSize:" << fontSize
        << "| position:" << position
        << "| outline:" << outlinePixels
        << "| shadow:" << shadowPixels
        << "| scale:" << scaleFactor
        << "| secondaryScale:" << secondaryScaleFactor
        << "| secondaryPosition:" << effectiveSecondaryPosition
        << "| protectDanmakuPrimary:" << protectPrimarySubtitleForDanmaku;
}

} 
