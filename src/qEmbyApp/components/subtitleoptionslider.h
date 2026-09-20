#ifndef SUBTITLEOPTIONSLIDER_H
#define SUBTITLEOPTIONSLIDER_H

#include "../utils/subtitleoptionutils.h"

#include <QVariant>
#include <QWidget>

class QLabel;
class ModernSlider;
class EditableValueLabel;

class SubtitleOptionSlider : public QWidget
{
    Q_OBJECT

public:
    // fallbackValue < 0 时回退到 SliderKind 的默认值；>= 0 时作为"配置未设置"
    // 时的回退值使用（副字幕参数借此回退到主字幕对应值，保持初始一致）。
    explicit SubtitleOptionSlider(SubtitleOptionUtils::SliderKind kind,
                                  QString configKey,
                                  QWidget *parent = nullptr,
                                  int fallbackValue = -1);

    int value() const;

signals:
    void valueChanged(int value);

private slots:
    void handleSliderValueChanged(int value);
    void handleConfigValueChanged(const QString &key, const QVariant &newValue);
    void handleValueTextSubmitted(const QString &text);

private:
    void applySpec();
    void refreshLabels();
    void syncFromStore();
    void setCurrentValue(int value, bool persistToStore, bool notify);
    static int variantToInt(const QVariant &value, int fallback);

    SubtitleOptionUtils::SliderKind m_kind;
    QString m_configKey;
    int m_fallbackValue = -1;
    EditableValueLabel *m_valueLabel = nullptr;
    QLabel *m_minLabel = nullptr;
    QLabel *m_maxLabel = nullptr;
    ModernSlider *m_slider = nullptr;
};

#endif 
