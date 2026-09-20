#include "skipsegmentsstore.h"

#include <utils/apppaths.h>

#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>

namespace
{
constexpr char kFileName[] = "skip-segments.json";
constexpr char kGlobalKey[] = "global";
constexpr char kSeriesKey[] = "series";
constexpr char kItemsKey[] = "items";
constexpr char kIntroKey[] = "introSec";
constexpr char kOutroKey[] = "outroSec";

SkipSegmentsStore::Lengths lengthsFromJson(const QJsonObject &obj)
{
    SkipSegmentsStore::Lengths lengths;
    lengths.introSec = qMax(0, obj.value(QLatin1String(kIntroKey)).toInt(0));
    lengths.outroSec = qMax(0, obj.value(QLatin1String(kOutroKey)).toInt(0));
    return lengths;
}

QJsonObject lengthsToJson(const SkipSegmentsStore::Lengths &lengths)
{
    QJsonObject obj;
    obj.insert(QLatin1String(kIntroKey), lengths.introSec);
    obj.insert(QLatin1String(kOutroKey), lengths.outroSec);
    return obj;
}
} // namespace

SkipSegmentsStore *SkipSegmentsStore::instance()
{
    static SkipSegmentsStore store;
    return &store;
}

QString SkipSegmentsStore::filePath() const
{
    return AppPaths::dataRoot() + QLatin1Char('/') + QLatin1String(kFileName);
}

void SkipSegmentsStore::ensureLoaded() const
{
    if (m_loaded)
    {
        return;
    }
    m_loaded = true;

    QFile file(filePath());
    if (!file.exists())
    {
        return;
    }
    if (!file.open(QIODevice::ReadOnly))
    {
        qWarning() << "[SkipSegments] failed to open" << filePath()
                   << "|" << file.errorString();
        return;
    }
    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
    {
        qWarning() << "[SkipSegments] invalid json" << filePath()
                   << "|" << error.errorString();
        return;
    }

    const QJsonObject root = doc.object();
    m_global = lengthsFromJson(root.value(QLatin1String(kGlobalKey)).toObject());

    const QJsonObject seriesObj = root.value(QLatin1String(kSeriesKey)).toObject();
    for (auto it = seriesObj.constBegin(); it != seriesObj.constEnd(); ++it)
    {
        m_series.insert(it.key(), lengthsFromJson(it.value().toObject()));
    }

    const QJsonObject itemsObj = root.value(QLatin1String(kItemsKey)).toObject();
    for (auto it = itemsObj.constBegin(); it != itemsObj.constEnd(); ++it)
    {
        m_items.insert(it.key(), lengthsFromJson(it.value().toObject()));
    }

    qInfo() << "[SkipSegments] loaded" << filePath()
            << "| global:" << m_global.introSec << "/" << m_global.outroSec
            << "| series:" << m_series.size()
            << "| items:" << m_items.size();
}

void SkipSegmentsStore::save()
{
    QJsonObject root;
    root.insert(QLatin1String(kGlobalKey), lengthsToJson(m_global));

    QJsonObject seriesObj;
    for (auto it = m_series.constBegin(); it != m_series.constEnd(); ++it)
    {
        seriesObj.insert(it.key(), lengthsToJson(it.value()));
    }
    root.insert(QLatin1String(kSeriesKey), seriesObj);

    QJsonObject itemsObj;
    for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it)
    {
        itemsObj.insert(it.key(), lengthsToJson(it.value()));
    }
    root.insert(QLatin1String(kItemsKey), itemsObj);

    QSaveFile file(filePath());
    if (!file.open(QIODevice::WriteOnly))
    {
        qWarning() << "[SkipSegments] failed to write" << filePath()
                   << "|" << file.errorString();
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit())
    {
        qWarning() << "[SkipSegments] commit failed" << filePath()
                   << "|" << file.errorString();
        return;
    }
    qInfo() << "[SkipSegments] saved" << filePath()
            << "| global:" << m_global.introSec << "/" << m_global.outroSec
            << "| series:" << m_series.size()
            << "| items:" << m_items.size();
}

SkipSegmentsStore::Lengths SkipSegmentsStore::global() const
{
    ensureLoaded();
    return m_global;
}

void SkipSegmentsStore::setGlobal(const Lengths &lengths)
{
    ensureLoaded();
    if (m_global == lengths)
    {
        return;
    }
    m_global = lengths;
    save();
}

std::optional<SkipSegmentsStore::Lengths>
SkipSegmentsStore::seriesEntry(const QString &seriesId) const
{
    ensureLoaded();
    if (seriesId.trimmed().isEmpty())
    {
        return std::nullopt;
    }
    const auto it = m_series.constFind(seriesId);
    if (it == m_series.constEnd())
    {
        return std::nullopt;
    }
    return it.value();
}

void SkipSegmentsStore::setSeriesEntry(const QString &seriesId,
                                       const Lengths &lengths)
{
    ensureLoaded();
    if (seriesId.trimmed().isEmpty())
    {
        return;
    }
    if (m_series.value(seriesId) == lengths && m_series.contains(seriesId))
    {
        return;
    }
    m_series.insert(seriesId, lengths);
    save();
}

void SkipSegmentsStore::clearSeriesEntry(const QString &seriesId)
{
    ensureLoaded();
    // Qt6：QHash::remove 返回 bool（是否移除过），不要与 0 比较。
    if (m_series.remove(seriesId))
    {
        save();
    }
}

std::optional<SkipSegmentsStore::Lengths>
SkipSegmentsStore::itemEntry(const QString &itemId) const
{
    ensureLoaded();
    if (itemId.trimmed().isEmpty())
    {
        return std::nullopt;
    }
    const auto it = m_items.constFind(itemId);
    if (it == m_items.constEnd())
    {
        return std::nullopt;
    }
    return it.value();
}

void SkipSegmentsStore::setItemEntry(const QString &itemId,
                                     const Lengths &lengths)
{
    ensureLoaded();
    if (itemId.trimmed().isEmpty())
    {
        return;
    }
    if (m_items.value(itemId) == lengths && m_items.contains(itemId))
    {
        return;
    }
    m_items.insert(itemId, lengths);
    save();
}

void SkipSegmentsStore::clearItemEntry(const QString &itemId)
{
    ensureLoaded();
    // Qt6：QHash::remove 返回 bool（是否移除过），不要与 0 比较。
    if (m_items.remove(itemId))
    {
        save();
    }
}

SkipSegmentsStore::Lengths SkipSegmentsStore::resolve(const QString &seriesId,
                                                      const QString &itemId) const
{
    ensureLoaded();
    if (!seriesId.trimmed().isEmpty())
    {
        const auto it = m_series.constFind(seriesId);
        if (it != m_series.constEnd())
        {
            return it.value();
        }
    }
    if (!itemId.trimmed().isEmpty())
    {
        const auto it = m_items.constFind(itemId);
        if (it != m_items.constEnd())
        {
            return it.value();
        }
    }
    return m_global;
}
