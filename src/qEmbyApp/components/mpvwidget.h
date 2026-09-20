#ifndef MPVWIDGET_H
#define MPVWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QString>
#include <mpv/render_gl.h>
#include "mpvcontroller.h"

class MpvHttpStreamRelay;
class QThread;
class QPaintEvent;
class QResizeEvent;
class QShowEvent;

class MpvWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    // standalone=true：独立窗口模式，mpv 用 vo=gpu-next + wid 自建渲染到
    // 本 widget 的原生 HWND（绕开 render API 的旧 gpu renderer，支持杜比
    // 视界 P5）。此时本 widget 不再走 OpenGL render context，仅充当 wid 容器。
    explicit MpvWidget(QWidget *parent = nullptr, bool standalone = false);
    ~MpvWidget() override;

    
    void shutdown();

    void loadMedia(const QString &url, const QString &serverId = QString());
    void play();
    void resumeAfterContextRestore();
    void pause();
    void stop();
    void seek(double positionInSeconds);

    // Custom User-Agent for HTTP(S) streams (servers with strict UA
    // whitelists). Empty = keep libmpv default ("libmpv").
    void setCustomUserAgent(const QString &userAgent);

    // 按片源强制视频软解（hwdec=no）。用于纯 Dolby Vision（profile 5）：
    // 硬解会把携带 DV 元数据的 RPU NAL 丢弃，mpv 无法应用 fallback 色彩
    // 映射，画面发绿。false = 恢复 init 时的基线（含 RDP 强制软解）。
    void setForceSoftwareDecode(bool force);

    MpvController* controller() const { return m_controller; }

signals:
    
    void positionChanged(double position);
    void durationChanged(double duration);
    void playbackStateChanged(bool isPaused);
    void networkSpeedChanged(qint64 bytesPerSecond);
    void relayActiveChanged(bool active);
    void errorOccurred(const QString &errorMsg);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;
    // standalone 模式不触发 QOpenGLWidget 的 GL 合成（避免与 mpv d3d11 直绘
    // 争抢同一原生 HWND），直接吞掉 paint 事件。
    void paintEvent(QPaintEvent *event) override;
    // standalone 模式：widget 尺寸变化 / 首次显示时，主动把 mpv 的渲染区同步成
    // widget 客户区大小（wid 下 mpv 不一定收得到 Qt 的尺寸变化通知）。
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private slots:
    void cleanupGL(); 
    void maybeUpdate(); 

private:
    static void onMpvRenderUpdate(void *ctx);
    static void *getProcAddress(void *ctx, const char *name);
    void loadMediaNow(const QString &url, const QString &serverId, bool wasPending);
    // relay lives on m_relayThread, so stop() has to be dispatched to it (and
    // skipped once that thread is gone -- see the destructor).
    void stopRelay();
    // standalone：把 mpv 自建的渲染子窗口尺寸对齐到本 widget 的客户区。
    void syncStandaloneRenderArea();

    MpvController *m_controller;
    MpvHttpStreamRelay *m_streamRelay = nullptr;
    // The relay runs on its own thread: with a non-interleaved audio track it
    // serves several hundred connections per second, which measured at 83% of
    // one core -- on the very thread that also runs the Qt UI and the danmaku
    // overlay, so the window and the keyboard both felt sluggish. Moving it off
    // that thread costs nothing extra and stops the contention; the total CPU
    // stays the same but lands on another core.
    QThread *m_relayThread = nullptr;
    bool m_usingStreamRelay = false;
    bool m_resumeWhenRenderReady = false;
    bool m_standalone = false;
    mpv_render_context *m_mpv_gl;

    
    QString m_pendingUrl;
    QString m_pendingServerId;
    QString m_customUserAgent;
};

#endif 
