#include "mpvcontroller.h"
#include "../utils/subtitlestyleutils.h"
#include "../utils/playerpreferenceutils.h"
#include <QDebug>
#include <QMetaType>
#include <QElapsedTimer>
#include <QThreadPool>
#include <clocale>
#include <cstdint>
#include <config/configstore.h>
#include <config/config_keys.h>
#include <QStandardPaths>
#include <QRegularExpression>
#ifdef Q_OS_WIN
#include <QThread>
#include <windows.h>
#include <utils/apppaths.h>
#endif


QAtomicInt MpvController::s_warmedUp(0);


struct node_autofree {
    mpv_node *ptr;
    explicit node_autofree(mpv_node *a_ptr) : ptr(a_ptr) {}
    ~node_autofree() { mpv_free_node_contents(ptr); }
};

MpvController::MpvController(QObject *parent)
    : QObject(parent), m_mpv(nullptr) {
}

MpvController::~MpvController() {
    forceCleanup();
}

void MpvController::forceCleanup() {
    if (m_mpv) {
        
        this->blockSignals(true);

        mpv_command_string(m_mpv, "stop");
        
        mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
        
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
}

void MpvController::warmupOnce() {
    
    
    if (!s_warmedUp.testAndSetOrdered(0, 1)) {
        qDebug() << "[MpvController::warmupOnce] already warmed up, skip";
        return;
    }

    qDebug() << "[MpvController::warmupOnce] dispatch mpv warmup to worker thread"
             << "(preload libmpv DLLs + FFmpeg codec tables to avoid first-play UI freeze)";

    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    QThreadPool::globalInstance()->start([]() {
        QElapsedTimer timer;
        timer.start();

        {
            MpvController warmup;
            const bool ok = warmup.init();
            qDebug() << "[MpvController::warmupOnce] mpv_initialize completed"
                     << "| ok=" << ok
                     << "| elapsed=" << timer.elapsed() << "ms";
        }
        

        qDebug() << "[MpvController::warmupOnce] total elapsed" << timer.elapsed() << "ms"
                 << "(libmpv DLLs + codec tables now resident in process memory;"
                 << "next mpv_initialize will run on hot path)";
    });
}

bool MpvController::init(bool standalone, void *wid) {
    
    std::setlocale(LC_NUMERIC, "C");

    m_mpv = mpv_create();
    if (!m_mpv) {
        emit errorOccurred(tr("Failed to create MPV instance."));
        return false;
    }

    
    
    
    
    mpv_set_option_string(m_mpv, "osc", "no");
    
    mpv_set_option_string(m_mpv, "load-scripts", "no");
    
    mpv_set_option_string(m_mpv, "config", "no");

    
    // standalone：mpv 自建渲染管线（vo=gpu-next → d3d11）挂到 wid 指向的
    // 原生 HWND，绕开 render API 的旧 gpu renderer，从而支持杜比视界 P5。
    // Embedded：render API（vo=libmpv），由 MpvWidget 的 OpenGL 上下文驱动。
    if (standalone) {
        mpv_set_option_string(m_mpv, "vo", "gpu-next");
        if (wid) {
            int64_t widValue =
                static_cast<int64_t>(reinterpret_cast<std::intptr_t>(wid));
            mpv_set_option(m_mpv, "wid", MPV_FORMAT_INT64, &widValue);
        }
    } else {
        mpv_set_option_string(m_mpv, "vo", "libmpv");
    }
    
    mpv_set_option_string(m_mpv, "keep-open", "yes");
    
    
    
    
    // blend-subtitles=video 会把字幕烘焙进视频帧，在 vo=libmpv render API +
    // 硬解 copy 模式 + 10-bit（p010）组合下字幕被静默吞掉（视频正常、
    // 无任何报错）。恢复 mpv 默认值（no，由 renderer 正常合成字幕）。
    mpv_set_option_string(m_mpv, "sub-gray", "yes");
    mpv_set_option_string(m_mpv, "sub-blur", "0");
    mpv_set_option_string(m_mpv, "sub-gauss", "0");
    
    
    mpv_set_option_string(m_mpv, "sub-hinting", "none");
    mpv_set_option_string(m_mpv, "sub-ass-hinting", "none");
    // libavcodec 的 warning 会被 mpv 原样转发：DV 片源几乎每帧都会打印
    // "Multiple Dolby Vision RPUs found in one AU"，一次播放能刷出两千多行，
    // 把真正有用的日志淹没。把 ffmpeg 模块压到 error 级即可去掉这类噪音，
    // mpv 自身以及其它模块的 warning 不受影响。
    // 模块名必须写到子模块级：mpv 日志里的前缀是 "ffmpeg/video"，
    // 只写 "ffmpeg" 匹配不上（v0.41 实测无效）。
    const char *ffmpegQuiet =
        "ffmpeg=error,ffmpeg/video=error,ffmpeg/audio=error,ffmpeg/demux=error";
    // mpv 日志级别（「设置 → 通用 → mpv 日志级别」，默认 info）：连接、缓冲、
    // seek 这些排查起播慢最需要的信息都是 verbose(v) 级，默认的 info 会把它们
    // 全滤掉 —— 日志里就会莫名出现几十秒的空白。
    // 两级都要放开：msg-level 决定 mpv 内部生成哪些消息，
    // mpv_request_log_messages 决定往客户端送哪些，缺一个都收不到。
    // 取值走白名单：配置里写了别的一律按 info，不把任意字符串塞给 msg-level。
    QString mpvLogLevel =
        ConfigStore::instance()
            ->get<QString>(ConfigKeys::PlayerMpvLogLevel, QStringLiteral("info"))
            .trimmed()
            .toLower();
    if (mpvLogLevel != QLatin1String("v") &&
        mpvLogLevel != QLatin1String("debug")) {
        mpvLogLevel = QStringLiteral("info");
    }
    const bool verboseLog = (mpvLogLevel != QLatin1String("info"));
    const QByteArray levelBytes = mpvLogLevel.toUtf8();
    const QByteArray msgLevel =
        verboseLog
            ? QByteArray("all=") + levelBytes + "," + ffmpegQuiet
            : QByteArray(ffmpegQuiet);
    mpv_set_option_string(m_mpv, "msg-level", msgLevel.constData());
    mpv_request_log_messages(m_mpv, levelBytes.constData());

    
    
    
    QString vsyncMode = ConfigStore::instance()->get<QString>(
        ConfigKeys::PlayerVideoSync, "display-resample");
    mpv_set_option_string(m_mpv, "video-sync", vsyncMode.toUtf8().constData());
    
    mpv_set_option_string(m_mpv, "interpolation", vsyncMode == "display-resample" ? "yes" : "no");
    qDebug() << "[MpvController] Applied video sync mode"
             << "| video-sync=" << vsyncMode
             << "| interpolation=" << (vsyncMode == "display-resample");

    
    
    
    QString hwdec = ConfigStore::instance()->get<QString>(ConfigKeys::PlayerHwDec, "auto-copy");

#ifdef Q_OS_WIN
    // standalone 用 d3d11 渲染（gpu-next），"-copy" 变体（auto-copy / d3d11va-copy）
    // 会多做一次 GPU→内存→GPU 的往返拷贝，4K 10bit 下开销明显（用户实测独立窗口
    // 播放卡顿）。d3d11 支持零拷贝互操作，这里把 copy 变体映射回非 copy 版本
    // （auto-copy→auto、d3d11va-copy→d3d11va）。内嵌（render API + OpenGL）保持
    // 原有 auto-copy 不变。
    if (standalone && hwdec.endsWith(QLatin1String("-copy"))) {
        const QString zeroCopyHwdec = hwdec.left(hwdec.size() - 5);
        qInfo() << "[MpvController] standalone hwdec mapped to zero-copy variant"
                << "| configured:" << hwdec << "| effective:" << zeroCopyHwdec;
        hwdec = zeroCopyHwdec;
    }

    // Embedded（render API）走 OpenGL 后端；standalone（gpu-next）由 mpv 自选
    // d3d11，不能再强制 gpu-api=opengl，否则 gpu-next 会退回 OpenGL 互操作。
    if (!standalone) {
        mpv_set_option_string(m_mpv, "gpu-api", "opengl");
        mpv_set_option_string(m_mpv, "opengl-es", "no");
    }
    bool isRDP = GetSystemMetrics(SM_REMOTESESSION) != 0;
    if (isRDP) {
        
        hwdec = "no";
        mpv_set_option_string(m_mpv, "profile", "sw-fast");

        
        mpv_set_option_string(m_mpv, "gpu-dumb-mode", "yes");
        if (!standalone) {
            mpv_set_option_string(m_mpv, "opengl-pbo", "no");
            mpv_set_option_string(m_mpv, "dither-depth", "no");
        }
    }
#endif
    mpv_set_option_string(m_mpv, "hwdec", hwdec.toUtf8().constData());
    m_effectiveHwdec = hwdec;

    
    
    
    const QString audioRules =
        ConfigStore::instance()->get<QString>(ConfigKeys::PlayerAudioLang, "auto");
    const QStringList audioLangCodes =
        PlayerPreferenceUtils::mpvLanguageCodesForRules(audioRules);
    if (!audioLangCodes.isEmpty()) {
        const QByteArray alangValue = audioLangCodes.join(',').toUtf8();
        mpv_set_option_string(m_mpv, "alang", alangValue.constData());
        qDebug() << "[MpvController] Applied audio language rules to MPV"
                 << "| rules=" << audioRules << "| alang=" << alangValue;
    }

    const QString subtitleRules =
        ConfigStore::instance()->get<QString>(ConfigKeys::PlayerSubLang, "auto");
    if (PlayerPreferenceUtils::isSubtitleDisabled(subtitleRules)) {
        mpv_set_option_string(m_mpv, "sid", "no");
        qDebug() << "[MpvController] Subtitle rules disable subtitles"
                 << "| rules=" << subtitleRules;
    } else {
        const QStringList subtitleLangCodes =
            PlayerPreferenceUtils::mpvLanguageCodesForRules(subtitleRules);
        if (!subtitleLangCodes.isEmpty()) {
            const QByteArray slangValue = subtitleLangCodes.join(',').toUtf8();
            mpv_set_option_string(m_mpv, "slang", slangValue.constData());
            qDebug() << "[MpvController] Applied subtitle language rules to MPV"
                     << "| rules=" << subtitleRules << "| slang=" << slangValue;
        }
    }

    
    
    
    bool volNormal = ConfigStore::instance()->get<bool>(ConfigKeys::PlayerVolNormal, false);
    if (volNormal) {
        mpv_set_option_string(m_mpv, "af", "dynaudnorm=g=5:f=250:r=0.9:p=0.5");
    }

    
    
    
    bool useMpvConf = ConfigStore::instance()->get<bool>(ConfigKeys::PlayerUseMpvConf, false);
    if (useMpvConf) {
        QString mpvConfDir = AppPaths::dataRoot() + QStringLiteral("/mpv");
        mpv_set_option_string(m_mpv, "config-dir", mpvConfDir.toUtf8().constData());
        mpv_set_option_string(m_mpv, "config", "yes");
    }

    if (mpv_initialize(m_mpv) < 0) {
        emit errorOccurred(tr("Failed to initialize MPV context."));
        return false;
    }

    SubtitleStyleUtils::applyToController(this);

    mpv_set_wakeup_callback(m_mpv, MpvController::onMpvEvents, this);

    
    observeProperty("time-pos", MPV_FORMAT_DOUBLE);
    observeProperty("duration", MPV_FORMAT_DOUBLE);
    observeProperty("pause", MPV_FORMAT_FLAG);
    observeProperty("eof-reached", MPV_FORMAT_FLAG);
    
    observeProperty("cache-speed", MPV_FORMAT_INT64);
    observeProperty("volume", MPV_FORMAT_DOUBLE);
    observeProperty("mute", MPV_FORMAT_FLAG);
    observeProperty("speed", MPV_FORMAT_DOUBLE);
    observeProperty("track-list", MPV_FORMAT_NODE);
    observeProperty("sid", MPV_FORMAT_NODE);
    observeProperty("secondary-sid", MPV_FORMAT_NODE);
    
    
    observeProperty("paused-for-cache", MPV_FORMAT_FLAG);
    observeProperty("seeking", MPV_FORMAT_FLAG);
    
    return true;
}

void MpvController::onMpvEvents(void *ctx) {
    
    auto *self = static_cast<MpvController *>(ctx);
    QMetaObject::invokeMethod(self, "eventHandler", Qt::QueuedConnection);
}

void MpvController::eventHandler() {
    if (!m_mpv) return;

    while (true) {
        mpv_event *event = mpv_wait_event(m_mpv, 0);
        if (event->event_id == MPV_EVENT_NONE) break;

        switch (event->event_id) {
        case MPV_EVENT_FILE_LOADED:
            emit fileLoaded();
            break;
        case MPV_EVENT_END_FILE: {
            auto *prop = static_cast<mpv_event_end_file *>(event->data);
            if (prop && prop->reason == MPV_END_FILE_REASON_EOF) {
                emit endOfFile("eof");
            } else if (prop && prop->reason == MPV_END_FILE_REASON_ERROR) {
                emit endOfFile("error");
            }
            break;
        }
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto *prop = static_cast<mpv_event_property *>(event->data);
            
            
            if (!prop || !prop->data || prop->format == MPV_FORMAT_NONE) break;

            QString propName = QString::fromUtf8(prop->name);
            QVariant data;

            if (prop->format == MPV_FORMAT_DOUBLE) {
                data = *static_cast<double *>(prop->data);
                if (propName == "time-pos") emit positionChanged(data.toDouble());
                else if (propName == "duration") emit durationChanged(data.toDouble());
            } else if (prop->format == MPV_FORMAT_FLAG) {
                data = *static_cast<bool *>(prop->data);
                if (propName == "pause") emit playbackStateChanged(data.toBool());
            } else if (prop->format == MPV_FORMAT_NODE) {
                data = nodeToVariant(static_cast<mpv_node *>(prop->data));
            } else if (prop->format == MPV_FORMAT_STRING) {
                char *str = *static_cast<char **>(prop->data);
                data = str ? QString::fromUtf8(str) : QString();
            } else if (prop->format == MPV_FORMAT_INT64) {
                data = static_cast<qlonglong>(*static_cast<int64_t *>(prop->data));
            }

            emit propertyChanged(propName, data);
            break;
        }
        case MPV_EVENT_LOG_MESSAGE: {
            auto *msg = static_cast<mpv_event_log_message *>(event->data);
            if (msg) {
                
                qDebug().noquote() << QString("[MPV %1] %2").arg(msg->prefix, QString::fromUtf8(msg->text).trimmed());
            }
            break;
        }
        default:
            break;
        }
    }
}

void MpvController::observeProperty(const QString &property, mpv_format format, uint64_t id) {
    if (m_mpv) {
        mpv_observe_property(m_mpv, id, property.toUtf8().constData(), format);
    }
}

int MpvController::setProperty(const QString &property, const QVariant &value) {
    if (!m_mpv) return MPV_ERROR_UNINITIALIZED;
    mpv_node node;
    memset(&node, 0, sizeof(mpv_node)); 
    setNode(&node, value);
    int err = mpv_set_property(m_mpv, property.toUtf8().constData(), MPV_FORMAT_NODE, &node);
    freeNode(&node);
    if (err < 0) {
        if (property.startsWith(QStringLiteral("secondary-sub-"))) {
            qDebug() << "[MpvController] setProperty failed"
                     << "| property:" << property
                     << "| error:" << mpv_error_string(err);
        } else {
            qWarning() << "[MpvController] setProperty failed"
                       << "| property:" << property
                       << "| error:" << mpv_error_string(err);
        }
    }
    return err;
}

QVariant MpvController::getProperty(const QString &property) {
    if (!m_mpv) return QVariant();
    mpv_node node;
    memset(&node, 0, sizeof(mpv_node)); 
    int err = mpv_get_property(m_mpv, property.toUtf8().constData(), MPV_FORMAT_NODE, &node);
    if (err < 0) return QVariant();
    node_autofree f(&node);
    return nodeToVariant(&node);
}

int MpvController::command(const QVariant &params, QVariant *resultOut) {
    if (!m_mpv) return MPV_ERROR_UNINITIALIZED;

    mpv_node node;
    memset(&node, 0, sizeof(mpv_node));
    setNode(&node, params);

    mpv_node mpvResult;
    memset(&mpvResult, 0, sizeof(mpv_node)); 

    int err = mpv_command_node(m_mpv, &node, &mpvResult);
    freeNode(&node);
    node_autofree f(&mpvResult);

    if (err < 0) {
        QString commandName;
        const QVariantList args = params.toList();
        if (!args.isEmpty()) {
            commandName = args.first().toString();
        }
        qWarning() << "[MpvController] command failed"
                   << "| command:" << (commandName.isEmpty()
                                           ? QStringLiteral("<unknown>")
                                           : commandName)
                   << "| error:" << mpv_error_string(err);
        return err;
    }

    if (resultOut) {
        *resultOut = nodeToVariant(&mpvResult);
    }
    return err;
}

QVariant MpvController::command(const QVariant &params) {
    QVariant result;
    const int err = command(params, &result);
    return err < 0 ? QVariant() : result;
}



mpv_node_list *MpvController::createList(mpv_node *dst, bool is_map, int num) {
    dst->format = is_map ? MPV_FORMAT_NODE_MAP : MPV_FORMAT_NODE_ARRAY;
    auto *list = new mpv_node_list();
    dst->u.list = list;
    list->values = new mpv_node[num]();
    if (is_map) list->keys = new char *[num]();
    return list;
}

void MpvController::setNode(mpv_node *dst, const QVariant &src) {
    if (src.typeId() == QMetaType::QString) {
        dst->format = MPV_FORMAT_STRING;
        dst->u.string = qstrdup(src.toString().toUtf8().constData());
    } else if (src.typeId() == QMetaType::Bool) {
        dst->format = MPV_FORMAT_FLAG;
        dst->u.flag = src.toBool() ? 1 : 0;
    } else if (src.typeId() == QMetaType::Int || src.typeId() == QMetaType::LongLong || src.typeId() == QMetaType::Double) {
        if (src.typeId() == QMetaType::Double) {
            dst->format = MPV_FORMAT_DOUBLE;
            dst->u.double_ = src.toDouble();
        } else {
            dst->format = MPV_FORMAT_INT64;
            dst->u.int64 = src.toLongLong();
        }
    } else if (src.canConvert<QVariantList>()) {
        QVariantList qlist = src.toList();
        mpv_node_list *list = createList(dst, false, qlist.size());
        list->num = qlist.size();
        for (int n = 0; n < qlist.size(); ++n) setNode(&list->values[n], qlist[n]);
    } else if (src.canConvert<QVariantMap>()) {
        QVariantMap qmap = src.toMap();
        mpv_node_list *list = createList(dst, true, qmap.size());
        list->num = qmap.size();
        int n = 0;
        for (auto it = qmap.constKeyValueBegin(); it != qmap.constKeyValueEnd(); ++it) {
            list->keys[n] = qstrdup(it->first.toUtf8().constData());
            setNode(&list->values[n], it->second);
            ++n;
        }
    } else {
        dst->format = MPV_FORMAT_NONE;
    }
}

void MpvController::freeNode(mpv_node *dst) {
    switch (dst->format) {
    case MPV_FORMAT_STRING:
        delete[] dst->u.string;
        break;
    case MPV_FORMAT_NODE_ARRAY:
    case MPV_FORMAT_NODE_MAP: {
        mpv_node_list *list = dst->u.list;
        if (list) {
            for (int n = 0; n < list->num; ++n) {
                if (list->keys) delete[] list->keys[n];
                if (list->values) freeNode(&list->values[n]);
            }
            delete[] list->keys;
            delete[] list->values;
        }
        delete list;
        break;
    }
    default: break;
    }
    dst->format = MPV_FORMAT_NONE;
}

QVariant MpvController::nodeToVariant(const mpv_node *node) {
    switch (node->format) {
    case MPV_FORMAT_STRING: return QVariant(QString::fromUtf8(node->u.string));
    case MPV_FORMAT_FLAG: return QVariant(static_cast<bool>(node->u.flag));
    case MPV_FORMAT_INT64: return QVariant(static_cast<qlonglong>(node->u.int64));
    case MPV_FORMAT_DOUBLE: return QVariant(node->u.double_);
    case MPV_FORMAT_NODE_ARRAY: {
        mpv_node_list *list = node->u.list;
        QVariantList qlist;
        for (int n = 0; n < list->num; ++n) qlist.append(nodeToVariant(&list->values[n]));
        return QVariant(qlist);
    }
    case MPV_FORMAT_NODE_MAP: {
        mpv_node_list *list = node->u.list;
        QVariantMap qmap;
        for (int n = 0; n < list->num; ++n) qmap.insert(QString::fromUtf8(list->keys[n]), nodeToVariant(&list->values[n]));
        return QVariant(qmap);
    }
    default: return QVariant();
    }
}
