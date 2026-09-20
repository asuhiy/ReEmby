#include "pageplayer.h"
#include "../../components/danmakuserverdialog.h"
#include "../../components/danmakuoptionslider.h"
#include "../../components/moderncombobox.h"
#include "../../components/modernnumberinput.h"
#include "../../components/modernswitch.h"
#include "../../components/moderntaginput.h"
#include "../../components/moderntoast.h"
#include "../../components/playercombobox.h"
#include "../../components/settingscard.h"
#include "../../components/settingssubpanel.h"
#include "../../utils/danmakuoptionutils.h"
#include "../../utils/danmakurendererutils.h"
#include "../../managers/externalplayerdetector.h"
#include "../../managers/thememanager.h"
#include "config/config_keys.h"
#include "config/configstore.h"
#include "services/skip/skipsegmentsstore.h"
#include "services/danmaku/danmakuservice.h"
#include "services/danmaku/danmakusettings.h"
#include "services/manager/servermanager.h"
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QSlider>
#include <QStandardPaths>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QComboBox>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QStringList>
#include <QStandardPaths>
#include <QTextStream>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>
#include <algorithm>
#include <memory>
#include <qembycore.h>
#include <utils/apppaths.h>

PagePlayer::PagePlayer(QEmbyCore *core, QWidget *parent)
    : SettingsPageBase(core, tr("Player"), parent) {
  const QString sid = m_core->serverManager()->activeProfile().id;
  
  auto *hwCombo = new ModernComboBox(this);
  hwCombo->addItem(tr("Auto"), "auto-copy");
  hwCombo->addItem(tr("D3D11VA"), "d3d11va-copy");
  hwCombo->addItem(tr("DXVA2"), "dxva2-copy");
  hwCombo->addItem(tr("CUDA (NVIDIA)"), "cuda-copy");
  hwCombo->addItem(tr("None (Software)"), "no");
  m_mainLayout->addWidget(
      new SettingsCard(":/svg/dark/hw-decode.svg", tr("Hardware Decoding"),
                       tr("Select GPU acceleration method for video decoding"),
                       hwCombo, ConfigKeys::PlayerHwDec, this));

  
  auto *vsyncCombo = new ModernComboBox(this);
  vsyncCombo->addItem(tr("Audio"), "audio");
  vsyncCombo->addItem(tr("Display Resample (Default)"), "display-resample");
  vsyncCombo->addItem(tr("Display VSync"), "display-vdrop");
  m_mainLayout->addWidget(
      new SettingsCard(":/svg/dark/video-sync.svg", tr("Video Sync Mode"),
                       tr("Method for synchronizing video frames to display"),
                       vsyncCombo, ConfigKeys::PlayerVideoSync, this,
                       QVariant("display-resample")));

  auto createLanguageRuleInput = [this](bool allowNone) {
    auto *input = new ModernTagInput(this);
    input->setPopupMode(ModernTagInput::ForcePopup);
    input->addPreset(tr("Auto"), "auto");
    if (allowNone) {
      input->addPreset(tr("None"), "none");
    }
    input->addPreset(tr("Chinese"), "chi");
    input->addPreset(tr("Chinese (Hong Kong)"), "chi-hk");
    input->addPreset(tr("Chinese (Macau)"), "chi-mo");
    input->addPreset(tr("Chinese (Taiwan)"), "chi-tw");
    input->addPreset(tr("English"), "eng");
    input->addPreset(tr("Japanese"), "jpn");
    input->addPreset(tr("Korean"), "kor");
    input->addPreset(tr("French"), "fre");
    input->addPreset(tr("German"), "ger");
    input->addPreset(tr("Spanish"), "spa");
    input->addPreset(tr("Russian"), "rus");
    return input;
  };

  
  auto *audioLangInput = createLanguageRuleInput(false);
  m_mainLayout->addWidget(new SettingsCard(
      ":/svg/dark/audio-lang.svg", tr("Preferred Audio Language"),
      tr("Rules are matched in order; choose presets or type custom keywords and press Enter"),
      audioLangInput, ConfigKeys::PlayerAudioLang, this, QVariant("auto")));

  
  auto *subLangInput = createLanguageRuleInput(true);
  m_mainLayout->addWidget(new SettingsCard(
      ":/svg/dark/subtitle-lang.svg", tr("Preferred Subtitle Language"),
      tr("Rules are matched in order; choose presets or type custom keywords and press Enter"),
      subLangInput, ConfigKeys::PlayerSubLang, this, QVariant("auto")));

  
  auto *versionInput = new ModernTagInput(this);
  versionInput->addPreset(tr("Original"), "original");
  versionInput->addPreset(tr("4K"), "2160p");
  versionInput->addPreset(tr("1080p"), "1080p");
  versionInput->addPreset(tr("720p"), "720p");
  versionInput->addPreset(tr("480p"), "480p");
  versionInput->addPreset(tr("Remux"), "remux");
  versionInput->addPreset(tr("Bitrate High to Low"), "bitrate-desc");
  versionInput->addPreset(tr("Bitrate Low to High"), "bitrate-asc");
  versionInput->addPreset(tr("File Size Large to Small"), "size-desc");
  versionInput->addPreset(tr("File Size Small to Large"), "size-asc");
  versionInput->addPreset(tr("Date New to Old"), "date-desc");
  versionInput->addPreset(tr("Date Old to New"), "date-asc");
  m_mainLayout->addWidget(new SettingsCard(
      ":/svg/dark/video-version.svg", tr("Preferred Video Version"),
      tr("Choose keyword or sort rules in order; type custom keywords and press Enter"),
      versionInput, ConfigKeys::PlayerPreferredVersion, this));


  m_mainLayout->addWidget(new SettingsCard(
      ":/svg/dark/player-continuous-play.svg", tr("Continuous Playback"),
      tr("Automatically play the next episode or media when playback ends"),
      new ModernSwitch(this), ConfigKeys::PlayerContinuousPlay, this,
      QVariant(true)));

  // Faster start: lower the initial buffered-data threshold mpv waits for
  // before playback begins (cache-pause-wait). Trade-off: on flaky
  // connections playback may pause briefly earlier than with the default.
  m_mainLayout->addWidget(new SettingsCard(
      ":/svg/dark/hw-decode.svg", tr("Fast Start"),
      tr("Start playback sooner by lowering the initial buffering "
         "requirement; may pause briefly on slow connections"),
      new ModernSwitch(this), ConfigKeys::PlayerFastStart, this,
      QVariant(false)));

  // Compatibility relay: a local HTTP proxy that caches byte ranges so mpv
  // does not pay a network round trip for every small follow-up request.
  // Needed only for sources whose audio track is stored apart from the video
  // (non-interleaved layout); for a normally interleaved file it is pure
  // overhead. Off by default: when disabled the relay is never created and
  // mpv talks to the server directly.
  m_mainLayout->addWidget(new SettingsCard(
      ":/svg/dark/hw-decode.svg", tr("Compatibility Relay"),
      tr("Turn on only if a source keeps stuttering while other files play "
         "smoothly; it adds a local proxy and uses more CPU"),
      new ModernSwitch(this), ConfigKeys::PlayerRelayEnabled, this,
      QVariant(false)));

  // Next-episode prefetch: slider 0-100 (0 = disabled). Value is read into
  // PlayerView at playback start, so a new value applies to the next media.
  {
    auto *prefetchWidget = new QWidget(this);
    auto *row = new QHBoxLayout(prefetchWidget);
    row->setContentsMargins(0, 0, 0, 0);
    auto *prefetchSlider = new QSlider(Qt::Horizontal, prefetchWidget);
    prefetchSlider->setRange(0, 100);
    prefetchSlider->setMinimumWidth(200);
    auto *prefetchValue = new QLabel(prefetchWidget);
    // 数值标签默认走 palette 文字色，在暗色主题下几乎看不清（用户反馈）。
    // 交给主题 QSS 上色：#settingsProgressValue（深色主题近白 / 浅色主题深灰）。
    prefetchValue->setObjectName(QStringLiteral("settingsProgressValue"));
    prefetchValue->setMinimumWidth(36);
    prefetchValue->setAlignment(Qt::AlignCenter);
    row->addWidget(prefetchSlider, 1);
    row->addWidget(prefetchValue);

    const int initial =
        ConfigStore::instance()->get<int>(ConfigKeys::PlayerPrefetchThreshold, 90);
    prefetchSlider->setValue(initial);
    prefetchValue->setText(QString::number(initial));
    connect(prefetchSlider, &QSlider::valueChanged, this, [prefetchValue](int v) {
      prefetchValue->setText(QString::number(v));
      ConfigStore::instance()->set(ConfigKeys::PlayerPrefetchThreshold, v);
    });

    m_mainLayout->addWidget(new SettingsCard(
        ":/svg/dark/player-continuous-play.svg", tr("Prefetch Next Episode"),
        tr("Negotiate the next episode's playback source once playback "
           "progress passes this percentage, so auto-advance starts "
           "instantly (0 = disabled)"),
        prefetchWidget, QString(), this));
  }

  // Disk-backed demuxer cache: mpv writes the cache's packet data to disk
  // instead of RAM, which is what makes a very large buffer size affordable
  // on a machine with limited memory. Cache files are deleted as soon as the
  // media is unloaded, so this does not speed up replaying the same media,
  // and no manual flush is needed when switching media.
  {
    auto *diskCacheSwitch = new ModernSwitch(this);
    m_mainLayout->addWidget(new SettingsCard(
        ":/svg/dark/hw-decode.svg", tr("Disk Cache"),
        tr("Write cache data to disk instead of memory, so a large buffer "
           "size can be used even with limited RAM"),
        diskCacheSwitch, ConfigKeys::PlayerDiskCache, this, QVariant(false)));

    auto *cacheDirRow = new QWidget(this);
    auto *cacheDirLayout = new QHBoxLayout(cacheDirRow);
    cacheDirLayout->setContentsMargins(0, 0, 0, 0);
    cacheDirLayout->setSpacing(8);
    auto *cacheDirInput = new QLineEdit(cacheDirRow);
    cacheDirInput->setPlaceholderText(tr("Cache directory (default: system temp)"));
    cacheDirInput->setMinimumWidth(280);
    cacheDirInput->setText(ConfigStore::instance()->get<QString>(
        ConfigKeys::PlayerDiskCacheDir, QString()));
    connect(cacheDirInput, &QLineEdit::editingFinished, this, [cacheDirInput]() {
      ConfigStore::instance()->set(ConfigKeys::PlayerDiskCacheDir,
                                   cacheDirInput->text().trimmed());
    });
    auto *cacheDirBrowse = new QPushButton(tr("Browse..."), cacheDirRow);
    cacheDirBrowse->setObjectName(QStringLiteral("secondary-btn"));
    connect(cacheDirBrowse, &QPushButton::clicked, this, [cacheDirInput]() {
      const QString start = cacheDirInput->text().trimmed().isEmpty()
                                ? AppPaths::cacheDir()
                                : cacheDirInput->text().trimmed();
      const QString picked = QFileDialog::getExistingDirectory(
          cacheDirInput, tr("Select Cache Directory"), start,
          QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
      if (picked.isEmpty()) return;
      cacheDirInput->setText(picked);
      ConfigStore::instance()->set(ConfigKeys::PlayerDiskCacheDir, picked);
    });
    cacheDirLayout->addWidget(cacheDirInput, 1);
    cacheDirLayout->addWidget(cacheDirBrowse);
    m_mainLayout->addWidget(new SettingsCard(
        ":/svg/dark/hw-decode.svg", tr("Disk Cache Directory"),
        tr("Where mpv stores disk cache files. Empty uses the system "
           "temporary directory"),
        cacheDirRow, QString(), this));
  }

  // ---- Advanced mpv tuning ------------------------------------------
  // Mirrors the advanced options found in desktop players. Values are read
  // per-file by MpvWidget (mpvwidget.cpp loadMediaNow); empty/absent values
  // leave mpv defaults untouched.

  // Audio channel layout.
  {
    auto *audioChannels = new QComboBox(this);
    audioChannels->addItem(tr("Auto"), QStringLiteral("auto"));
    audioChannels->addItem(tr("Stereo"), QStringLiteral("stereo"));
    audioChannels->addItem(tr("Mono"), QStringLiteral("mono"));
    const QString cur = ConfigStore::instance()->get<QString>(
        ConfigKeys::PlayerAudioChannels, QString());
    const int idx = audioChannels->findData(cur);
    audioChannels->setCurrentIndex(idx >= 0 ? idx : 0);
    connect(audioChannels, &QComboBox::currentIndexChanged, this,
            [audioChannels](int) {
              const QString v = audioChannels->currentData().toString();
              ConfigStore::instance()->set(
                  ConfigKeys::PlayerAudioChannels,
                  v == QStringLiteral("auto") ? QString() : v);
            });
    m_mainLayout->addWidget(new SettingsCard(
        ":/svg/dark/hw-decode.svg", tr("Audio Channel Layout"),
        tr("Force the output channel layout. Auto lets mpv pick"),
        audioChannels, QString(), this));
  }

  auto addAdvancedBool = [this](const QString &title, const QString &desc,
                                const QString &configKey, bool defaultValue,
                                const QString &mpvProperty) {
    Q_UNUSED(mpvProperty); // property application lives in MpvWidget
    auto *sw = new ModernSwitch(this);
    m_mainLayout->addWidget(new SettingsCard(
        ":/svg/dark/hw-decode.svg", title, desc, sw, configKey, this,
        QVariant(defaultValue)));
  };

  addAdvancedBool(tr("Audio Normalize Downmix"),
                  tr("Normalize audio volume when downmixing channels"),
                  ConfigKeys::PlayerAudioNormalizeDownmix, true,
                  QStringLiteral("audio-normalize-downmix"));
  addAdvancedBool(tr("Audio Exclusive Mode"),
                  tr("Use exclusive audio output when supported by the device"),
                  ConfigKeys::PlayerAudioExclusive, false,
                  QStringLiteral("audio-exclusive"));
  addAdvancedBool(tr("Keep Audio Stream on Pause"),
                  tr("Keep the audio stream alive while paused to prevent "
                     "device sleep"),
                  ConfigKeys::PlayerAudioStreamSilence, false,
                  QStringLiteral("audio-stream-silence"));
  addAdvancedBool(tr("curl Network Backend"),
                  tr("Prefer libcurl over ffmpeg for network streams"),
                  ConfigKeys::PlayerCurlBackend, false,
                  QStringLiteral("network-mode"));
  addAdvancedBool(tr("TCP Keep-Alive"),
                  tr("Send TCP keep-alive probes on network streams"),
                  ConfigKeys::PlayerTcpKeepAlive, false,
                  QStringLiteral("network-tcp-keepalive"));

  auto addAdvancedSpin = [this](const QString &title, const QString &desc,
                                const QString &configKey, int minimum,
                                int maximum, int defaultValue,
                                const QString &suffix) {
    auto *spin = new QSpinBox(this);
    spin->setRange(minimum, maximum);
    spin->setMinimumWidth(96);
    if (!suffix.isEmpty()) {
      spin->setSuffix(suffix);
    }
    const int cur = ConfigStore::instance()->get<int>(configKey, defaultValue);
    spin->setValue(cur);
    connect(spin, &QSpinBox::valueChanged, this, [configKey](int v) {
      ConfigStore::instance()->set(configKey, v);
    });
    m_mainLayout->addWidget(new SettingsCard(
        ":/svg/dark/hw-decode.svg", title, desc, spin, QString(), this));
  };

  addAdvancedSpin(tr("Stream Buffer I/O Size (KiB)"),
                  tr("Size of the network stream I/O buffer"),
                  ConfigKeys::PlayerStreamBufferSize, 16, 262144, 128,
                  QString());
  addAdvancedSpin(tr("Max Buffer Size (MiB)"),
                  tr("Maximum demuxer read-ahead buffer"),
                  ConfigKeys::PlayerDemuxerMaxBytes, 128, 16384, 1536, QString());
  addAdvancedSpin(tr("Max Backward Buffer (MiB)"),
                  tr("Maximum backward demuxer buffer for seeking back"),
                  ConfigKeys::PlayerDemuxerMaxBackBytes, 0, 8192, 0, QString());
  addAdvancedSpin(tr("Buffer Read-Ahead (seconds)"),
                  tr("How much media is read ahead in seconds"),
                  ConfigKeys::PlayerDemuxerReadaheadSecs, 1, 3600, 1, QString());

  
  auto *longPressSwitch = new ModernSwitch(this);
  m_mainLayout->addWidget(new SettingsCard(
      ":/svg/dark/player-long-press.svg", tr("Long Press Left/Right"),
      tr("Enable long press actions for left/right keys during playback"),
      longPressSwitch, ConfigKeys::PlayerLongPressSeek, this, QVariant(true)));

  auto bindPlayerSubPanelCombo = [](QComboBox *combo, const QString &configKey,
                                    const QString &defaultValue) {
    if (!combo || configKey.isEmpty()) {
      return;
    }

    auto *store = ConfigStore::instance();
    QString currentValue = store->get<QString>(configKey, defaultValue);
    int currentIndex = combo->findData(currentValue);
    if (currentIndex < 0) {
      currentIndex = combo->findData(defaultValue);
    }
    if (currentIndex >= 0) {
      combo->setCurrentIndex(currentIndex);
    }

    QObject::connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     combo, [store, configKey, combo](int index) {
                       store->set(configKey, combo->itemData(index));
                     });

    QObject::connect(store, &ConfigStore::valueChanged, combo,
                     [combo, configKey](const QString &key,
                                        const QVariant &newValue) {
                       if (key != configKey || !combo) {
                         return;
                       }

                       QSignalBlocker blocker(combo);
                       const int index = combo->findData(newValue);
                       if (index >= 0) {
                         combo->setCurrentIndex(index);
                       }
                     });
  };

  auto createLongPressSubPanel = [this](const QString &iconPath,
                                        const QString &title,
                                        const QString &description,
                                        QWidget *control) {
    auto *panel = new SettingsSubPanel(iconPath, this);

    auto *textContainer = new QWidget(panel);
    textContainer->setSizePolicy(QSizePolicy::Expanding,
                                 QSizePolicy::Preferred);
    auto *textLayout = new QVBoxLayout(textContainer);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(2);

    auto *titleLabel = new QLabel(title, panel);
    titleLabel->setObjectName("SettingsCardTitle");
    titleLabel->setWordWrap(true);
    textLayout->addWidget(titleLabel);

    auto *descLabel = new QLabel(description, panel);
    descLabel->setObjectName("SettingsCardDesc");
    descLabel->setWordWrap(true);
    textLayout->addWidget(descLabel);

    panel->contentLayout()->addWidget(textContainer, 1);

    if (control) {
      control->setParent(panel);
      if (auto *combo = qobject_cast<QComboBox *>(control)) {
        combo->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
      }
      panel->contentLayout()->addWidget(control, 0, Qt::AlignVCenter);
    }

    m_mainLayout->addWidget(panel);
    return panel;
  };

  auto *longPressStepCombo = new ModernComboBox(this);
  longPressStepCombo->addItem(tr("3 Seconds"), "3");
  longPressStepCombo->addItem(tr("5 Seconds"), "5");
  longPressStepCombo->addItem(tr("10 Seconds"), "10");
  longPressStepCombo->addItem(tr("15 Seconds"), "15");
  longPressStepCombo->addItem(tr("30 Seconds"), "30");
  bindPlayerSubPanelCombo(longPressStepCombo, ConfigKeys::PlayerSeekStep,
                          "10");
  auto *longPressStepPanel = createLongPressSubPanel(
      ":/svg/dark/player-seek-step.svg", tr("Seek Step"),
      tr("Number of seconds to skip when pressing forward or rewind"),
      longPressStepCombo);

  auto *longPressTriggerCombo = new ModernComboBox(this);
  longPressTriggerCombo->addItem(tr("300 ms"), "300");
  longPressTriggerCombo->addItem(tr("500 ms"), "500");
  longPressTriggerCombo->addItem(tr("800 ms"), "800");
  longPressTriggerCombo->addItem(tr("1000 ms"), "1000");
  bindPlayerSubPanelCombo(longPressTriggerCombo,
                          ConfigKeys::PlayerLongPressTriggerMs, "500");
  auto *longPressTriggerPanel = createLongPressSubPanel(
      ":/svg/dark/player-long-press-trigger.svg", tr("Long Press Trigger Time"),
      tr("Set how long left/right keys must be held before the long press action starts"),
      longPressTriggerCombo);

  auto *longPressModeCombo = new ModernComboBox(this);
  longPressModeCombo->addItem(tr("Time Jump Playback"), "seek");
  longPressModeCombo->addItem(tr("Speed Playback"), "speed");
  bindPlayerSubPanelCombo(longPressModeCombo, ConfigKeys::PlayerLongPressMode,
                          "seek");
  auto *longPressModePanel = createLongPressSubPanel(
      ":/svg/dark/player-long-press-mode.svg", tr("Long Press Mode"),
      tr("Choose whether holding left/right keys performs time jump seek or temporary speed playback"),
      longPressModeCombo);

  if (ConfigStore::instance()->get<bool>(ConfigKeys::PlayerLongPressSeek,
                                         true)) {
    longPressStepPanel->initExpanded();
    longPressTriggerPanel->initExpanded();
    longPressModePanel->initExpanded();
  }

  connect(longPressSwitch, &ModernSwitch::toggled, longPressStepPanel,
          &SettingsSubPanel::setExpanded);
  connect(longPressSwitch, &ModernSwitch::toggled, longPressTriggerPanel,
          &SettingsSubPanel::setExpanded);
  connect(longPressSwitch, &ModernSwitch::toggled, longPressModePanel,
          &SettingsSubPanel::setExpanded);

  m_mainLayout->addWidget(new SettingsCard(
      ":/svg/dark/player-mouse-edge-long-press.svg",
      tr("Mouse Edge Long Press"),
      tr("Hold the left or right side of the playback area for speed rewind or fast forward"),
      new ModernSwitch(this), ConfigKeys::PlayerMouseEdgeLongPress, this,
      QVariant(true)));

  auto *mediaSwitcherCombo = new ModernComboBox(this);
  mediaSwitcherCombo->addItem(tr("Right Sidebar"), "sidebar");
  mediaSwitcherCombo->addItem(tr("Bottom HUD"), "hud");
  m_mainLayout->addWidget(new SettingsCard(
      ":/svg/dark/player.svg", tr("Media Switcher Position"),
      tr("Choose whether recent movies and season or episode switching appear in the right sidebar or the bottom HUD"),
      mediaSwitcherCombo, ConfigKeys::PlayerMediaSwitcherMode, this,
      QVariant("sidebar")));

  
  m_mainLayout->addWidget(new SettingsCard(
      ":/svg/dark/click-pause.svg", tr("Click to Pause"),
      tr("Single click on the video area to toggle play/pause"),
      new ModernSwitch(this), ConfigKeys::PlayerClickToPause, this,
      QVariant(true)));

  
  m_mainLayout->addWidget(new SettingsCard(
      ":/svg/dark/skip-intro.svg", tr("Auto Skip Intro"),
      tr("Automatically skip the intro sequence when playback starts"),
      new ModernSwitch(this), ConfigKeys::PlayerSkipIntro, this,
      QVariant(false)));

  m_mainLayout->addWidget(new SettingsCard(
      ":/svg/dark/skip-outro.svg", tr("Auto Skip Outro"),
      tr("Automatically skip the ending credits during playback"),
      new ModernSwitch(this), ConfigKeys::PlayerSkipOutro, this,
      QVariant(false)));

  
  // 全局默认跳过时长：某剧未单独设置时使用；按剧覆盖可在播放器的设置菜单
  // （片头片尾跳过）中调整；0 = 不跳过该项。
  {
    auto *durationsWidget = new QWidget(this);
    auto *durationsLayout = new QHBoxLayout(durationsWidget);
    durationsLayout->setContentsMargins(0, 0, 0, 0);
    durationsLayout->setSpacing(10);

    auto *store = SkipSegmentsStore::instance();
    const SkipSegmentsStore::Lengths globalDefaults = store->global();

    auto *introLabel = new QLabel(tr("Intro"), durationsWidget);
    auto *introInput = new ModernNumberInput(durationsWidget);
    introInput->setRange(0, 600);
    introInput->setValue(qBound(0, globalDefaults.introSec, 600));

    auto *outroLabel = new QLabel(tr("Outro"), durationsWidget);
    auto *outroInput = new ModernNumberInput(durationsWidget);
    outroInput->setRange(0, 600);
    outroInput->setValue(qBound(0, globalDefaults.outroSec, 600));

    durationsLayout->addWidget(introLabel);
    durationsLayout->addWidget(introInput);
    durationsLayout->addSpacing(4);
    durationsLayout->addWidget(outroLabel);
    durationsLayout->addWidget(outroInput);
    durationsLayout->addStretch();

    const auto persistGlobalDurations = [store, introInput, outroInput]() {
      store->setGlobal({introInput->value(), outroInput->value()});
    };
    connect(introInput, qOverload<int>(&QSpinBox::valueChanged), this,
            [persistGlobalDurations](int) { persistGlobalDurations(); });
    connect(outroInput, qOverload<int>(&QSpinBox::valueChanged), this,
            [persistGlobalDurations](int) { persistGlobalDurations(); });

    m_mainLayout->addWidget(new SettingsCard(
        ":/svg/dark/skip-intro.svg", tr("Default Skip Durations"),
        tr("Used when a series has no per-series override (adjust per series "
           "from the player's Settings menu). In seconds; 0 disables "
           "skipping."),
        durationsWidget, QString(), this));
  }

  m_mainLayout->addWidget(new SettingsCard(
      ":/svg/dark/window-pop.svg", tr("Independent Player Window"),
      tr("Open video in a separate detached window"), new ModernSwitch(this),
      ConfigKeys::PlayerIndependentWindow, this));

  m_mainLayout->addWidget(new SettingsCard(
      ":/svg/dark/video-output.svg",
      tr("Auto Independent Window for Dolby Vision"),
      tr("Automatically play Dolby Vision sources in the independent player "
         "window. The embedded renderer cannot reproduce Dolby Vision colors "
         "and shows a green tint"),
      new ModernSwitch(this), ConfigKeys::PlayerDvAutoIndependentWindow, this,
      QVariant(true)));

  m_mainLayout->addWidget(new SettingsCard(
      ":/svg/dark/volume-normalize.svg", tr("Volume Normalization"),
      tr("Automatically adjust volume to prevent sudden loud noises"),
      new ModernSwitch(this), ConfigKeys::PlayerVolNormal, this));

  
  auto *mpvConfSwitch = new ModernSwitch(this);
  m_mainLayout->addWidget(new SettingsCard(
      ":/svg/dark/mpv-conf.svg", tr("Use Custom mpv.conf"),
      tr("Load settings from an external mpv.conf configuration file"),
      mpvConfSwitch, ConfigKeys::PlayerUseMpvConf, this));

  
  auto *confPanel = new SettingsSubPanel(":/svg/dark/mpv-conf.svg", this);

  const QString mpvConfDir = AppPaths::dataRoot() + QStringLiteral("/mpv");
  QString mpvConfPath = mpvConfDir + "/mpv.conf";

  auto *confPathLabel = new QLabel(
      tr("Config path: %1").arg(QDir::toNativeSeparators(mpvConfPath)), this);
  confPathLabel->setObjectName("SettingsCardDesc");
  confPathLabel->setWordWrap(true);
  confPanel->contentLayout()->addWidget(confPathLabel, 1);

  auto *openConfBtn = new QPushButton(tr("Open File"), this);
  openConfBtn->setObjectName("SettingsCardButton");
  openConfBtn->setCursor(Qt::PointingHandCursor);
  openConfBtn->setFixedHeight(30);
  confPanel->contentLayout()->addWidget(openConfBtn, 0, Qt::AlignVCenter);

  m_mainLayout->addWidget(confPanel);

  
  if (ConfigStore::instance()->get<bool>(ConfigKeys::PlayerUseMpvConf, false)) {
    confPanel->initExpanded();
  }

  
  connect(mpvConfSwitch, &ModernSwitch::toggled, confPanel,
          &SettingsSubPanel::setExpanded);

  
  connect(openConfBtn, &QPushButton::clicked, this,
          [mpvConfDir, mpvConfPath]() {
            QDir().mkpath(mpvConfDir);
            if (!QFile::exists(mpvConfPath)) {
              QFile file(mpvConfPath);
              if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream out(&file);
                out << "# MPV Configuration File for ReEmby\n";
                out << "# Uncomment or add options below, one per line.\n";
                out << "# Reference: https://mpv.io/manual/stable/#options\n";
                out << "#\n";
                out << "# hwdec=auto-copy\n";
                out << "# video-sync=display-resample\n";
                out << "# deband=yes\n";
                file.close();
              }
            }
            QDesktopServices::openUrl(QUrl::fromLocalFile(mpvConfPath));
          });

  auto bindSettingsControl = [](QWidget *control, const QString &configKey,
                                const QVariant &defaultValue = QVariant()) {
    if (!control || configKey.isEmpty()) {
      return;
    }

    auto *store = ConfigStore::instance();

    if (auto *switchControl = qobject_cast<ModernSwitch *>(control)) {
      switchControl->setChecked(
          store->get<bool>(configKey, defaultValue.toBool()));
      connect(switchControl, &ModernSwitch::toggled, control,
              [store, configKey](bool checked) {
                store->set(configKey, checked);
              });
    } else if (auto *comboControl = qobject_cast<QComboBox *>(control)) {
      QString currentVal = store->get<QString>(configKey);
      if (currentVal.isEmpty() && defaultValue.isValid()) {
        currentVal = defaultValue.toString();
      }
      const int idx = comboControl->findData(currentVal);
      if (idx >= 0) {
        comboControl->setCurrentIndex(idx);
      }

      connect(comboControl,
              QOverload<int>::of(&QComboBox::currentIndexChanged), control,
              [store, configKey, comboControl](int index) {
                store->set(configKey, comboControl->itemData(index));
              });
    } else if (auto *lineEdit = qobject_cast<QLineEdit *>(control)) {
      lineEdit->setText(
          store->get<QString>(configKey, defaultValue.toString()));
      connect(lineEdit, &QLineEdit::editingFinished, control,
              [store, configKey, lineEdit]() {
                store->set(configKey, lineEdit->text());
              });
    } else if (auto *tagInput = qobject_cast<ModernTagInput *>(control)) {
      tagInput->setValue(
          store->get<QString>(configKey, defaultValue.toString()));
      connect(tagInput, &ModernTagInput::valueChanged, control,
              [store, configKey](const QString &value) {
                store->set(configKey, value);
              });
    }

    connect(store, &ConfigStore::valueChanged, control,
            [control, configKey](const QString &key, const QVariant &newValue) {
              if (key != configKey || !control) {
                return;
              }

              QSignalBlocker blocker(control);
              if (auto *switchControl = qobject_cast<ModernSwitch *>(control)) {
                switchControl->setChecked(newValue.toBool());
              } else if (auto *comboControl =
                             qobject_cast<QComboBox *>(control)) {
                const int idx = comboControl->findData(newValue);
                if (idx >= 0) {
                  comboControl->setCurrentIndex(idx);
                }
              } else if (auto *lineEdit =
                             qobject_cast<QLineEdit *>(control)) {
                lineEdit->setText(newValue.toString());
              } else if (auto *tagInput =
                             qobject_cast<ModernTagInput *>(control)) {
                tagInput->setValue(newValue.toString());
              }
            });
  };

  auto *subtitleTitle = new QLabel(tr("Subtitles"), this);
  subtitleTitle->setObjectName("SettingsSubTitle");
  m_mainLayout->addWidget(subtitleTitle);

  m_mainLayout->addWidget(new SettingsCard(
      ":/svg/dark/subtitle-lang.svg", tr("Enable Secondary Subtitle"),
      tr("Show a second content subtitle track. Adds secondary subtitle "
         "selection to the player subtitle menu, with its own style options "
         "in subtitle settings"),
      new ModernSwitch(this), ConfigKeys::PlayerSubtitleSecondaryEnabled, this,
      QVariant(false)));

  // 「允许拖动字幕位置」设置项已移除：该功能已放弃（拖动逻辑保留在
  // PlayerView 中，配置默认关闭，不再对外暴露开关）。

  auto *danmakuTitle = new QLabel(tr("Danmaku"), this);
  danmakuTitle->setObjectName("SettingsSubTitle");
  m_mainLayout->addWidget(danmakuTitle);

  const QString danmakuEnabledKey =
      ConfigKeys::forServer(sid, ConfigKeys::PlayerDanmakuEnabled);
  const bool danmakuEnabled = ConfigStore::instance()->get<bool>(
      danmakuEnabledKey,
      ConfigStore::instance()->get<bool>(ConfigKeys::PlayerDanmakuEnabled,
                                         false));
  auto *danmakuSwitch = new ModernSwitch(this);
  auto *danmakuSettingsCard = new SettingsCard(
      ":/svg/dark/danmaku.svg", tr("Enable Danmaku"),
      tr("Display streaming danmaku in the embedded player"),
      danmakuSwitch, danmakuEnabledKey, this,
      QVariant(danmakuEnabled));
  m_mainLayout->addWidget(danmakuSettingsCard);

  QList<SettingsSubPanel *> danmakuPanels;
  auto addDanmakuSubPanel = [&](const QString &iconPath, const QString &title,
                                const QString &description, QWidget *control,
                                const QString &configKey = QString(),
                                const QVariant &defaultValue = QVariant()) {
    auto *panel = new SettingsSubPanel(iconPath, this);

    auto *textContainer = new QWidget(panel);
    textContainer->setSizePolicy(QSizePolicy::Expanding,
                                 QSizePolicy::Preferred);
    auto *textLayout = new QVBoxLayout(textContainer);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(2);

    auto *titleLabel = new QLabel(title, panel);
    titleLabel->setObjectName("SettingsCardTitle");
    titleLabel->setWordWrap(true);
    textLayout->addWidget(titleLabel);

    if (!description.isEmpty()) {
      auto *descLabel = new QLabel(description, panel);
      descLabel->setObjectName("SettingsCardDesc");
      descLabel->setWordWrap(true);
      textLayout->addWidget(descLabel);
    }

    panel->contentLayout()->addWidget(textContainer, 1);

    if (control) {
      control->setParent(panel);
      if (auto *combo = qobject_cast<QComboBox *>(control)) {
        combo->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
      } else if (auto *btn = qobject_cast<QPushButton *>(control)) {
        btn->setObjectName("SettingsCardButton");
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedHeight(30);
      } else if (auto *lineEdit = qobject_cast<QLineEdit *>(control)) {
        lineEdit->setMinimumWidth(200);
        lineEdit->setFixedHeight(32);
        lineEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
      } else if (qobject_cast<ModernTagInput *>(control)) {
        control->setMinimumWidth(qMax(control->minimumWidth(), 220));
        control->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
      }

      panel->contentLayout()->addWidget(control, 0, Qt::AlignVCenter);
      bindSettingsControl(control, configKey, defaultValue);
    }

    m_mainLayout->addWidget(panel);
    danmakuPanels.append(panel);
    return panel;
  };

  auto addDanmakuSliderSubPanel =
      [&](const QString &iconPath, const QString &title,
          const QString &description, DanmakuOptionUtils::SliderKind kind,
          const QString &configKey) {
        auto *control = new DanmakuOptionSlider(kind, configKey, this);
        return addDanmakuSubPanel(iconPath, title, description, control);
      };

  auto *danmakuRendererCombo = new ModernComboBox(this);
  danmakuRendererCombo->addItem(tr("ASS Subtitle Track"),
                                DanmakuRendererUtils::assTrackRendererId());
  danmakuRendererCombo->addItem(
      tr("Native Smooth Renderer"),
      DanmakuRendererUtils::nativeSmoothRendererId());
  addDanmakuSubPanel(
      ":/svg/dark/danmaku.svg", tr("Danmaku Renderer"),
      tr("Choose whether danmaku is rendered through the ASS subtitle track or ReEmby's adaptive native renderer"),
      danmakuRendererCombo, ConfigKeys::PlayerDanmakuRenderer,
      QVariant(DanmakuRendererUtils::defaultRendererId()));

  // Bilibili danmaku source toggle (built-in, opt-in; requires a logged-in
  // BiliBili account). The state lives in the bilibili entry of the per-server
  // danmaku server list, so toggling here just calls saveServers().
  const auto bilibiliServerIndex = [sid]() {
    QList<DanmakuServerDefinition> servers =
        DanmakuSettings::loadServers(sid);
    for (int i = 0; i < servers.size(); ++i) {
      if (servers.at(i).id == QLatin1String("bilibili") ||
          servers.at(i).provider.trimmed().toLower() ==
              QLatin1String("bilibili")) {
        return i;
      }
    }
    return -1;
  };
  auto *bilibiliSwitch = new ModernSwitch(this);
  bilibiliSwitch->setChecked(
      bilibiliServerIndex() >= 0
          ? DanmakuSettings::loadServers(sid).at(bilibiliServerIndex()).enabled
          : false);
  connect(bilibiliSwitch, &ModernSwitch::toggled, this, [sid](bool on) {
    QList<DanmakuServerDefinition> servers =
        DanmakuSettings::loadServers(sid);
    for (DanmakuServerDefinition &server : servers) {
      if (server.id == QLatin1String("bilibili") ||
          server.provider.trimmed().toLower() ==
              QLatin1String("bilibili")) {
        server.enabled = on;
        break;
      }
    }
    DanmakuSettings::saveServers(sid, servers, QString());
    ModernToast::showMessage(
        on ? tr("BiliBili danmaku enabled")
           : tr("BiliBili danmaku disabled"),
        2000);
  });
  addDanmakuSubPanel(
      ":/svg/dark/danmaku.svg", tr("Enable BiliBili Danmaku"),
      tr("Requires a logged-in BiliBili account (Settings -> BiliBili). When enabled, matched anime will also pull danmaku from BiliBili."),
      bilibiliSwitch);

  auto *danmakuServerPanel =
      new SettingsSubPanel(":/svg/dark/danmaku-server.svg", this);
  auto *danmakuServerTextContainer = new QWidget(danmakuServerPanel);
  danmakuServerTextContainer->setSizePolicy(QSizePolicy::Expanding,
                                            QSizePolicy::Preferred);
  auto *danmakuServerTextLayout =
      new QVBoxLayout(danmakuServerTextContainer);
  danmakuServerTextLayout->setContentsMargins(0, 0, 0, 0);
  danmakuServerTextLayout->setSpacing(2);

  auto *danmakuServerTitleLabel =
      new QLabel(tr("Danmaku Server"), danmakuServerPanel);
  danmakuServerTitleLabel->setObjectName("SettingsCardTitle");
  danmakuServerTitleLabel->setWordWrap(true);
  danmakuServerTextLayout->addWidget(danmakuServerTitleLabel);

  auto *danmakuServerDescLabel = new QLabel(danmakuServerPanel);
  danmakuServerDescLabel->setObjectName("SettingsCardDesc");
  danmakuServerDescLabel->setWordWrap(true);
  danmakuServerTextLayout->addWidget(danmakuServerDescLabel);

  danmakuServerPanel->contentLayout()->addWidget(danmakuServerTextContainer,
                                                 1);

  auto *manageDanmakuServerBtn =
      new QPushButton(tr("Manage"), danmakuServerPanel);
  manageDanmakuServerBtn->setObjectName("SettingsCardButton");
  manageDanmakuServerBtn->setCursor(Qt::PointingHandCursor);
  manageDanmakuServerBtn->setFixedHeight(30);
  danmakuServerPanel->contentLayout()->addWidget(manageDanmakuServerBtn, 0,
                                                 Qt::AlignVCenter);

  m_mainLayout->addWidget(danmakuServerPanel);
  danmakuPanels.append(danmakuServerPanel);

  // Danmaku sources are shared globally across all Emby servers now
  // (DanmakuSettings::loadServers/saveServers use the account-level
  // danmaku/servers + danmaku/selected_server keys), so the UI listens
  // to the global keys, not the per-server ones.
  const QString danmakuServersKey =
      QString::fromLatin1(ConfigKeys::DanmakuServers);
  const QString danmakuSelectedServerKey =
      QString::fromLatin1(ConfigKeys::DanmakuSelectedServer);
  const auto updateDanmakuServerSummary =
      [this, sid, danmakuServerDescLabel]() {
        const DanmakuServerDefinition selectedServer =
            DanmakuSettings::selectedServer(sid);
        QString serverName = selectedServer.name.trimmed();
        if (serverName == QStringLiteral("DandanPlay Open API")) {
          serverName = tr("DandanPlay Open API");
        }
        if (serverName.isEmpty()) {
          serverName = selectedServer.baseUrl.trimmed();
        }

        const bool isDandanPlay =
            selectedServer.provider.trimmed().compare(
                QStringLiteral("dandanplay"), Qt::CaseInsensitive) == 0;
        const bool isDanmuApi =
            selectedServer.provider.trimmed().compare(
                QStringLiteral("danmu_api"), Qt::CaseInsensitive) == 0;
        const QList<DanmakuServerDefinition> servers =
            DanmakuSettings::loadServers(sid);
        const int enabledServerCount =
            std::count_if(servers.cbegin(), servers.cend(),
                          [](const DanmakuServerDefinition &server) {
                            return server.enabled;
                          });
        QStringList summaryLines;
        summaryLines.append(tr("Preferred server: %1").arg(serverName));
        summaryLines.append(
            tr("Server type: %1")
                .arg(isDanmuApi ? tr("LogVar / danmu_api")
                                : tr("DandanPlay")));
        summaryLines.append(
            tr("Address: %1").arg(selectedServer.baseUrl.trimmed()));
        summaryLines.append(
            tr("Enabled servers: %1").arg(enabledServerCount));
        if (isDandanPlay && selectedServer.contentScope.trimmed().compare(
                                QStringLiteral("anime"),
                                Qt::CaseInsensitive) == 0) {
          summaryLines.append(tr("Supported content: Anime only"));
        } else if (isDanmuApi) {
          summaryLines.append(tr("Supported content: General video"));
        }
        if (!selectedServer.builtIn &&
            !selectedServer.description.trimmed().isEmpty()) {
          summaryLines.append(
              tr("Description: %1").arg(selectedServer.description.trimmed()));
        }
        if (isDandanPlay && !selectedServer.builtIn &&
            (selectedServer.appId.trimmed().isEmpty() ||
             selectedServer.appSecret.trimmed().isEmpty())) {
          summaryLines.append(
              tr("App ID and App Secret are required for official DandanPlay Open API access"));
        }

        danmakuServerDescLabel->setText(summaryLines.join(QStringLiteral("\n")));
      };
  updateDanmakuServerSummary();

  connect(manageDanmakuServerBtn, &QPushButton::clicked, this,
          [this, sid, updateDanmakuServerSummary]() {
            DanmakuServerDialog dialog(this);
            dialog.setServers(DanmakuSettings::loadServers(sid),
                              DanmakuSettings::selectedServerId(sid));
            if (dialog.exec() != QDialog::Accepted) {
              return;
            }

            DanmakuSettings::saveServers(sid, dialog.servers(),
                                         dialog.selectedServerId());
            updateDanmakuServerSummary();
          });

  connect(ConfigStore::instance(), &ConfigStore::valueChanged,
          danmakuServerPanel,
          [danmakuServersKey, danmakuSelectedServerKey,
           updateDanmakuServerSummary](const QString &key, const QVariant &) {
            if (key == danmakuServersKey || key == danmakuSelectedServerKey) {
              updateDanmakuServerSummary();
            }
          });

  auto *danmakuSourceModeCombo = new ModernComboBox(this);
  danmakuSourceModeCombo->addItem(tr("Prefer Local"), "prefer-local");
  danmakuSourceModeCombo->addItem(tr("Prefer Online"), "prefer-online");
  danmakuSourceModeCombo->addItem(tr("Online Only"), "online-only");
  danmakuSourceModeCombo->addItem(tr("Local Only"), "local-only");
  addDanmakuSubPanel(
      ":/svg/dark/danmaku-related.svg", tr("Danmaku Source"),
      tr("Choose whether local danmaku files or online danmaku should be preferred during matching"),
      danmakuSourceModeCombo,
      ConfigKeys::forServer(sid, ConfigKeys::DanmakuSourceMode),
      QVariant("prefer-local"));

  auto *localDanmakuPanel =
      new SettingsSubPanel(":/svg/dark/danmaku.svg", this);
  auto *localDanmakuTextContainer = new QWidget(localDanmakuPanel);
  localDanmakuTextContainer->setSizePolicy(QSizePolicy::Expanding,
                                           QSizePolicy::Preferred);
  auto *localDanmakuTextLayout =
      new QVBoxLayout(localDanmakuTextContainer);
  localDanmakuTextLayout->setContentsMargins(0, 0, 0, 0);
  localDanmakuTextLayout->setSpacing(2);

  auto *localDanmakuTitleLabel =
      new QLabel(tr("Local Danmaku Folder"), localDanmakuPanel);
  localDanmakuTitleLabel->setObjectName("SettingsCardTitle");
  localDanmakuTitleLabel->setWordWrap(true);
  localDanmakuTextLayout->addWidget(localDanmakuTitleLabel);

  const QString localDanmakuDir =
      DanmakuService::localDanmakuDirectoryPath(sid);
  auto *localDanmakuDescLabel = new QLabel(localDanmakuPanel);
  localDanmakuDescLabel->setObjectName("SettingsCardDesc");
  localDanmakuDescLabel->setWordWrap(true);
  localDanmakuDescLabel->setText(
      tr("Place .ass, .json, or .xml danmaku files here. Matching uses the current media title and manual keyword, and the selected danmaku source mode decides whether local files or online results are preferred first.\nFolder: %1")
          .arg(QDir::toNativeSeparators(localDanmakuDir)));
  localDanmakuTextLayout->addWidget(localDanmakuDescLabel);

  localDanmakuPanel->contentLayout()->addWidget(localDanmakuTextContainer, 1);

  auto *openLocalDanmakuFolderBtn =
      new QPushButton(tr("Open Folder"), localDanmakuPanel);
  openLocalDanmakuFolderBtn->setObjectName("SettingsCardButton");
  openLocalDanmakuFolderBtn->setCursor(Qt::PointingHandCursor);
  openLocalDanmakuFolderBtn->setFixedHeight(30);
  localDanmakuPanel->contentLayout()->addWidget(openLocalDanmakuFolderBtn, 0,
                                                Qt::AlignVCenter);

  m_mainLayout->addWidget(localDanmakuPanel);
  danmakuPanels.append(localDanmakuPanel);

  connect(openLocalDanmakuFolderBtn, &QPushButton::clicked, this,
          [sid]() {
            if (!DanmakuService::ensureLocalDanmakuDirectory(sid)) {
              return;
            }

            const QString folderPath =
                DanmakuService::localDanmakuDirectoryPath(sid);
            const QString readmePath = folderPath + QStringLiteral("/README.txt");
            if (!QFile::exists(readmePath)) {
              QFile readmeFile(readmePath);
              if (readmeFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream out(&readmeFile);
                out << "ReEmby local danmaku folder\n";
                out << "\n";
                out << "Supported file types:\n";
                out << "- .ass : attached directly to the embedded player\n";
                out << "- .json: parsed into danmaku comments and rendered to ASS\n";
                out << "- .xml : Bilibili-style XML danmaku parsed and rendered to ASS\n";
                out << "\n";
                out << "Source mode:\n";
                out << "- Set Danmaku Source in Player settings to choose local-first, online-first, or local-only behavior\n";
                out << "\n";
                out << "Matching rules:\n";
                out << "- Filenames are matched against the current media title\n";
                out << "- The player's manual danmaku keyword is also matched against filenames\n";
                out << "- Use filenames close to the video title for best results\n";
                out << "\n";
                out << "Simple JSON example:\n";
                out << "{\n";
                out << "  \"comments\": [\n";
                out << "    {\"timeMs\": 1200, \"text\": \"First danmaku\"},\n";
                out << "    {\"timeMs\": 3500, \"text\": \"Top danmaku\", \"mode\": 5, \"color\": \"#FFD54F\"},\n";
                out << "    {\"timeMs\": 5200, \"text\": \"Bottom danmaku\", \"mode\": 4}\n";
                out << "  ]\n";
                out << "}\n";
              }
            }

            QDesktopServices::openUrl(QUrl::fromLocalFile(folderPath));
          });

  addDanmakuSubPanel(
      ":/svg/dark/danmaku-auto-load.svg", tr("Auto Load Danmaku"),
      tr("Automatically search and load danmaku when playback starts"),
      new ModernSwitch(this),
      ConfigKeys::forServer(sid, ConfigKeys::DanmakuAutoLoad), QVariant(true));

  addDanmakuSubPanel(
      ":/svg/dark/danmaku-auto-match.svg", tr("Auto Match Danmaku"),
      tr("Automatically choose the best danmaku match when no cached manual result exists"),
      new ModernSwitch(this),
      ConfigKeys::forServer(sid, ConfigKeys::DanmakuAutoMatch),
      QVariant(true));

  addDanmakuSubPanel(
      ":/svg/dark/danmaku-related.svg", tr("Load Related Comments"),
      tr("Include related third-party comment sources when the provider supports it"),
      new ModernSwitch(this),
      ConfigKeys::forServer(sid, ConfigKeys::DanmakuWithRelated),
      QVariant(true));

  addDanmakuSubPanel(
      ":/svg/dark/danmaku-dual-subtitle.svg", tr("Dual Subtitle Mode"),
      tr("Keep regular subtitles visible alongside danmaku when possible"),
      new ModernSwitch(this), ConfigKeys::PlayerDanmakuDualSubtitle,
      QVariant(true));

  addDanmakuSliderSubPanel(
      ":/svg/dark/danmaku-opacity.svg", tr("Danmaku Opacity"),
      tr("Control the transparency of rendered danmaku"),
      DanmakuOptionUtils::SliderKind::Opacity,
      ConfigKeys::PlayerDanmakuOpacity);

  addDanmakuSliderSubPanel(
      ":/svg/dark/danmaku-size.svg", tr("Danmaku Size"),
      tr("Scale the font size used for danmaku rendering"),
      DanmakuOptionUtils::SliderKind::FontScale,
      ConfigKeys::PlayerDanmakuFontScale);

  addDanmakuSliderSubPanel(
      ":/svg/dark/danmaku-area.svg", tr("Display Area"),
      tr("Limit how much of the screen height danmaku can occupy"),
      DanmakuOptionUtils::SliderKind::Area,
      ConfigKeys::PlayerDanmakuAreaPercent);

  addDanmakuSliderSubPanel(
      ":/svg/dark/danmaku-density.svg", tr("Danmaku Density"),
      tr("Reduce or increase the number of simultaneous lanes"),
      DanmakuOptionUtils::SliderKind::Density,
      ConfigKeys::PlayerDanmakuDensity);

  addDanmakuSliderSubPanel(
      ":/svg/dark/danmaku-speed.svg", tr("Danmaku Speed"),
      tr("Adjust how quickly scrolling danmaku travels across the screen"),
      DanmakuOptionUtils::SliderKind::SpeedScale,
      ConfigKeys::PlayerDanmakuSpeedScale);

  addDanmakuSliderSubPanel(
      ":/svg/dark/danmaku-offset.svg", tr("Danmaku Offset"),
      tr("Shift danmaku timing earlier or later to align with playback"),
      DanmakuOptionUtils::SliderKind::OffsetMs,
      ConfigKeys::PlayerDanmakuOffsetMs);

  addDanmakuSubPanel(
      ":/svg/dark/danmaku-hide-scroll.svg", tr("Hide Scrolling Danmaku"),
      tr("Hide standard right-to-left scrolling comments"),
      new ModernSwitch(this), ConfigKeys::PlayerDanmakuHideScroll,
      QVariant(false));

  addDanmakuSubPanel(
      ":/svg/dark/danmaku-hide-top.svg", tr("Hide Top Danmaku"),
      tr("Hide fixed danmaku positioned at the top of the screen"),
      new ModernSwitch(this), ConfigKeys::PlayerDanmakuHideTop,
      QVariant(false));

  addDanmakuSubPanel(
      ":/svg/dark/danmaku-hide-bottom.svg", tr("Hide Bottom Danmaku"),
      tr("Hide fixed danmaku positioned at the bottom of the screen"),
      new ModernSwitch(this), ConfigKeys::PlayerDanmakuHideBottom,
      QVariant(false));

  auto *blockedKeywordsInput = new ModernTagInput(this);
  blockedKeywordsInput->setMinimumWidth(360);
  addDanmakuSubPanel(
      ":/svg/dark/danmaku-blocked.svg", tr("Blocked Keywords"),
      tr("Filter out danmaku containing these keywords"),
      blockedKeywordsInput, ConfigKeys::PlayerDanmakuBlockedKeywords);

  auto *cacheHoursCombo = new ModernComboBox(this);
  cacheHoursCombo->addItem(tr("6 Hours"), "6");
  cacheHoursCombo->addItem(tr("12 Hours"), "12");
  cacheHoursCombo->addItem(tr("1 Day"), "24");
  cacheHoursCombo->addItem(tr("3 Days"), "72");
  cacheHoursCombo->addItem(tr("7 Days"), "168");
  addDanmakuSubPanel(
      ":/svg/dark/danmaku-cache-duration.svg", tr("Danmaku Cache Duration"),
      tr("Reuse online match results, cached comment payloads, and generated ASS files for this long"),
      cacheHoursCombo,
      ConfigKeys::forServer(sid, ConfigKeys::DanmakuCacheHours),
      QVariant("24"));

  auto *clearDanmakuCacheBtn = new QPushButton(tr("Clear"), this);
  addDanmakuSubPanel(
      ":/svg/dark/danmaku-cache-clear.svg", tr("Clear Danmaku Cache"),
      tr("Remove cached online matches, cached comment payloads, and generated ASS files"),
      clearDanmakuCacheBtn);
  connect(clearDanmakuCacheBtn, &QPushButton::clicked, this, [this]() {
    if (m_core && m_core->danmakuService()) {
      m_core->danmakuService()->clearCache();
      ModernToast::showMessage(tr("Danmaku cache cleared"));
    }
  });

  const auto updateDanmakuPanels = [danmakuPanels](bool enabled) {
    for (auto *panel : danmakuPanels) {
      if (!panel) {
        continue;
      }
      panel->setExpandedImmediately(enabled);
    }
  };

  if (danmakuEnabled) {
    updateDanmakuPanels(true);
  }

  connect(danmakuSwitch, &ModernSwitch::toggled, this,
          [updateDanmakuPanels](bool enabled) {
            updateDanmakuPanels(enabled);
          });
  connect(ConfigStore::instance(), &ConfigStore::valueChanged, this,
          [danmakuEnabledKey, updateDanmakuPanels](const QString &key,
                                                   const QVariant &newValue) {
            if (key == danmakuEnabledKey) {
              updateDanmakuPanels(newValue.toBool());
            }
          });

  
  auto *extTitle = new QLabel(tr("External Player"), this);
  extTitle->setObjectName("SettingsSubTitle");
  m_mainLayout->addWidget(extTitle);

  QStringList supportedExternalPlayers;
#ifdef Q_OS_WIN
  supportedExternalPlayers = {QStringLiteral("PotPlayer"),
                              QStringLiteral("VLC"),
                              QStringLiteral("MPC-HC"),
                              QStringLiteral("MPC-BE"),
                              QStringLiteral("MPV")};
#elif defined(Q_OS_MAC)
  supportedExternalPlayers = {QStringLiteral("MPV"),
                              QStringLiteral("VLC"),
                              QStringLiteral("IINA")};
#else
  supportedExternalPlayers = {QStringLiteral("MPV"),
                              QStringLiteral("VLC"),
                              QStringLiteral("MPC-Qt")};
#endif
  const QString externalPlayerSupportText =
      tr("Supported players on this platform: %1")
          .arg(supportedExternalPlayers.join(QStringLiteral(", ")));

  auto *extSwitch = new ModernSwitch(this);
  m_mainLayout->addWidget(new SettingsCard(
      ":/svg/dark/external-player.svg", tr("Enable External Player"),
      externalPlayerSupportText, extSwitch, ConfigKeys::ExtPlayerEnable,
      this));

  

  
  auto *playerPanel =
      new SettingsSubPanel(":/svg/dark/external-player.svg", this);
  auto *playerCombo = new PlayerComboBox(this);

  
  auto syncAllPlayersToConfig = [](PlayerComboBox *combo) {
    QJsonArray arr;
    for (int i = 0; i < combo->count(); ++i) {
      QString source =
          combo->itemData(i, PlayerComboBox::SourceRole).toString();
      if (source == PlayerComboBox::SourceCustom)
        continue;
      QString path = combo->itemData(i).toString();
      if (path.isEmpty())
        continue;
      QJsonObject obj;
      obj["name"] = QFileInfo(path).baseName();
      obj["path"] = path;
      arr.append(obj);
    }
    ConfigStore::instance()->set(
        ConfigKeys::ExtPlayerDetectedList,
        QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
  };

  
  
  
  
  
  
  QList<DetectedPlayer> detectedPlayers =
      ExternalPlayerDetector::loadFromConfig();
  QString currentPath =
      ConfigStore::instance()->get<QString>(ConfigKeys::ExtPlayerPath);
  int selectedIdx = -1;

  for (int i = 0; i < detectedPlayers.size(); ++i) {
    const auto &p = detectedPlayers[i];
    playerCombo->addPlayer(QString("%1  (%2)").arg(p.name, p.path), p.path,
                           PlayerComboBox::SourceAuto);
    if (p.path == currentPath)
      selectedIdx = i;
  }
  
  playerCombo->addPlayer(tr("Custom..."), "custom",
                         PlayerComboBox::SourceCustom);

  
  if (selectedIdx >= 0) {
    playerCombo->setCurrentIndex(selectedIdx);
  } else if (!currentPath.isEmpty() && currentPath != "custom") {
    playerCombo->addPlayer(QDir::toNativeSeparators(currentPath), currentPath,
                           PlayerComboBox::SourceManual);
    
    int manualIdx = playerCombo->findData(currentPath);
    if (manualIdx >= 0)
      playerCombo->setCurrentIndex(manualIdx);
  }

  
  
  qDebug() << "[PagePlayer] Async external player detection scheduled"
           << "| cached count:" << detectedPlayers.size();
  auto *detectWatcher = new QFutureWatcher<QList<DetectedPlayer>>(this);
  QPointer<PlayerComboBox> safeCombo(playerCombo);
  connect(detectWatcher,
          &QFutureWatcher<QList<DetectedPlayer>>::finished, this,
          [detectWatcher, safeCombo, syncAllPlayersToConfig]() {
            detectWatcher->deleteLater();
            if (!safeCombo) {
              qDebug() << "[PagePlayer] Detection finished but combo was "
                          "destroyed; skip refresh";
              return;
            }
            const QList<DetectedPlayer> fresh = detectWatcher->result();
            qDebug() << "[PagePlayer] External player detection finished"
                     << "| detected count:" << fresh.size();

            const QString prevPath = safeCombo->currentPlayerPath();

            
            
            QSignalBlocker blocker(safeCombo);
            safeCombo->removeAutoDetected();

            
            
            for (const auto &p : fresh) {
              safeCombo->addPlayer(QString("%1  (%2)").arg(p.name, p.path),
                                   p.path, PlayerComboBox::SourceAuto);
            }

            
            const int restoreIdx = safeCombo->findData(prevPath);
            if (restoreIdx >= 0) {
              safeCombo->setCurrentIndex(restoreIdx);
            } else if (safeCombo->count() > 0) {
              safeCombo->setCurrentIndex(0);
            }

            
            syncAllPlayersToConfig(safeCombo);
            ExternalPlayerDetector::saveToConfig(fresh);
          });

  detectWatcher->setFuture(QtConcurrent::run(
      []() { return ExternalPlayerDetector::detect(); }));

  auto *playerLabel = new QLabel(tr("Player"), this);
  playerLabel->setObjectName("SettingsCardDesc");

  
  auto *searchBtn = new QPushButton(this);
  searchBtn->setObjectName("SettingsIconBtn");
  searchBtn->setCursor(Qt::PointingHandCursor);
  searchBtn->setFixedSize(28, 28);
  searchBtn->setToolTip(tr("Re-detect installed players"));

  playerPanel->contentLayout()->addWidget(playerLabel);
  playerPanel->contentLayout()->addWidget(playerCombo);
  playerPanel->contentLayout()->addWidget(searchBtn);

  m_mainLayout->addWidget(playerPanel);

  
  connect(
      searchBtn, &QPushButton::clicked, this,
      [this, playerCombo, syncAllPlayersToConfig, searchBtn]() {
        
        bool reduceAnimations =
            ConfigStore::instance()->get<bool>(ConfigKeys::UiAnimations, false);
        if (!reduceAnimations) {
          
          auto originalPix = std::make_shared<QPixmap>(
              searchBtn->icon().pixmap(searchBtn->iconSize()));

          auto *rotAnim = new QVariantAnimation(searchBtn);
          rotAnim->setStartValue(0.0);
          rotAnim->setEndValue(360.0);
          rotAnim->setDuration(500);
          rotAnim->setEasingCurve(QEasingCurve::InOutCubic);

          connect(rotAnim, &QVariantAnimation::valueChanged, searchBtn,
                  [searchBtn, originalPix](const QVariant &val) {
                    qreal angle = val.toReal();
                    QTransform rot;
                    rot.rotate(angle);
                    QPixmap rotated =
                        originalPix->transformed(rot, Qt::SmoothTransformation);
                    
                    int dx = (rotated.width() - originalPix->width()) / 2;
                    int dy = (rotated.height() - originalPix->height()) / 2;
                    QPixmap cropped = rotated.copy(dx, dy, originalPix->width(),
                                                   originalPix->height());
                    searchBtn->setIcon(QIcon(cropped));
                  });

          connect(rotAnim, &QVariantAnimation::finished, searchBtn,
                  [searchBtn]() {
                    
                    searchBtn->style()->unpolish(searchBtn);
                    searchBtn->style()->polish(searchBtn);
                  });

          rotAnim->start(QAbstractAnimation::DeleteWhenStopped);
        }

        
        QString prevPath = playerCombo->currentPlayerPath();

        
        
        playerCombo->blockSignals(true);
        playerCombo->removeAutoDetected();

        QList<DetectedPlayer> newPlayers = ExternalPlayerDetector::detect();
        for (const auto &p : newPlayers) {
          playerCombo->addPlayer(QString("%1  (%2)").arg(p.name, p.path),
                                 p.path, PlayerComboBox::SourceAuto);
        }

        
        int prevIdx = playerCombo->findData(prevPath);
        if (prevIdx >= 0) {
          playerCombo->setCurrentIndex(prevIdx);
        } else if (playerCombo->count() > 0) {
          playerCombo->setCurrentIndex(0);
        }
        playerCombo->blockSignals(false);

        
        syncAllPlayersToConfig(playerCombo);

        
        QString syncPath = playerCombo->currentPlayerPath();
        if (syncPath != "custom") {
          ConfigStore::instance()->set(ConfigKeys::ExtPlayerPath, syncPath);
        }
      });

  
  connect(playerCombo, &PlayerComboBox::playerRemoved, this,
          [playerCombo, syncAllPlayersToConfig]() {
            syncAllPlayersToConfig(playerCombo);
            ConfigStore::instance()->set(ConfigKeys::ExtPlayerPath,
                                         playerCombo->currentPlayerPath());
          });

  
  connect(
      playerCombo, QOverload<int>::of(&QComboBox::activated), this,
      [this, playerCombo, syncAllPlayersToConfig](int idx) {
        if (idx < 0)
          return;
        QString source =
            playerCombo->itemData(idx, PlayerComboBox::SourceRole).toString();
        QString data = playerCombo->itemData(idx).toString();

        if (source == PlayerComboBox::SourceCustom) {
          QString filePath = QFileDialog::getOpenFileName(
              this, tr("Select External Player"), QString(),
#ifdef Q_OS_WIN
              tr("Executable Files (*.exe)")
#else
              tr("All Files (*)")
#endif
          );
          if (!filePath.isEmpty()) {
            filePath = QDir::toNativeSeparators(filePath);
            playerCombo->blockSignals(true);
            playerCombo->addPlayer(filePath, filePath,
                                   PlayerComboBox::SourceManual);
            int newIdx = playerCombo->findData(filePath);
            playerCombo->setCurrentIndex(newIdx);
            playerCombo->blockSignals(false);
            syncAllPlayersToConfig(playerCombo);
            ConfigStore::instance()->set(ConfigKeys::ExtPlayerPath, filePath);
          } else {
            
            QString prevPath = ConfigStore::instance()->get<QString>(
                ConfigKeys::ExtPlayerPath);
            int prevIdx = playerCombo->findData(prevPath);
            if (prevIdx >= 0) {
              playerCombo->blockSignals(true);
              playerCombo->setCurrentIndex(prevIdx);
              playerCombo->blockSignals(false);
            }
          }
        } else {
          ConfigStore::instance()->set(ConfigKeys::ExtPlayerPath, data);
        }
      });

  
  auto *argsPanel = new SettingsSubPanel(":/svg/dark/player-args.svg", this);
  auto *argsLabel = new QLabel(tr("Arguments"), this);
  argsLabel->setObjectName("SettingsCardDesc");
  auto *argsEdit = new QLineEdit(this);
  argsEdit->setPlaceholderText(tr("e.g., --fullscreen --volume=100"));
  argsEdit->setMinimumWidth(200);
  argsEdit->setFixedHeight(32);
  argsEdit->setText(
      ConfigStore::instance()->get<QString>(ConfigKeys::ExtPlayerArgs));

  connect(argsEdit, &QLineEdit::editingFinished, this, [argsEdit]() {
    ConfigStore::instance()->set(ConfigKeys::ExtPlayerArgs, argsEdit->text());
  });

  argsPanel->contentLayout()->addWidget(argsLabel);
  argsPanel->contentLayout()->addWidget(argsEdit, 1);
  m_mainLayout->addWidget(argsPanel);

  
  auto *quickPlayPanel =
      new SettingsSubPanel(":/svg/dark/quick-play.svg", this);
  auto *quickPlayLabel =
      new QLabel(tr("Quick Play with External Player"), this);
  quickPlayLabel->setObjectName("SettingsCardDesc");
  auto *quickPlayDesc = new QLabel(
      tr("Use external player for quick play actions on cards and menus"),
      this);
  quickPlayDesc->setObjectName("SettingsCardDesc");
  quickPlayDesc->setWordWrap(true);
  auto *quickPlaySwitch = new ModernSwitch(this);
  quickPlaySwitch->setChecked(ConfigStore::instance()->get<bool>(
      ConfigKeys::ExtPlayerQuickPlay, false));
  connect(quickPlaySwitch, &ModernSwitch::toggled, this, [](bool checked) {
    ConfigStore::instance()->set(ConfigKeys::ExtPlayerQuickPlay, checked);
  });

  quickPlayPanel->contentLayout()->addWidget(quickPlayLabel);
  quickPlayPanel->contentLayout()->addWidget(quickPlayDesc, 1);
  quickPlayPanel->contentLayout()->addWidget(quickPlaySwitch);
  m_mainLayout->addWidget(quickPlayPanel);

  
  auto *directPanel =
      new SettingsSubPanel(":/svg/dark/direct-stream.svg", this);
  auto *directLabel = new QLabel(tr("Direct Stream"), this);
  directLabel->setObjectName("SettingsCardDesc");
  auto *directDesc =
      new QLabel(tr("Bypass server proxy and stream directly from source. "
                    "May not work on external networks"),
                 this);
  directDesc->setObjectName("SettingsCardDesc");
  directDesc->setWordWrap(true);
  auto *directSwitch = new ModernSwitch(this);
  directSwitch->setChecked(ConfigStore::instance()->get<bool>(
      ConfigKeys::ExtPlayerDirectStream, false));
  connect(directSwitch, &ModernSwitch::toggled, this, [](bool checked) {
    ConfigStore::instance()->set(ConfigKeys::ExtPlayerDirectStream, checked);
  });

  directPanel->contentLayout()->addWidget(directLabel);
  directPanel->contentLayout()->addWidget(directDesc, 1);
  directPanel->contentLayout()->addWidget(directSwitch);
  m_mainLayout->addWidget(directPanel);

  
  
  auto *replaceFrame = new QFrame(this);
  replaceFrame->setObjectName("UrlReplaceFrame");
  auto *replaceOuterLayout = new QHBoxLayout(replaceFrame);
  replaceOuterLayout->setContentsMargins(16, 10, 16, 10);
  replaceOuterLayout->setSpacing(12);

  
  auto *replaceIcon = new QLabel(replaceFrame);
  replaceIcon->setFixedSize(16, 16);
  replaceIcon->setAlignment(Qt::AlignCenter);
  const QString replaceIconPath = ":/svg/dark/url-replace.svg";
  replaceIcon->setPixmap(
      ThemeManager::getAdaptiveIcon(replaceIconPath).pixmap(16, 16));
  replaceOuterLayout->addWidget(replaceIcon, 0, Qt::AlignTop);

  
  auto *replaceVLayout = new QVBoxLayout();
  replaceVLayout->setContentsMargins(0, 0, 0, 0);
  replaceVLayout->setSpacing(4);

  auto *replaceLabel =
      new QLabel(tr("URL Path Replacement one rule per line: source => target"),
                 replaceFrame);
  replaceLabel->setObjectName("SettingsCardDesc");
  replaceLabel->setWordWrap(true);

  auto *replaceEdit = new QPlainTextEdit(replaceFrame);
  replaceEdit->setPlaceholderText(
      tr("e.g., http://192.168.2.1:19798/path/ => W:/mount/"));
  replaceEdit->setFixedHeight(80);
  replaceEdit->setPlainText(
      ConfigStore::instance()->get<QString>(ConfigKeys::ExtPlayerUrlReplace));

  
  connect(ThemeManager::instance(), &ThemeManager::themeChanged, replaceFrame,
          [replaceIcon, replaceIconPath]() {
            replaceIcon->setPixmap(
                ThemeManager::getAdaptiveIcon(replaceIconPath).pixmap(16, 16));
          });

  connect(replaceEdit, &QPlainTextEdit::textChanged, this, [replaceEdit]() {
    ConfigStore::instance()->set(ConfigKeys::ExtPlayerUrlReplace,
                                 replaceEdit->toPlainText());
  });

  replaceVLayout->addWidget(replaceLabel);
  replaceVLayout->addWidget(replaceEdit);
  replaceOuterLayout->addLayout(replaceVLayout, 1);
  m_mainLayout->addWidget(replaceFrame);

  
  bool extEnabled =
      ConfigStore::instance()->get<bool>(ConfigKeys::ExtPlayerEnable, false);
  if (extEnabled) {
    playerPanel->initExpanded();
    argsPanel->initExpanded();
    directPanel->initExpanded();
    quickPlayPanel->initExpanded();
  }
  replaceFrame->setVisible(extEnabled);

  connect(extSwitch, &ModernSwitch::toggled, playerPanel,
          &SettingsSubPanel::setExpanded);
  connect(extSwitch, &ModernSwitch::toggled, argsPanel,
          &SettingsSubPanel::setExpanded);
  connect(extSwitch, &ModernSwitch::toggled, directPanel,
          &SettingsSubPanel::setExpanded);
  connect(extSwitch, &ModernSwitch::toggled, quickPlayPanel,
          &SettingsSubPanel::setExpanded);
  connect(extSwitch, &ModernSwitch::toggled, replaceFrame,
          &QWidget::setVisible);

  
  m_mainLayout->addStretch();
}
