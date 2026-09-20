#ifndef SUBTITLEOPTIONUTILS_H
#define SUBTITLEOPTIONUTILS_H

#include <QString>
#include <QStringList>

namespace SubtitleOptionUtils {

enum class SliderKind {
    DelayMs,
    FontSize,
    Position,
    OutlineSize,
    ShadowOffset,
    ScalePercent
};

struct SliderSpec {
    int minimum = 0;
    int maximum = 100;
    int singleStep = 1;
    int pageStep = 10;
    int defaultValue = 0;
};

// 副字幕位置未单独设置时的回退偏移：主字幕位置上移该数值（数值更小 = 更靠
// 画面中上部），避免两条字幕叠在同一条线上。样式应用（SubtitleStyleUtils）
// 与设置对话框（PlayerSubtitleSettingsDialog）共用。
constexpr int kSecondaryPositionFallbackOffset = 6;

SliderSpec sliderSpec(SliderKind kind);
int clampSliderValue(SliderKind kind, int value);
QString formatSliderValue(SliderKind kind, int value);
bool parseSliderValue(SliderKind kind, QString text, int &value);

QString defaultFontFamily();
QString normalizeFontFamily(QString value);
QStringList availableFontFamilies();

} 

#endif 
