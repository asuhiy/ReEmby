#include "mediacarddelegate.h"
#include "medialistmodel.h"

#include "../../managers/thememanager.h"
#include <QAbstractItemView>
#include <QEvent>
#include <QFontMetrics>
#include <QIcon>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QStyle>

namespace
{

enum class HoverOverlayMode
{
    None,
    FullControls,
    MenuOnly
};

bool isPlayableItem(const MediaItem &item)
{
    return item.mediaType == "Video" || item.type == "Movie" || item.type == "Episode" || item.type == "Video" ||
           item.type == "MusicVideo";
}

bool isPlaylistItem(const MediaItem &item)
{
    return item.type == "Playlist";
}

bool canShowMenuOnlyButtonForTile(const MediaItem &item)
{
    if (item.id.trimmed().isEmpty())
    {
        return false;
    }

    const QString normalizedCollectionType = item.collectionType.trimmed().toLower();
    const QString normalizedType = item.type.trimmed().toLower();
    return normalizedCollectionType != "playlists" && normalizedCollectionType != "boxsets" &&
           normalizedType != "playlist" && normalizedType != "boxset";
}

HoverOverlayMode hoverOverlayModeForItem(const MediaItem &item, MediaCardDelegate::CardStyle style,
                                         bool showMoreButtonForNonPlayableTiles)
{
    if (style == MediaCardDelegate::EpisodeList)
    {
        return isPlayableItem(item) ? HoverOverlayMode::FullControls : HoverOverlayMode::None;
    }

    if (isPlayableItem(item) || item.type == "Series" || item.type == "Season")
    {
        return HoverOverlayMode::FullControls;
    }

    if (isPlaylistItem(item))
    {
        return HoverOverlayMode::MenuOnly;
    }

    if (style == MediaCardDelegate::LibraryTile && showMoreButtonForNonPlayableTiles &&
        canShowMenuOnlyButtonForTile(item))
    {
        return HoverOverlayMode::MenuOnly;
    }

    return HoverOverlayMode::None;
}

QRect baseImageRectForCard(MediaCardDelegate::CardStyle style, const QRect &rect, int padding)
{
    const int imgWidth = rect.width() - padding * 2;

    if (style == MediaCardDelegate::LibraryTile)
    {
        const int imgHeight = qRound(imgWidth * 9.0 / 16.0);
        return QRect(rect.x() + padding, rect.y() + padding, imgWidth, imgHeight);
    }

    const int imgHeight = qRound(imgWidth * 1.5);
    return QRect(rect.x() + padding, rect.y() + padding, imgWidth, imgHeight);
}

QRect hoverImageRect(const QRect &baseImgRect)
{
    const int expandW = qRound(baseImgRect.width() * 0.035);
    const int expandH = qRound(baseImgRect.height() * 0.035);
    return baseImgRect.adjusted(-expandW, -expandH, expandW, expandH);
}

QRect centerPlayButtonRect(const QRect &targetImgRect)
{
    const int playSize = 48;
    return QRect(targetImgRect.center().x() - playSize / 2, targetImgRect.center().y() - playSize / 2, playSize,
                 playSize);
}

QRect moreButtonRect(const QRect &targetImgRect)
{
    const int btnWidth = 28;
    const int btnHeight = 28;
    return QRect(targetImgRect.right() - btnWidth - 8, targetImgRect.bottom() - btnHeight - 8, btnWidth, btnHeight);
}

QRect favoriteButtonRect(const QRect &moreRect)
{
    const int btnWidth = 28;
    const int spacing = 6;
    return QRect(moreRect.left() - btnWidth - spacing, moreRect.top(), btnWidth, moreRect.height());
}

bool shouldShowPlayButton(HoverOverlayMode overlayMode, MediaCardDelegate::HoverControls controls)
{
    return overlayMode == HoverOverlayMode::FullControls && controls.testFlag(MediaCardDelegate::HoverControlPlay);
}

bool shouldShowFavoriteButton(HoverOverlayMode overlayMode, MediaCardDelegate::HoverControls controls)
{
    return overlayMode == HoverOverlayMode::FullControls && controls.testFlag(MediaCardDelegate::HoverControlFavorite);
}

bool shouldShowMoreButton(HoverOverlayMode overlayMode, MediaCardDelegate::HoverControls controls)
{
    return overlayMode != HoverOverlayMode::None && controls.testFlag(MediaCardDelegate::HoverControlMore);
}

struct HoverButtonLayout
{
    QRect playRect;
    QRect favoriteRect;
    QRect moreRect;
};

HoverButtonLayout buildHoverButtonLayout(const QRect &targetImgRect, bool showPlayButton, bool showFavoriteButton,
                                         bool showMoreButton)
{
    HoverButtonLayout layout;
    if (showPlayButton)
    {
        layout.playRect = centerPlayButtonRect(targetImgRect);
    }

    const QRect trailingButtonRect = moreButtonRect(targetImgRect);
    if (showMoreButton)
    {
        layout.moreRect = trailingButtonRect;
    }
    if (showFavoriteButton)
    {
        layout.favoriteRect = showMoreButton ? favoriteButtonRect(trailingButtonRect) : trailingButtonRect;
    }

    return layout;
}

QPixmap scaledCardPixmap(const MediaItem &item, const QPixmap &source, const QSize &targetSize,
                         MediaCardDelegate::CardStyle style)
{
    if (source.isNull() || targetSize.isEmpty())
    {
        return {};
    }

    const QString cacheKey = QStringLiteral("MediaCardDelegate:%1:%2:%3x%4:%5")
                                 .arg(item.id)
                                 .arg(source.cacheKey())
                                 .arg(targetSize.width())
                                 .arg(targetSize.height())
                                 .arg(static_cast<int>(style));

    QPixmap cached;
    if (QPixmapCache::find(cacheKey, &cached))
    {
        return cached;
    }

    QPixmap scaled = source.scaled(targetSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QPixmapCache::insert(cacheKey, scaled);
    return scaled;
}

void drawPlayedBadge(QPainter *painter, const QRect &targetImgRect, bool alignLeft)
{
    const int checkSize = 16;
    const int checkMargin = 4;
    const int checkX = alignLeft ? targetImgRect.left() + checkMargin
                                 : targetImgRect.right() - checkSize - checkMargin;
    const QRect checkRect(checkX, targetImgRect.top() + checkMargin, checkSize, checkSize);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(16, 185, 129, 140));
    painter->drawRoundedRect(checkRect, checkSize / 2, checkSize / 2);

    QIcon checkIcon(":/svg/dark/check.svg");
    if (!checkIcon.isNull())
    {
        checkIcon.paint(painter, checkRect.adjusted(3, 3, -3, -3), Qt::AlignCenter);
    }

    painter->restore();
}

} 


MediaCardThemeHelper::MediaCardThemeHelper(QWidget *parent)
    : QWidget(parent), m_hoverBg(229, 231, 235), 
      m_selectedBg(219, 234, 254), m_titleColor(31, 41, 55), m_subTitleColor(107, 114, 128),
      m_placeholderBg(229, 231, 235), m_placeholderTextColor(156, 163, 175)
{
    hide(); 
}



MediaCardDelegate::MediaCardDelegate(CardStyle style, QObject *parent)
    : QStyledItemDelegate(parent), m_style(style), m_tileSize(160, 270), m_themeHelper(nullptr)
{
}

QSize MediaCardDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(option);
    Q_UNUSED(index);
    
    return m_tileSize;
}


bool MediaCardDelegate::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease ||
        event->type() == QEvent::MouseButtonDblClick)
    {

        QWidget *viewport = qobject_cast<QWidget *>(object);
        if (viewport)
        {
            
            QAbstractItemView *view = qobject_cast<QAbstractItemView *>(viewport->parentWidget());
            if (view)
            {
                QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
                QPoint pos = mouseEvent->pos();
                QModelIndex index = view->indexAt(pos); 

                if (index.isValid())
                {
                    MediaItem item = index.data(MediaListModel::ItemDataRole).value<MediaItem>();
                    const HoverOverlayMode overlayMode =
                        hoverOverlayModeForItem(item, m_style, m_showMoreButtonForNonPlayableTiles);

                    if (overlayMode != HoverOverlayMode::None)
                    {
                        
                        QRect rect = view->visualRect(index);

                        
                        
                        
                        if (m_style == EpisodeList)
                        {
                            int padding = 10;
                            int imgWidth = 250;
                            int imgHeight = 140;
                            QRect baseImgRect(rect.x() + padding, rect.y() + (rect.height() - imgHeight) / 2, imgWidth,
                                              imgHeight);

                            
                            int favBtnSize = 24;
                            int rightMargin = 20;
                            QRect favRect(rect.right() - rightMargin - favBtnSize, rect.center().y() - favBtnSize / 2,
                                          favBtnSize, favBtnSize);

                            const bool showFavoriteButton = m_hoverControls.testFlag(HoverControlFavorite);
                            bool inFav = showFavoriteButton && favRect.contains(pos);

                            
                            int playSize = 48;
                            QRect playRectCenter(baseImgRect.center().x() - playSize / 2,
                                                 baseImgRect.center().y() - playSize / 2, playSize, playSize);
                            const bool showPlayButton = m_hoverControls.testFlag(HoverControlPlay);
                            bool inThumbPlay = showPlayButton && playRectCenter.contains(pos);

                            if (inFav || inThumbPlay)
                            {
                                if (event->type() == QEvent::MouseButtonRelease)
                                {
                                    if (inThumbPlay)
                                        emit playRequested(item);
                                    else if (inFav)
                                        emit favoriteRequested(item);
                                }
                                return true;
                            }
                        }
                        else
                        {
                            const QRect baseImgRect = baseImageRectForCard(m_style, rect, m_contentPadding);
                            const QRect targetImgRect = hoverImageRect(baseImgRect);
                            const bool showPlayButton = shouldShowPlayButton(overlayMode, m_hoverControls);
                            const bool showFavoriteButton = shouldShowFavoriteButton(overlayMode, m_hoverControls);
                            const bool showMoreButton = shouldShowMoreButton(overlayMode, m_hoverControls);

                            if (!showPlayButton && !showFavoriteButton && !showMoreButton)
                            {
                                return QStyledItemDelegate::eventFilter(object, event);
                            }

                            const HoverButtonLayout buttonLayout = buildHoverButtonLayout(
                                targetImgRect, showPlayButton, showFavoriteButton, showMoreButton);

                            
                            const bool inPlay = showPlayButton && buttonLayout.playRect.contains(pos);
                            const bool inFav = showFavoriteButton && buttonLayout.favoriteRect.contains(pos);
                            const bool inMore = showMoreButton && buttonLayout.moreRect.contains(pos);

                            if (inPlay || inFav || inMore)
                            {
                                
                                if (event->type() == QEvent::MouseButtonRelease)
                                {
                                    if (inPlay)
                                        emit playRequested(item);
                                    else if (inFav)
                                        emit favoriteRequested(item);
                                    else if (inMore)
                                        emit moreMenuRequested(item, mouseEvent->globalPosition().toPoint());
                                }
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }
    
    return QStyledItemDelegate::eventFilter(object, event);
}

void MediaCardDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    
    if (option.widget)
    {
        
        QAbstractItemView *view = qobject_cast<QAbstractItemView *>(const_cast<QWidget *>(option.widget));
        if (view)
        {
            QWidget *viewport = view->viewport(); 
            if (!m_installedViewports.contains(viewport))
            {
                viewport->installEventFilter(const_cast<MediaCardDelegate *>(this));
                m_installedViewports.insert(viewport);

                connect(viewport, &QObject::destroyed, const_cast<MediaCardDelegate *>(this),
                        [this, viewport]() { m_installedViewports.remove(viewport); });
            }
        }
    }

    if (!m_themeHelper && option.widget)
    {
        m_themeHelper = new MediaCardThemeHelper(const_cast<QWidget *>(option.widget));
        m_themeHelper->setObjectName("media-card-theme");
        option.widget->style()->polish(m_themeHelper); 
    }

    
    QColor selectedBg = m_themeHelper ? m_themeHelper->selectedBg() : QColor(219, 234, 254);
    QColor titleColor = m_themeHelper ? m_themeHelper->titleColor() : QColor(31, 41, 55);
    QColor subTitleColor = m_themeHelper ? m_themeHelper->subTitleColor() : QColor(107, 114, 128);
    QColor placeholderBg = m_themeHelper ? m_themeHelper->placeholderBg() : QColor(229, 231, 235);
    QColor placeholderTextColor = m_themeHelper ? m_themeHelper->placeholderTextColor() : QColor(156, 163, 175);

    
    bool isDarkTheme = ThemeManager::instance()->isDarkMode();

    
    const qreal fontScale = ThemeManager::fontScaleFactor();

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true); 

    QRect rect = option.rect;
    MediaItem item = index.data(MediaListModel::ItemDataRole).value<MediaItem>();
    QPixmap poster = index.data(MediaListModel::PosterPixmapRole).value<QPixmap>();
    bool isHovered = option.state & QStyle::State_MouseOver;
    const HoverOverlayMode overlayMode = hoverOverlayModeForItem(item, m_style, m_showMoreButtonForNonPlayableTiles);
    const bool showPlayButton = shouldShowPlayButton(overlayMode, m_hoverControls);
    const bool showFavoriteButton = shouldShowFavoriteButton(overlayMode, m_hoverControls);
    const bool showMoreButton = shouldShowMoreButton(overlayMode, m_hoverControls);
    const bool hasHoverButtons = showPlayButton || showFavoriteButton || showMoreButton;

    
    
    
    if (m_style == EpisodeList)
    {
        if (isHovered)
        {
            
            painter->setPen(Qt::NoPen);
            painter->setBrush(isDarkTheme ? QColor(255, 255, 255, 20) : QColor(0, 0, 0, 15));
            painter->drawRoundedRect(rect.adjusted(5, 5, -5, -5), 8, 8);
        }

        int padding = 10;
        int imgWidth = 250;
        int imgHeight = 140;
        QRect baseImgRect(rect.x() + padding, rect.y() + (rect.height() - imgHeight) / 2, imgWidth, imgHeight);

        int imgRadius = 8;
        painter->setPen(Qt::NoPen);
        painter->setBrush(placeholderBg);
        painter->drawRoundedRect(baseImgRect, imgRadius, imgRadius);

        if (!poster.isNull())
        {
            QPainterPath path;
            path.addRoundedRect(baseImgRect, imgRadius, imgRadius);
            painter->setClipPath(path);
            QPixmap scaled = scaledCardPixmap(item, poster, baseImgRect.size(), m_style);
            int px = baseImgRect.x() + (baseImgRect.width() - scaled.width()) / 2;
            int py = baseImgRect.y() + (baseImgRect.height() - scaled.height()) / 2;
            painter->drawPixmap(px, py, scaled);
            painter->setClipping(false);
        }
        else
        {
            painter->setPen(placeholderTextColor);
            painter->drawText(baseImgRect, Qt::AlignCenter, tr("No Image"));
        }

        if (isHovered && showPlayButton)
        {
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(0, 0, 0, 70));
            painter->drawRoundedRect(baseImgRect, imgRadius, imgRadius);

            int playSize = 48;
            QRect playRectCenter(baseImgRect.center().x() - playSize / 2, baseImgRect.center().y() - playSize / 2,
                                 playSize, playSize);
            painter->setBrush(QColor(0, 0, 0, 160));
            painter->drawEllipse(playRectCenter);
            QIcon playIcon(":/svg/player/play.svg");
            if (!playIcon.isNull())
            {
                playIcon.paint(painter, playRectCenter.adjusted(12, 12, -12, -12), Qt::AlignCenter);
            }
            else
            {
                QPainterPath playPath;
                playPath.moveTo(playRectCenter.center().x() - 4, playRectCenter.center().y() - 8);
                playPath.lineTo(playRectCenter.center().x() + 10, playRectCenter.center().y());
                playPath.lineTo(playRectCenter.center().x() - 4, playRectCenter.center().y() + 8);
                playPath.closeSubpath();
                painter->setBrush(Qt::white);
                painter->drawPath(playPath);
            }
        }

        if (item.userData.played)
        {
            drawPlayedBadge(painter, baseImgRect, false);
        }

        
        int textX = baseImgRect.right() + 20;
        int favBtnSize = 24;
        int rightMargin = 20;
        int btnAreaWidth = favBtnSize + rightMargin + 10;
        int textWidth = rect.right() - textX - btnAreaWidth - 10;

        QRect titleRect(textX, baseImgRect.top() + 5, textWidth, 24);
        QRect metaRect(textX, titleRect.bottom() + 5, textWidth, 18);
        QRect descRect(textX, metaRect.bottom() + 10, textWidth, baseImgRect.bottom() - metaRect.bottom() - 10);

        QFont titleFont = option.font;
        titleFont.setPixelSize(qMax(8, qRound(16 * fontScale)));
        titleFont.setBold(true);
        painter->setFont(titleFont);
        painter->setPen(titleColor);
        QString epTitle = QString("%1. %2").arg(item.indexNumber).arg(item.name);
        QFontMetrics fmTitle(titleFont);
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          fmTitle.elidedText(epTitle, Qt::ElideRight, titleRect.width()));

        QFont metaFont = option.font;
        metaFont.setPixelSize(qMax(8, qRound(13 * fontScale)));
        painter->setFont(metaFont);
        painter->setPen(subTitleColor);
        QStringList metas;
        if (!item.premiereDate.isEmpty())
        {
            const int len = item.premiereDate.length();
            if (len > 0)
                metas << item.premiereDate.left(qMin(len, 10));
        }
        if (item.runTimeTicks > 0)
        {
            long long mins = item.runTimeTicks / 10000000 / 60;
            metas << QString(tr("%1 min")).arg(mins);
        }
        painter->drawText(metaRect, Qt::AlignLeft | Qt::AlignVCenter, metas.join(" • "));

        QFont descFont = option.font;
        descFont.setPixelSize(qMax(8, qRound(13 * fontScale)));
        painter->setFont(descFont);
        painter->setPen(subTitleColor);

        QString desc = item.overview;
        desc.replace('\n', ' ');
        QFontMetrics fmDesc(descFont);
        int availableHeight = descRect.height();
        int lineSpacing = fmDesc.lineSpacing();
        int maxLines = availableHeight / lineSpacing;
        if (maxLines < 1)
            maxLines = 1;

        QString elidedDesc = fmDesc.elidedText(desc, Qt::ElideRight, descRect.width() * maxLines);
        painter->drawText(descRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, elidedDesc);

        
        if (showFavoriteButton)
        {
            QRect favRect(rect.right() - rightMargin - favBtnSize, rect.center().y() - favBtnSize / 2, favBtnSize,
                          favBtnSize);
            QString themeFolder = isDarkTheme ? "dark" : "light";
            QString favIconPath = item.isFavorite() ? QString(":/svg/%1/heart-fill.svg").arg(themeFolder)
                                                    : QString(":/svg/%1/heart-outline.svg").arg(themeFolder);
            QIcon favIcon(favIconPath);
            if (!favIcon.isNull())
            {
                favIcon.paint(painter, favRect.adjusted(2, 2, -2, -2));
            }
        }

        painter->restore();
        return;
    }

    
    
    

    
    if (option.state & QStyle::State_Selected)
    {
        painter->setPen(Qt::NoPen);
        painter->setBrush(selectedBg);
        painter->drawRoundedRect(rect, 8, 8);
    }

    const int padding = m_contentPadding;
    QRect baseImgRect, titleRect, yearRect;

    
    if (m_style == Poster)
    {
        baseImgRect = baseImageRectForCard(m_style, rect, padding);
        const int imgWidth = baseImgRect.width();
        const int titleHeight = qMax(16, m_titleFontPixelSize + 6);
        const int subTitleHeight = qMax(14, m_subTitleFontPixelSize + 4);
        titleRect = QRect(rect.x() + padding, baseImgRect.bottom() + 4, imgWidth, titleHeight);
        yearRect = QRect(rect.x() + padding, titleRect.bottom() + 1, imgWidth, subTitleHeight);
    }
    else if (m_style == LibraryTile)
    {
        baseImgRect = baseImageRectForCard(m_style, rect, padding);
        const int imgWidth = baseImgRect.width();
        const int titleHeight = qMax(16, m_titleFontPixelSize + 6);
        const int subTitleHeight = qMax(14, m_subTitleFontPixelSize + 4);
        titleRect = QRect(rect.x() + padding, baseImgRect.bottom() + 4, imgWidth, titleHeight);
        yearRect = QRect(rect.x() + padding, titleRect.bottom() + 1, imgWidth, subTitleHeight);
    }
    else if (m_style == Cast)
    {
        baseImgRect = baseImageRectForCard(m_style, rect, padding);
        const int imgWidth = baseImgRect.width();
        const int titleHeight = qMax(16, m_titleFontPixelSize + 6);
        const int subTitleHeight = qMax(14, m_subTitleFontPixelSize + 4);
        titleRect = QRect(rect.x() + padding, baseImgRect.bottom() + 5, imgWidth, titleHeight);
        yearRect = QRect(rect.x() + padding, titleRect.bottom() + 1, imgWidth, subTitleHeight);
    }

    
    QRect targetImgRect = baseImgRect;

    if (isHovered)
    {
        targetImgRect = hoverImageRect(baseImgRect);
        const int expandH = targetImgRect.bottom() - baseImgRect.bottom();
        titleRect.translate(0, expandH);
        yearRect.translate(0, expandH);

        
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0, 0, 0, 60)); 
        
        painter->drawRoundedRect(targetImgRect.translated(0, 6), 12, 12);
    }

    
    int imgRadius = 8;
    painter->setPen(Qt::NoPen);
    painter->setBrush(placeholderBg);
    painter->drawRoundedRect(targetImgRect, imgRadius, imgRadius);

    
    if (!poster.isNull())
    {
        QPainterPath path;
        path.addRoundedRect(targetImgRect, imgRadius, imgRadius);
        painter->setClipPath(path);

        
        QPixmap scaled = scaledCardPixmap(item, poster, targetImgRect.size(), m_style);
        int px = targetImgRect.x() + (targetImgRect.width() - scaled.width()) / 2;
        int py = targetImgRect.y() + (targetImgRect.height() - scaled.height()) / 2;

        painter->drawPixmap(px, py, scaled);
        painter->setClipping(false);
    }
    else
    {
        painter->setPen(placeholderTextColor);
        painter->drawText(targetImgRect, Qt::AlignCenter, tr("No Image"));
    }

    // —— 剧集 S/E 标签（进度条上方，半透明黑底白字）——
    // 无论是否有进度条都显示——S/E 是剧集内容的关键信息，应稳定可见。
    // 覆盖两种卡片：
    //  1) Episode 卡片：显示 "S01E02 · 单集名"
    //  2) Series 折叠卡（继续观看折叠为整部剧）：同样显示 "S01E02 · 单集名"（单集名为下一集）
    const bool isEpisode = (item.type == "Episode");
    const bool isSeriesResumeFallback =
        item.type == "Series" && item.isResumeDisplayFallback;
    const bool hasSeasonEpisode =
        (isEpisode || isSeriesResumeFallback) &&
        item.parentIndexNumber >= 0 && item.indexNumber >= 0;
    QRect sxeRect;
    if (hasSeasonEpisode) {
        const int sxeHeight = qMax(18, qRound(18 * fontScale));
        const int sxeMargin = 4;
        // 默认放在图片底部（即使没有进度条也能显示）；有进度条时贴在 barRect 上方。
        sxeRect = QRect(targetImgRect.left() + sxeMargin,
                        targetImgRect.bottom() - sxeHeight - sxeMargin,
                        targetImgRect.width() - sxeMargin * 2,
                        sxeHeight);
    }

    // —— 播放进度条 ——
    // 已播（UserData.PlayedPercentage > 0）时在图片底部画一条进度条；
    // "下一集待播"（playedPercentage == 0，服务器只记录了下集）不画。
    const double playedPct = item.userData.playedPercentage;
    QRect barRect;
    if (playedPct > 0.0)
    {
        const qreal pct = qBound(0.0, playedPct / 100.0, 1.0);
        const int barHeight = qMax(3, qRound(4 * fontScale));
        const int barMargin = 4;
        barRect = QRect(targetImgRect.left() + barMargin,
                        targetImgRect.bottom() - barHeight - barMargin,
                        targetImgRect.width() - barMargin * 2,
                        barHeight);

        // 底槽（半透明黑）
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0, 0, 0, 140));
        painter->drawRoundedRect(barRect, barHeight / 2, barHeight / 2);

        // 已播填充（Emby 红）
        if (pct > 0.0)
        {
            QRect filledRect = barRect;
            filledRect.setWidth(qRound(barRect.width() * pct));
            painter->setBrush(QColor(232, 46, 62, 230));
            painter->drawRoundedRect(filledRect, barHeight / 2, barHeight / 2);
        }
    }

    // S/E 行：定位 + 绘制（必须在进度条之后画，才知道 barRect 真实位置）
    if (hasSeasonEpisode) {
        if (!barRect.isNull()) {
            // 有进度条：S/E 行贴在 barRect 上方
            sxeRect.moveTop(barRect.top() - sxeRect.height() - 4);
        }
        // 半透明黑底（alpha 170 适配浅色封面）
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0, 0, 0, 170));
        painter->drawRoundedRect(sxeRect, 4, 4);

        // 文字：S01E02 · 玉坠风波（单集名缺失时只显示 S01E02）。
        const QString season =
            item.parentIndexNumber >= 10
                ? QStringLiteral("S%1").arg(item.parentIndexNumber)
                : QStringLiteral("S0%1").arg(item.parentIndexNumber);
        const QString episode =
            item.indexNumber >= 10
                ? QStringLiteral("E%1").arg(item.indexNumber)
                : QStringLiteral("E0%1").arg(item.indexNumber);
        // season/episode 均已带各自前缀（S01 / E02），直接拼接
        QString sxeText = QStringLiteral("%1%2").arg(season, episode);
        if (isSeriesResumeFallback) {
            const QString epName = item.resumeEpisodeName.trimmed();
            if (!epName.isEmpty()) {
                sxeText += QStringLiteral(" \u00B7 %1").arg(epName);
            }
        } else if (!item.name.trimmed().isEmpty()) {
            sxeText += QStringLiteral(" \u00B7 %1").arg(item.name);
        }

        QFont sxeFont = option.font;
        sxeFont.setPixelSize(qMax(10, qRound(12 * fontScale)));
        sxeFont.setBold(true);
        painter->setFont(sxeFont);
        painter->setPen(QColor(255, 255, 255, 235));
        QFontMetrics fm(sxeFont);
        const QString elidedSxe =
            fm.elidedText(sxeText, Qt::ElideRight, sxeRect.width() - 12);
        painter->drawText(sxeRect.adjusted(6, 0, -6, 0),
                           Qt::AlignVCenter | Qt::AlignLeft, elidedSxe);
    }

    
    
    
    if ((item.type == "Series" || item.type == "Season") && item.recursiveItemCount > 0)
    {
        QFont badgeFont = option.font;
        badgeFont.setPixelSize(qMax(8, qRound(11 * fontScale)));
        badgeFont.setBold(true);

        
        QString countStr = item.recursiveItemCount > 999 ? "999+" : QString::number(item.recursiveItemCount);

        
        QFontMetrics fm(badgeFont);
        int textWidth = fm.horizontalAdvance(countStr);

        
        int badgeHeight = 22;
        int badgeWidth = qMax(badgeHeight, textWidth + 10);

        
        int badgeMargin = 4;
        QRect badgeRect(targetImgRect.right() - badgeWidth - badgeMargin, targetImgRect.top() + badgeMargin, badgeWidth,
                        badgeHeight);

        
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0, 0, 0, 160));
        
        painter->drawRoundedRect(badgeRect, badgeHeight / 2, badgeHeight / 2);

        
        painter->setFont(badgeFont);
        painter->setPen(Qt::white);
        painter->drawText(badgeRect, Qt::AlignCenter, countStr);
    }

    
    
    
    if ((item.type == "Episode" || item.type == "Season") && item.userData.played)
    {
        drawPlayedBadge(painter, targetImgRect, item.type == "Season");
    }

    
    
    
    if (isHovered && hasHoverButtons)
    {
        
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0, 0, 0, 70));
        painter->drawRoundedRect(targetImgRect, imgRadius, imgRadius);

        const HoverButtonLayout buttonLayout =
            buildHoverButtonLayout(targetImgRect, showPlayButton, showFavoriteButton, showMoreButton);

        if (showPlayButton)
        {
            painter->setBrush(QColor(0, 0, 0, 160));
            painter->drawEllipse(buttonLayout.playRect);

            QIcon playIcon(":/svg/player/play.svg");
            if (!playIcon.isNull())
            {
                playIcon.paint(painter, buttonLayout.playRect.adjusted(12, 12, -12, -12), Qt::AlignCenter);
            }
            else
            {
                QPainterPath playPath;
                playPath.moveTo(buttonLayout.playRect.center().x() - 4, buttonLayout.playRect.center().y() - 8);
                playPath.lineTo(buttonLayout.playRect.center().x() + 10, buttonLayout.playRect.center().y());
                playPath.lineTo(buttonLayout.playRect.center().x() - 4, buttonLayout.playRect.center().y() + 8);
                playPath.closeSubpath();
                painter->setBrush(Qt::white);
                painter->drawPath(playPath);
            }
        }

        if (showMoreButton)
        {
            painter->setBrush(QColor(0, 0, 0, 160));
            painter->drawRoundedRect(buttonLayout.moreRect, 6, 6);

            QIcon moreIcon(":/svg/dark/more-line.svg");
            if (!moreIcon.isNull())
            {
                moreIcon.paint(painter, buttonLayout.moreRect.adjusted(4, 4, -4, -4), Qt::AlignCenter);
            }
            else
            {
                painter->setBrush(Qt::white);
                int cx = buttonLayout.moreRect.center().x();
                int cy = buttonLayout.moreRect.center().y();
                painter->drawEllipse(cx - 2, cy - 8, 4, 4);
                painter->drawEllipse(cx - 2, cy - 2, 4, 4);
                painter->drawEllipse(cx - 2, cy + 4, 4, 4);
            }
        }

        if (showFavoriteButton)
        {
            painter->setBrush(QColor(0, 0, 0, 160));
            painter->drawRoundedRect(buttonLayout.favoriteRect, 6, 6);

            QString favIconPath = item.isFavorite() ? ":/svg/dark/heart-fill.svg" : ":/svg/dark/heart-outline.svg";
            QIcon favIcon(favIconPath);
            if (!favIcon.isNull())
            {
                favIcon.paint(painter, buttonLayout.favoriteRect.adjusted(5, 5, -5, -5), Qt::AlignCenter);
            }
            else
            {
                painter->setBrush(Qt::white);
                painter->drawEllipse(buttonLayout.favoriteRect.center(), 4, 4);
            }
        }
    }

    
    
    
    if (!m_highlightedItemId.isEmpty() && item.id == m_highlightedItemId)
    {
        QColor borderColor = isDarkTheme ? QColor(96, 165, 250, 220)  
                                         : QColor(59, 130, 246, 220); 
        QPen highlightPen(borderColor, 2.0);
        painter->setPen(highlightPen);
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(targetImgRect.adjusted(-2, -2, 2, 2), 10, 10);
    }

    
    QFont titleFont = option.font;
    titleFont.setPixelSize(qMax(8, qRound(m_titleFontPixelSize * fontScale)));
    titleFont.setBold(true);
    painter->setFont(titleFont);
    painter->setPen(titleColor);

    QFontMetrics fm(titleFont);
    
    // 标题只显示纯名字——剧集 S/E 已在进度条上方独立画出（避免重复）。
    QString displayTitle = item.name;
    QString elidedTitle = fm.elidedText(displayTitle, Qt::ElideRight, titleRect.width());
    Qt::Alignment titleAlign =
        (m_style == Poster) ? (Qt::AlignHCenter | Qt::AlignVCenter) : (Qt::AlignHCenter | Qt::AlignVCenter);
    painter->drawText(titleRect, titleAlign, elidedTitle);

    
    
    const bool canDrawYear = item.productionYear > 0 && yearRect.isValid() && yearRect.bottom() <= rect.bottom();
    if ((m_style == Poster || m_style == LibraryTile) && canDrawYear)
    {
        QFont subFont = option.font;
        subFont.setPixelSize(qMax(8, qRound(m_subTitleFontPixelSize * fontScale)));
        painter->setFont(subFont);
        painter->setPen(subTitleColor);
        painter->drawText(yearRect, Qt::AlignHCenter | Qt::AlignVCenter, QString::number(item.productionYear));
    }
    else if (m_style == Cast)
    {
        QFont subFont = option.font;
        subFont.setPixelSize(qMax(8, qRound(m_subTitleFontPixelSize * fontScale)));
        painter->setFont(subFont);
        painter->setPen(subTitleColor);
        QFontMetrics fmSub(subFont);
        QString elidedRole = fmSub.elidedText(item.overview, Qt::ElideRight, yearRect.width());
        painter->drawText(yearRect, Qt::AlignHCenter | Qt::AlignVCenter, elidedRole);
    }

    painter->restore();
}
