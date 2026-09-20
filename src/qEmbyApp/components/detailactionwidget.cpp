#include "detailactionwidget.h"
#include "../managers/externalplayerdetector.h"
#include "../managers/thememanager.h"
#include "../utils/mediasourcepreferenceutils.h"
#include "../utils/playerpreferenceutils.h"
#include "modernmenubutton.h"
#include "splitplayerbutton.h"
#include "flowlayout.h"
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPoint>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QStyle>
#include <QVBoxLayout>
#include <config/config_keys.h>
#include <config/configstore.h>
#include <services/trakt/traktservice.h>

DetailActionWidget::DetailActionWidget(QWidget *parent) : QWidget(parent) {
  // 关键：打开 height-for-width。QWidget 的 sizePolicy 默认
  // hasHeightForWidth()==false，外层 QVBoxLayout 因此只认构造期算出的
  // sizeHint().height()，永远不会在拿到真实宽度后重新询问高度 —— 行动作
  // 按钮需要换行时（"继续播放 S01E02" + "重新播放 S01E02" 等），构造期算出的
  // 单行高度会把第二行裁掉，表现为整行按钮不可见。
  {
    QSizePolicy sp = sizePolicy();
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
  }

  auto *mainLayout = new QVBoxLayout(this);
  mainLayout->setContentsMargins(0, 0, 0, 0);
  mainLayout->setSpacing(4);

  
  // 用 FlowLayout 让行动作按钮按父宽度自动换行：详情页在"继续播放 S01E1 /
  // 重新播放 S01E1"两个长按钮出现时，QHBoxLayout 不换行会把后面的外置播放器
  // 按钮挤出可见区（用户截图确认）。
  //
  // **构造时不要传 this**（这是行动作按钮整行错乱的真根因）：QLayout(QWidget*)
  // 构造函数会执行 parent->setLayout(this)，而本 widget 已有 mainLayout →
  // setLayout 失败（qWarning "already has a layout"）；同时 QObject parent 已被
  // 设为 this，导致随后 mainLayout->addLayout() 里的 addChildLayout() 因
  // "childLayout->parent() 非空"而 qWarning（"already has a parent"）后直接
  // return。两头都失败 → FlowLayout 成为孤儿：既不是顶层布局、也没挂进
  // mainLayout，setGeometry() 永不执行，按钮 geometry 从未被设置 → 全部堆在
  // 默认位置互相重叠（用户截图中按钮与 streamSelectors 行挤压重叠）。
  // 正确用法：无 parent 构造，交给 mainLayout->addLayout() 收养（与项目内其它
  // 7 处 FlowLayout 一致：要么传专用容器 widget，要么不传）。
  // setMinimumHeight 仅作一行高度的下限保护（不限制换行后的更高需求）。
  auto *actionsLayout = new FlowLayout(0, 12, 6);
  setMinimumHeight(40);

  m_resumeBtn = new QPushButton(tr("▶ Resume"), this);
  m_resumeBtn->setObjectName("detail-resume-btn");
  m_resumeBtn->setCursor(Qt::PointingHandCursor);
  m_resumeBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

  m_playBtn = new QPushButton(tr("▶ Play"), this);
  m_playBtn->setObjectName("detail-play-btn");
  m_playBtn->setCursor(Qt::PointingHandCursor);
  m_playBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

  m_favBtn = new QPushButton(this);
  m_favBtn->setObjectName("detail-fav-btn");
  m_favBtn->setIcon(QIcon(":/svg/light/heart.svg"));
  m_favBtn->setIconSize(QSize(20, 20));
  m_favBtn->setFixedSize(36, 36);
  m_favBtn->setCursor(Qt::PointingHandCursor);

  m_playedBtn = new QPushButton(this);
  m_playedBtn->setObjectName("detail-played-btn");
  m_playedBtn->setIconSize(QSize(20, 20));
  m_playedBtn->setFixedSize(36, 36);
  m_playedBtn->setCursor(Qt::PointingHandCursor);

  m_traktBtn = new QPushButton(tr("Trakt"), this);
  m_traktBtn->setObjectName("detail-trakt-btn");
  m_traktBtn->setCursor(Qt::PointingHandCursor);
  m_traktBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  m_traktBtn->setToolTip(tr("Sync watched status to Trakt"));
  m_traktBtn->hide();

  m_danmakuMatchBtn = new QPushButton(QStringLiteral("\U0001F4AC ") + tr("Match Danmaku"), this);
  m_danmakuMatchBtn->setObjectName("detail-danmaku-match-btn");
  m_danmakuMatchBtn->setCursor(Qt::PointingHandCursor);
  m_danmakuMatchBtn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  m_danmakuMatchBtn->setMinimumWidth(0);
  m_danmakuMatchBtn->setToolTip(tr("Match danmaku for this item"));
  m_danmakuMatchBtn->hide();

  m_danmakuRematchBtn = new QPushButton(QStringLiteral("\u21BB ") + tr("Rematch Danmaku"), this);
  m_danmakuRematchBtn->setObjectName("detail-danmaku-rematch-btn");
  m_danmakuRematchBtn->setCursor(Qt::PointingHandCursor);
  m_danmakuRematchBtn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  m_danmakuRematchBtn->setMinimumWidth(0);
  m_danmakuRematchBtn->hide();

  m_danmakuClearBtn = new QPushButton(QStringLiteral("\u2715 ") + tr("Clear Danmaku Binding"), this);
  m_danmakuClearBtn->setObjectName("detail-danmaku-clear-btn");
  m_danmakuClearBtn->setCursor(Qt::PointingHandCursor);
  m_danmakuClearBtn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  m_danmakuClearBtn->setMinimumWidth(0);
  m_danmakuClearBtn->hide();

  m_progressWidget = new QWidget(this);
  m_progressWidget->setMaximumWidth(450);
  auto *progressLayout = new QHBoxLayout(m_progressWidget);
  progressLayout->setContentsMargins(0, 0, 0, 0);
  progressLayout->setSpacing(10);

  m_progressBar = new QProgressBar(m_progressWidget);
  m_progressBar->setObjectName("detail-progress-bar");
  m_progressBar->setTextVisible(false);

  m_remainingTimeLabel = new QLabel(m_progressWidget);
  m_remainingTimeLabel->setObjectName("detail-remaining-time");

  progressLayout->addWidget(m_progressBar, 1);
  progressLayout->addWidget(m_remainingTimeLabel);

  
  m_extPlayerBtn = new SplitPlayerButton(this);
  m_extPlayerBtn->setObjectName("detail-ext-player-btn");
  m_extPlayerBtn->setIconOnly(true);
  m_extPlayerBtn->hide();

  connect(m_extPlayerBtn, &SplitPlayerButton::playRequested, this,
          &DetailActionWidget::externalPlayRequested);
  connect(m_extPlayerBtn, &SplitPlayerButton::playerSelected, this,
          &DetailActionWidget::externalPlayRequested);

  actionsLayout->addWidget(m_resumeBtn);
  actionsLayout->addWidget(m_playBtn);
  actionsLayout->addWidget(m_extPlayerBtn);
  actionsLayout->addWidget(m_favBtn);
  actionsLayout->addWidget(m_playedBtn);
  actionsLayout->addWidget(m_traktBtn);
  actionsLayout->addWidget(m_danmakuMatchBtn);
  actionsLayout->addWidget(m_danmakuRematchBtn);
  actionsLayout->addWidget(m_danmakuClearBtn);
  actionsLayout->addWidget(m_progressWidget);
  // 注意：FlowLayout 没有 addStretch()（那是 QBoxLayout 的方法）——流式布局
  // 本身就是左对齐逐行排布，不需要尾部弹簧。

  
  auto *streamSelectorsLayout = new QHBoxLayout();
  streamSelectorsLayout->setSpacing(6);
  streamSelectorsLayout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

  m_versionComboBox = new ModernMenuButton(this);
  m_audioComboBox = new ModernMenuButton(this);
  m_subtitleComboBox = new ModernMenuButton(this);

  streamSelectorsLayout->addWidget(m_versionComboBox);
  streamSelectorsLayout->addWidget(m_audioComboBox);
  streamSelectorsLayout->addWidget(m_subtitleComboBox);
  streamSelectorsLayout->addStretch();

  mainLayout->addLayout(actionsLayout);
  mainLayout->addLayout(streamSelectorsLayout);

  connect(m_playBtn, &QPushButton::clicked, this,
          &DetailActionWidget::playRequested);
  connect(m_resumeBtn, &QPushButton::clicked, this,
          &DetailActionWidget::resumeRequested);
  connect(m_favBtn, &QPushButton::clicked, this,
          &DetailActionWidget::favoriteRequested);
  connect(m_playedBtn, &QPushButton::clicked, this,
          &DetailActionWidget::playedToggleRequested);
  connect(m_danmakuMatchBtn, &QPushButton::clicked, this,
          &DetailActionWidget::danmakuMatchRequested);
  connect(m_danmakuRematchBtn, &QPushButton::clicked, this,
          &DetailActionWidget::danmakuRematchRequested);
  connect(m_danmakuClearBtn, &QPushButton::clicked, this,
          &DetailActionWidget::danmakuClearBindingsRequested);
  connect(m_traktBtn, &QPushButton::clicked, this, [this]() {
    QMenu menu(m_traktBtn);
    QAction* markAction = menu.addAction(tr("Mark as Watched"));
    QAction* unmarkAction = menu.addAction(tr("Remove from History"));
    QAction* chosen = menu.exec(m_traktBtn->mapToGlobal(
        QPoint(0, m_traktBtn->height())));
    if (chosen == markAction)
      Q_EMIT traktMarkWatchedRequested();
    else if (chosen == unmarkAction)
      Q_EMIT traktUnmarkWatchedRequested();
  });
  connect(m_versionComboBox, &ModernMenuButton::currentIndexChanged, this,
          [this](int visualIndex) {
            if (visualIndex >= 0 && visualIndex < m_sourceIndexes.size())
              Q_EMIT sourceVersionChanged(m_sourceIndexes[visualIndex]);
          });
  connect(m_audioComboBox, &ModernMenuButton::currentIndexChanged, this,
          [this](int) {
            Q_EMIT audioStreamChanged(m_audioComboBox->currentData().toInt());
          });
  connect(m_subtitleComboBox, &ModernMenuButton::currentIndexChanged, this,
          [this](int) {
            Q_EMIT subtitleStreamChanged(
                m_subtitleComboBox->currentData().toInt());
          });

  clear();
}

bool DetailActionWidget::hasHeightForWidth() const {
  // 内部 mainLayout 含 FlowLayout（行动作按钮按宽度换行），其
  // hasHeightForWidth() 为 true；QWidget 基类实现会转发给 layout，这里显式
  // 重写只为语义清晰 + 与 sizePolicy().setHeightForWidth(true) 配套。
  return layout() ? layout()->hasHeightForWidth() : QWidget::hasHeightForWidth();
}

int DetailActionWidget::heightForWidth(int width) const {
  // 把真实可用宽度透传给内部 layout（QVBoxLayout → FlowLayout），
  // FlowLayout 据此算出实际需要的行数与总高度。
  if (QLayout *lay = layout(); lay && lay->hasHeightForWidth()) {
    return lay->totalHeightForWidth(width);
  }
  return QWidget::heightForWidth(width);
}

void DetailActionWidget::clear() {
  m_resumeBtn->hide();
  m_playBtn->setText(tr("▶ Play"));
  m_progressWidget->hide();

  m_traktItemType.clear();
  if (m_traktBtn)
    m_traktBtn->hide();
  if (m_danmakuMatchBtn)
    m_danmakuMatchBtn->hide();
  if (m_danmakuRematchBtn)
    m_danmakuRematchBtn->hide();
  if (m_danmakuClearBtn)
    m_danmakuClearBtn->hide();
  m_danmakuBoundCount = 0;

  m_versionComboBox->blockSignals(true);
  m_versionComboBox->clear();
  m_sourceIndexes.clear();
  m_versionComboBox->hide();
  m_versionComboBox->blockSignals(false);

  m_audioComboBox->blockSignals(true);
  m_audioComboBox->clear();
  m_audioComboBox->hide();
  m_audioComboBox->blockSignals(false);

  m_subtitleComboBox->blockSignals(true);
  m_subtitleComboBox->clear();
  m_subtitleComboBox->hide();
  m_subtitleComboBox->blockSignals(false);
}

void DetailActionWidget::setTraktItemType(const QString& itemType) {
  m_traktItemType = itemType;
  updateTraktButtonVisibility();
}

void DetailActionWidget::setDanmakuMatchVisible(bool visible) {
  if (m_danmakuMatchBtn) {
    m_danmakuMatchBtn->setVisible(visible);
  }
}

void DetailActionWidget::setDanmakuMatchBoundCount(int count) {
  m_danmakuBoundCount = count;
  const bool bound = count > 0;
  if (m_danmakuMatchBtn) {
    m_danmakuMatchBtn->setVisible(!bound);
  }
  if (m_danmakuRematchBtn) {
    m_danmakuRematchBtn->setVisible(bound);
    if (bound) {
      m_danmakuRematchBtn->setToolTip(
          tr("Re-open the danmaku matcher. %1 episode binding(s) saved.")
              .arg(count));
    } else {
      m_danmakuRematchBtn->setToolTip(QString());
    }
  }
  if (m_danmakuClearBtn) {
    m_danmakuClearBtn->setVisible(bound);
  }
}

void DetailActionWidget::updateTraktButtonVisibility() {
  if (!m_traktBtn)
    return;
  const bool supported = m_traktItemType == QLatin1String("Movie") ||
                         m_traktItemType == QLatin1String("Episode") ||
                         m_traktItemType == QLatin1String("Series");
  bool visible = false;
  if (supported) {
    TraktService* service = TraktService::instance();
    visible = ConfigStore::instance()->get<bool>(
                  ConfigKeys::TraktSyncButtonEnabled, false) &&
              service->isLoggedIn() && !service->clientId().isEmpty();
  }
  m_traktBtn->setVisible(visible);
}

void DetailActionWidget::setupNormalMode(const MediaItem &item) {
  setTraktItemType(item.type);
  setDanmakuMatchVisible(
      item.type == QLatin1String("Movie") ||
      item.type == QLatin1String("Episode") ||
      item.type == QLatin1String("Series"));
  m_playBtn->setEnabled(true);
  if (item.userData.playedPercentage > 0 &&
      item.userData.playedPercentage < 100) {
    m_resumeBtn->show();
    m_playBtn->setText(tr("↺ Play from Beginning"));

    m_progressBar->setValue(static_cast<int>(item.userData.playedPercentage));
    long long remainingTicks =
        item.runTimeTicks - item.userData.playbackPositionTicks;
    if (remainingTicks > 0) {
      m_remainingTimeLabel->setText(
          tr("%1 remaining").arg(formatRunTime(remainingTicks)));
    } else {
      m_remainingTimeLabel->clear();
    }
    m_progressWidget->show();
  } else {
    m_resumeBtn->hide();
    m_playBtn->setText(tr("▶ Play"));
    m_progressWidget->hide();
  }
}

void DetailActionWidget::setSeriesLoadingMode() {
  m_resumeBtn->hide();
  m_playBtn->setText(tr("▶ Play"));
  m_playBtn->setEnabled(false);
  m_progressWidget->hide();
}

void DetailActionWidget::setupSeriesMode(const MediaItem &nextUpItem,
                                         const QString &epTag) {
  setTraktItemType(QLatin1String("Series"));
  setDanmakuMatchVisible(true);
  m_playBtn->setEnabled(true);
  if (nextUpItem.userData.playbackPositionTicks > 0) {
    m_resumeBtn->show();
    m_resumeBtn->setText(tr("▶ Resume %1").arg(epTag));
    m_playBtn->setText(tr("↺ Restart %1").arg(epTag));

    if (nextUpItem.userData.playedPercentage > 0) {
      m_progressBar->setValue(
          static_cast<int>(nextUpItem.userData.playedPercentage));
      long long remainingTicks =
          nextUpItem.runTimeTicks - nextUpItem.userData.playbackPositionTicks;
      if (remainingTicks > 0) {
        m_remainingTimeLabel->setText(
            tr("%1 remaining").arg(formatRunTime(remainingTicks)));
      } else {
        m_remainingTimeLabel->clear();
      }
      m_progressWidget->show();
    }
  } else {
    m_resumeBtn->hide();
    m_playBtn->setText(tr("▶ Play %1").arg(epTag));
    m_progressWidget->hide();
  }
}

void DetailActionWidget::setFavoriteState(bool isFavorite) {
  m_favBtn->setProperty("isFavorite", isFavorite);
  m_favBtn->setIcon(QIcon(isFavorite ? ":/svg/light/heart-fill.svg"
                                     : ":/svg/light/heart-outline.svg"));
  m_favBtn->style()->unpolish(m_favBtn);
  m_favBtn->style()->polish(m_favBtn);
}

void DetailActionWidget::setPlayedState(bool played) {
  m_playedBtn->setProperty("played", played);
  const QString themeDir =
      ThemeManager::instance()->isDarkMode() ? "dark" : "light";
  if (played) {
    m_playedBtn->setIcon(QIcon(":/svg/dark/played-check.svg"));
  } else {
    m_playedBtn->setIcon(
        QIcon(QString(":/svg/%1/unplayed-check.svg").arg(themeDir)));
  }
  m_playedBtn->style()->unpolish(m_playedBtn);
  m_playedBtn->style()->polish(m_playedBtn);
}

void DetailActionWidget::setSources(const QList<MediaSourceInfo> &sources,
                                    int currentIndex) {
  m_versionComboBox->blockSignals(true);
  m_versionComboBox->clear();
  m_sourceIndexes.clear();

  if (!sources.isEmpty()) {
    const QString preferredRules =
        ConfigStore::instance()
            ->get<QString>(ConfigKeys::PlayerPreferredVersion)
            .trimmed();
    m_sourceIndexes = MediaSourcePreferenceUtils::preferredMediaSourceOrder(
        sources, preferredRules);
    for (int visualIndex = 0; visualIndex < m_sourceIndexes.size();
         ++visualIndex) {
      const int i = m_sourceIndexes[visualIndex];
      const auto &src = sources[i];
      QString versionName =
          src.name.isEmpty() ? tr("Version %1").arg(visualIndex + 1) : src.name;
      QString videoInfo = tr("Unknown Video");
      for (const auto &stream : src.mediaStreams) {
        if (stream.type == "Video") {
          QStringList parts;
          if (stream.width > 0)
            parts << QString("%1x%2").arg(stream.width).arg(stream.height);
          if (!stream.codec.isEmpty())
            parts << stream.codec.toUpper();
          if (stream.realFrameRate > 0)
            parts << QString::number(stream.realFrameRate, 'f', 1) + " fps";
          if (stream.bitRate > 0)
            parts << QString("%1 Mbps").arg(stream.bitRate / 1000000.0, 0, 'f',
                                            1);
          if (!parts.isEmpty())
            videoInfo = parts.join(" · ");
          break;
        }
      }
      m_versionComboBox->addItem("🎞 " + versionName, videoInfo, QString(),
                                 src.id);
    }

    const int selectedSourceIndex =
        currentIndex >= 0 && currentIndex < sources.size()
            ? currentIndex
            : MediaSourcePreferenceUtils::resolvePreferredMediaSourceIndex(
                  sources, preferredRules);
    const int selectedVisualIndex = m_sourceIndexes.indexOf(selectedSourceIndex);
    m_versionComboBox->setCurrentIndex(qMax(0, selectedVisualIndex));
    m_versionComboBox->show();
  } else {
    m_versionComboBox->hide();
  }
  m_versionComboBox->blockSignals(false);
}

void DetailActionWidget::setStreams(
    const MediaSourceInfo &source, std::optional<int> rememberedAudioIndex,
    std::optional<int> rememberedSubtitleIndex) {
  m_audioComboBox->blockSignals(true);
  m_subtitleComboBox->blockSignals(true);
  m_audioComboBox->clear();
  m_subtitleComboBox->clear();

  m_subtitleComboBox->addItem("🚫 " + tr("No Subtitles"), QString(),
                              tr("Disable subtitle display"), -1);

  
  QString prefAudioLang = ConfigStore::instance()->get<QString>(
      ConfigKeys::PlayerAudioLang, "auto");
  QString prefSubLang =
      ConfigStore::instance()->get<QString>(ConfigKeys::PlayerSubLang, "auto");
  const int preferredAudioStreamIndex =
      PlayerPreferenceUtils::findPreferredStreamIndex(
          source.mediaStreams, "Audio", prefAudioLang);
  const int preferredSubtitleStreamIndex =
      PlayerPreferenceUtils::findPreferredStreamIndex(
          source.mediaStreams, "Subtitle", prefSubLang);
  const bool subtitleDisabled =
      PlayerPreferenceUtils::isSubtitleDisabled(prefSubLang);

  int defaultAudioIdx = 0;
  int defaultSubIdx = 0; 
  bool subMatchedByPref = false;
  bool subMatchedByDefault = false; 
  int rememberedAudioComboIndex = -1;
  int rememberedSubtitleComboIndex = -1;

  QList<int> orderedStreamPositions =
      PlayerPreferenceUtils::preferredStreamOrder(source.mediaStreams,
                                                   "Audio", prefAudioLang);
  orderedStreamPositions.append(
      PlayerPreferenceUtils::preferredStreamOrder(source.mediaStreams,
                                                   "Subtitle", prefSubLang));

  for (const int streamPosition : orderedStreamPositions) {
    const auto &stream = source.mediaStreams[streamPosition];
    if (stream.type == "Audio") {
      QString title =
          stream.displayTitle.isEmpty() ? stream.language : stream.displayTitle;
      if (title.isEmpty())
        title = tr("Audio Track");
      QStringList parts;
      if (!stream.codec.isEmpty())
        parts << stream.codec.toUpper();
      if (stream.channels > 0)
        parts << QString("%1 ch").arg(stream.channels);
      if (stream.sampleRate > 0)
        parts << QString("%1 kHz").arg(stream.sampleRate / 1000.0, 0, 'f', 1);
      if (stream.bitRate > 0)
        parts << QString("%1 kbps").arg(stream.bitRate / 1000);

      
      QStringList secondLineParts;
      if (!stream.title.isEmpty())
        secondLineParts << stream.title;
      if (!parts.isEmpty())
        secondLineParts << parts.join(" · ");
      m_audioComboBox->addItem("🔊 " + title, QString(),
                               secondLineParts.join("  ·  "), stream.index);

      int curIdx = m_audioComboBox->count() - 1;
      if (rememberedAudioIndex.has_value() &&
          stream.index == *rememberedAudioIndex) {
        rememberedAudioComboIndex = curIdx;
      }
      if (preferredAudioStreamIndex >= 0 &&
          stream.index == preferredAudioStreamIndex) {
        defaultAudioIdx = curIdx;
      } else if (preferredAudioStreamIndex < 0 && stream.isDefault) {
        defaultAudioIdx = curIdx;
      }
    } else if (stream.type == "Subtitle") {
      QString title =
          stream.displayTitle.isEmpty() ? stream.language : stream.displayTitle;
      if (title.isEmpty())
        title = tr("Subtitle");
      QStringList parts;
      if (!stream.codec.isEmpty())
        parts << stream.codec.toUpper();
      if (stream.isForced)
        parts << tr("Forced");
      if (stream.isExternal)
        parts << tr("External");
      if (stream.isHearingImpaired)
        parts << tr("SDH");

      
      QStringList secondLineParts;
      if (!stream.title.isEmpty())
        secondLineParts << stream.title;
      if (!parts.isEmpty())
        secondLineParts << parts.join(" · ");
      m_subtitleComboBox->addItem("💬 " + title, QString(),
                                  secondLineParts.join("  ·  "), stream.index);

      int curIdx = m_subtitleComboBox->count() - 1;
      if (rememberedSubtitleIndex.has_value() &&
          stream.index == *rememberedSubtitleIndex) {
        rememberedSubtitleComboIndex = curIdx;
      }
      if (preferredSubtitleStreamIndex >= 0 &&
          stream.index == preferredSubtitleStreamIndex) {
        defaultSubIdx = curIdx;
        subMatchedByPref = true;
      } else if (preferredSubtitleStreamIndex < 0 && !subMatchedByPref &&
                 stream.isDefault) {
        defaultSubIdx = curIdx;
        subMatchedByDefault = true;
      }
    }
  }

  if (rememberedAudioComboIndex >= 0) {
    defaultAudioIdx = rememberedAudioComboIndex;
  }

  
  if (rememberedSubtitleIndex.has_value() &&
      *rememberedSubtitleIndex == -1) {
    defaultSubIdx = 0;
  } else if (rememberedSubtitleComboIndex >= 0) {
    defaultSubIdx = rememberedSubtitleComboIndex;
  } else if (subtitleDisabled) {
    defaultSubIdx = 0; 
  } else if (!subMatchedByPref && !subMatchedByDefault &&
             m_subtitleComboBox->count() > 1) {
    
    
    defaultSubIdx = 1;
  }

  if (m_audioComboBox->count() > 0) {
    m_audioComboBox->setCurrentIndex(defaultAudioIdx);
    m_audioComboBox->show();
  } else {
    m_audioComboBox->hide();
  }

  if (m_subtitleComboBox->count() > 1) {
    m_subtitleComboBox->setCurrentIndex(defaultSubIdx);
    m_subtitleComboBox->show();
  } else {
    m_subtitleComboBox->hide();
  }

  m_audioComboBox->blockSignals(false);
  m_subtitleComboBox->blockSignals(false);
}

int DetailActionWidget::currentSourceIndex() const {
  const int visualIndex = m_versionComboBox->currentIndex();
  return visualIndex >= 0 && visualIndex < m_sourceIndexes.size()
             ? m_sourceIndexes[visualIndex]
             : 0;
}
int DetailActionWidget::currentAudioIndex() const {
  return m_audioComboBox->isVisible() ? m_audioComboBox->currentData().toInt()
                                      : -1;
}
int DetailActionWidget::currentSubtitleIndex() const {
  return m_subtitleComboBox->isVisible()
             ? m_subtitleComboBox->currentData().toInt()
             : -1;
}

QString DetailActionWidget::formatRunTime(long long ticks) {
  long long totalSeconds = ticks / 10000000;
  long long hours = totalSeconds / 3600;
  long long minutes = (totalSeconds % 3600) / 60;
  if (hours > 0)
    return QString(tr("%1 hr %2 min")).arg(hours).arg(minutes);
  return QString(tr("%1 min")).arg(minutes);
}

void DetailActionWidget::refreshExtPlayerButton() {
  bool extEnabled =
      ConfigStore::instance()->get<bool>(ConfigKeys::ExtPlayerEnable, false);
  if (!extEnabled) {
    m_extPlayerBtn->hide();
    return;
  }

  QString currentPath =
      ConfigStore::instance()->get<QString>(ConfigKeys::ExtPlayerPath);

  
  QList<DetectedPlayer> allPlayers = ExternalPlayerDetector::loadFromConfig();

  
  QSet<QString> knownPaths;
  for (const auto &p : allPlayers)
    knownPaths.insert(p.path);
  if (!currentPath.isEmpty() && currentPath != "custom" &&
      !knownPaths.contains(currentPath) && QFileInfo::exists(currentPath)) {
    allPlayers.prepend({QFileInfo(currentPath).baseName(), currentPath});
  }

  if (allPlayers.isEmpty()) {
    m_extPlayerBtn->hide();
    return;
  }

  
  QString activePlayerPath = currentPath;
  if (activePlayerPath.isEmpty() || activePlayerPath == "custom") {
    activePlayerPath = allPlayers.first().path;
  }
  m_extPlayerBtn->setPlayers(allPlayers, activePlayerPath);
}
