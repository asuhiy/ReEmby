#include "dvdetectionutils.h"

#include <QDebug>
#include <QString>

namespace DvDetectionUtils {

bool hasVideoStreamData(const MediaSourceInfo &source)
{
    for (const MediaStreamInfo &stream : source.mediaStreams)
    {
        if (stream.type == QStringLiteral("Video"))
            return true;
    }
    return false;
}

bool isPureDolbyVision(const MediaSourceInfo &source)
{
    int videoStreams = 0;
    QString rangeType, range, codecProfile;
    bool result = false;
    for (const MediaStreamInfo &stream : source.mediaStreams)
    {
        if (stream.type != QStringLiteral("Video"))
            continue;
        if (videoStreams == 0)
        {
            rangeType = stream.videoRangeType.trimmed();
            range = stream.videoRange.trimmed();
            codecProfile = stream.profile.trimmed();
        }
        videoStreams++;
        const QString type = stream.videoRangeType.trimmed();
        if (type.compare(QStringLiteral("DOVI"), Qt::CaseInsensitive) == 0)
        {
            result = true;
            break;
        }
        if (type.compare(QStringLiteral("DOVIWithHDR10"), Qt::CaseInsensitive) == 0
            || type.compare(QStringLiteral("DOVIWithSDR"), Qt::CaseInsensitive) == 0
            || type.compare(QStringLiteral("DOVIWithHLG"), Qt::CaseInsensitive) == 0)
        {
            result = false;
            break;
        }
        const QString range = stream.videoRange.trimmed();
        // Emby 旧字段值是 "DolbyVision"（4.8 实测），新字段才是 "DOVI"。
        result = range.compare(QStringLiteral("DOVI"), Qt::CaseInsensitive) == 0
                 || range.compare(QStringLiteral("DolbyVision"), Qt::CaseInsensitive) == 0;
        break;
    }
    qInfo().noquote() << "[DvDetection] DV decode check"
                      << "| videoStreams:" << videoStreams
                      << "| videoRangeType:"
                      << (rangeType.isEmpty() ? QStringLiteral("-") : rangeType)
                      << "| videoRange:"
                      << (range.isEmpty() ? QStringLiteral("-") : range)
                      << "| profile:"
                      << (codecProfile.isEmpty() ? QStringLiteral("-") : codecProfile)
                      << "| pureDv:" << result;
    return result;
}

} // namespace DvDetectionUtils
