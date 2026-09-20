#ifndef MPVCONTROLLER_H
#define MPVCONTROLLER_H

#include <QObject>
#include <QVariant>
#include <QString>
#include <QAtomicInt>
#include <mpv/client.h>

class MpvController : public QObject {
    Q_OBJECT
public:
    explicit MpvController(QObject *parent = nullptr);
    ~MpvController() override;

    mpv_handle *mpv() const { return m_mpv; }

    // init() 实际生效的 hwdec 值（含 RDP 会话强制 "no" 的决策），
    // 供按片源覆盖后恢复基线使用。
    QString effectiveHwdec() const { return m_effectiveHwdec; }

    
    // standalone=true：改用 vo=gpu-next + wid 让 mpv 自建渲染（挂到 wid 指向的
    // 原生 HWND），绕开 render API 锁死的旧 gpu renderer，从而支持 Dolby Vision
    // P5（libplacebo/gpu-next 才处理 IPT 色彩）。Embedded 场景保持 vo=libmpv
    // render API 不变。
    bool init(bool standalone = false, void *wid = nullptr);

    
    void forceCleanup();

    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    static void warmupOnce();

    void observeProperty(const QString &property, mpv_format format, uint64_t id = 0);
    int setProperty(const QString &property, const QVariant &value);
    QVariant getProperty(const QString &property);
    int command(const QVariant &params, QVariant *resultOut);
    QVariant command(const QVariant &params);

signals:
    
    void propertyChanged(const QString &property, const QVariant &value);

    
    void positionChanged(double position);
    void durationChanged(double duration);
    void playbackStateChanged(bool isPaused);
    void fileLoaded();
    void endOfFile(const QString &reason);
    void errorOccurred(const QString &errorMsg);

public slots:
    void eventHandler();

private:
    static void onMpvEvents(void *ctx);

    
    void setNode(mpv_node *dst, const QVariant &src);
    void freeNode(mpv_node *dst);
    QVariant nodeToVariant(const mpv_node *node);
    mpv_node_list *createList(mpv_node *dst, bool is_map, int num);

    mpv_handle *m_mpv;
    QString m_effectiveHwdec;

    
    
    static QAtomicInt s_warmedUp;
};

#endif 
