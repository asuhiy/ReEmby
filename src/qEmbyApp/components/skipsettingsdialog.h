#ifndef SKIPSETTINGSDIALOG_H
#define SKIPSETTINGSDIALOG_H

#include "playeroverlaydialog.h"

#include <QString>
#include <functional>

class QLabel;
class ModernSlider;

// 播放器内"片头片尾设置"对话框：为当前剧集
// （按 seriesId）或当前影片（按 itemId）设置手动跳过时长——时长滑块 +
// "用当前位置"按钮 + 恢复默认/取消/保存。保存后发出 settingsSaved，
// 由 PlayerView 立即应用到当前播放。
class SkipSettingsDialog : public PlayerOverlayDialog
{
    Q_OBJECT

public:
    // 手动时长上限（"范围 5 分钟"）。
    static constexpr int kMaxSeconds = 300;

    SkipSettingsDialog(const QString &seriesId, const QString &itemId,
                       const QString &scopeName,
                       std::function<double()> positionProvider,
                       std::function<double()> durationProvider,
                       QWidget *parent = nullptr);

signals:
    void settingsSaved();

private:
    void buildUi();
    void saveAndClose();
    void restoreDefaults();
    int currentPositionSeconds() const;
    int remainingSeconds() const;

    const QString m_seriesId;
    const QString m_itemId;
    const QString m_scopeName;
    const std::function<double()> m_positionProvider;
    const std::function<double()> m_durationProvider;

    ModernSlider *m_introSlider = nullptr;
    ModernSlider *m_outroSlider = nullptr;
    QLabel *m_introValueLabel = nullptr;
    QLabel *m_outroValueLabel = nullptr;
    // 点击过"恢复默认"后保存：清除本剧/本条覆盖（回到全局默认）而不是写入。
    bool m_restoreRequested = false;
};

#endif // SKIPSETTINGSDIALOG_H
