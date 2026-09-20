#ifndef SKIPSEGMENTSSTORE_H
#define SKIPSEGMENTSSTORE_H

#include "qEmbyCore_global.h"

#include <QHash>
#include <QString>
#include <optional>

// 手动"跳过片头/片尾"时长存储：全局默认 + 按剧/按条目覆盖。
//
// 文件：<数据根>/skip-segments.json
//   {
//     "global": { "introSec": 90, "outroSec": 120 },
//     "series": { "<seriesId>": { "introSec": 60, "outroSec": 120 } },
//     "items":  { "<itemId>":   { "introSec": 30, "outroSec": 90 } }
//   }
//
// 语义：
//   - series/items 记录存在时整条生效（缺省字段视为 0 = 不跳过该项）；
//   - 记录不存在时继承 global；
//   - global 缺省为 0/0（即不跳过）。
//   - 0 表示"不跳过"（不是"未设置"）。
class QEMBYCORE_EXPORT SkipSegmentsStore
{
public:
    struct Lengths
    {
        int introSec = 0;
        int outroSec = 0;

        bool operator==(const Lengths &other) const
        {
            return introSec == other.introSec && outroSec == other.outroSec;
        }
        bool operator!=(const Lengths &other) const { return !(*this == other); }
    };

    static SkipSegmentsStore *instance();

    Lengths global() const;
    void setGlobal(const Lengths &lengths);

    std::optional<Lengths> seriesEntry(const QString &seriesId) const;
    void setSeriesEntry(const QString &seriesId, const Lengths &lengths);
    void clearSeriesEntry(const QString &seriesId);

    std::optional<Lengths> itemEntry(const QString &itemId) const;
    void setItemEntry(const QString &itemId, const Lengths &lengths);
    void clearItemEntry(const QString &itemId);

    // 有效值：按剧覆盖 > 按条目覆盖 > 全局默认。
    Lengths resolve(const QString &seriesId, const QString &itemId) const;

private:
    SkipSegmentsStore() = default;
    SkipSegmentsStore(const SkipSegmentsStore &) = delete;
    SkipSegmentsStore &operator=(const SkipSegmentsStore &) = delete;

    QString filePath() const;
    void ensureLoaded() const;
    void save();

    // 延迟加载缓存：查询入口都是 const 方法（resolve/global/...），故全部
    // 数据成员标 mutable（MSVC 下 const 方法里写非 mutable 成员 = C2678）。
    mutable bool m_loaded = false;
    mutable Lengths m_global;
    mutable QHash<QString, Lengths> m_series;
    mutable QHash<QString, Lengths> m_items;
};

#endif // SKIPSEGMENTSSTORE_H
