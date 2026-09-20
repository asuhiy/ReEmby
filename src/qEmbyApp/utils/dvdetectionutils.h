#ifndef DVDETECTIONUTILS_H
#define DVDETECTIONUTILS_H

#include <models/media/playbackinfo.h>

// Dolby Vision 片源检测（从 playerview.cpp 的匿名命名空间提取，供
// PlaybackManager 等复用：DV 自动独立窗口 / 强制软解决策）。
namespace DvDetectionUtils {

// 判定数据是否可用：没有任何 Video 流说明 sourceInfo 来自列表 API
//（不带 MediaStreams），判定没有依据，需要拉 detail 复查。
bool hasVideoStreamData(const MediaSourceInfo &source);

// 纯 Dolby Vision（profile 5，无 HDR10/SDR 兼容层）。硬解会丢弃携带 DV
// 元数据的 RPU NAL → 画面发绿；内嵌的 render API 也不处理 DV 的 IPT 色彩。
// 这类片源需要软解，或走独立窗口（wid + gpu-next）。
// Emby 字段语义：VideoRangeType "DOVI" = 纯 DV；"DOVIWithHDR10/WithSDR/
// WithHLG" = 有兼容层的 hybrid，颜色正常，不做覆盖。
// 旧版 Emby 只有 VideoRange（值 "DOVI"/"DolbyVision"，无法区分 profile）时，
// 保守按纯 DV 处理（误伤 hybrid 只多耗 CPU，漏判则发绿不可看）。
bool isPureDolbyVision(const MediaSourceInfo &source);

} // namespace DvDetectionUtils

#endif // DVDETECTIONUTILS_H
