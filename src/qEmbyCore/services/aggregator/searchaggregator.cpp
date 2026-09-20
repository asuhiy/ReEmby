#include "searchaggregator.h"
#include "../../api/apiclient.h"
#include "../../config/config_keys.h"
#include "../../config/configstore.h"
#include "../manager/servermanager.h"
#include <qcorotask.h>
#include <qcoronetwork.h>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QSharedPointer>
#include <atomic>

namespace {
// 单个服务器 API 超时（毫秒）：超时视为该服务器失败，不阻塞其他。
constexpr int kPerServerTimeoutMs = 5000;

// 与 MediaService 卡片 tooltip 字段保持一致，保证年份/集数/可下载等齐全。
// UserData 加进来：聚合继续观看需要 PlayedPercentage 画进度条、PlaybackPositionTicks
// 算剩余、LastPlayedDate 按时间倒序。
QString mediaCardFields()
{
    return QStringLiteral(
        "ProductionYear,RecursiveItemCount,CanDownload,"
        "Overview,Genres,CommunityRating,CriticRating,"
        "OfficialRating,RunTimeTicks,People,Studios,"
        "ProviderIds,ChildCount,ParentId,Path,MediaStreams,"
        "UserData");
}
} // namespace

SearchAggregator::SearchAggregator(ServerManager* serverManager,
                                   NetworkManager* network,
                                   QObject* parent)
    : QObject(parent)
    , m_serverManager(serverManager)
    , m_network(network)
{
}

void SearchAggregator::cancel()
{
    ++m_generation;
}

void SearchAggregator::searchAll(const QString& query,
                                 const QString& includeItemTypes,
                                 int perServerLimit,
                                 ServerResultCallback onServerResult,
                                 CompleteCallback onComplete)
{
    QString pathTemplate =
        QStringLiteral("/Users/%1/Items?SearchTerm=%2&Recursive=true&Fields=%3")
            .arg(QStringLiteral("{uid}"),
                 QString::fromUtf8(QUrl::toPercentEncoding(query)),
                 mediaCardFields());
    if (!includeItemTypes.isEmpty())
        pathTemplate += QStringLiteral("&IncludeItemTypes=%1").arg(includeItemTypes);
    if (perServerLimit > 0)
        pathTemplate += QStringLiteral("&Limit=%1").arg(perServerLimit);
    fanOut(pathTemplate, std::move(onServerResult), std::move(onComplete));
}

void SearchAggregator::getResumeItemsAllServers(
    int perServerLimit, ServerResultCallback onServerResult,
    CompleteCallback onComplete)
{
    // 聚合继续观看：与主页 dashboard 完全同源——Emby 专用
    // /Users/{uid}/Items/Resume 端点（MediaTypes=Video，Movie + Episode
    // 混合）。之前误用 /Items?Filters=IsResumable&IncludeItemTypes=Episode，
    // 把电影条目全部过滤掉，导致聚合历史和主页继续观看对不上。
    // S/E 标签（S01E02）由 MediaCardDelegate 按 item.type==Episode 自动
    // 绘制，电影不显示 S/E，无需在查询层过滤。
    QString pathTemplate =
        QStringLiteral("/Users/%1/Items/Resume?Recursive=true&MediaTypes=Video"
                      "&Fields=%2&EnableImageTypes=Primary,Backdrop,Thumb"
                      "&ImageTypeLimit=1")
            .arg(QStringLiteral("{uid}"), mediaCardFields());
    if (perServerLimit > 0)
        pathTemplate += QStringLiteral("&Limit=%1").arg(perServerLimit);
    fanOut(pathTemplate, std::move(onServerResult), std::move(onComplete));
}

void SearchAggregator::getFavoritesAllServers(
    const QString& includeItemTypes, int perServerLimit,
    ServerResultCallback onServerResult, CompleteCallback onComplete)
{
    QString pathTemplate =
        QStringLiteral("/Users/%1/Items?Filters=IsFavorite&Recursive=true&Fields=%2")
            .arg(QStringLiteral("{uid}"), mediaCardFields());
    if (!includeItemTypes.isEmpty())
        pathTemplate += QStringLiteral("&IncludeItemTypes=%1").arg(includeItemTypes);
    if (perServerLimit > 0)
        pathTemplate += QStringLiteral("&Limit=%1").arg(perServerLimit);
    fanOut(pathTemplate, std::move(onServerResult), std::move(onComplete));
}

void SearchAggregator::fanOut(const QString& pathTemplate,
                              ServerResultCallback onServerResult,
                              CompleteCallback onComplete)
{
    if (!m_serverManager) {
        if (onComplete) onComplete(0, 0);
        return;
    }

    // 只把「有效且参与聚合」的服务器纳入 fan-out（设置 → 媒体库 →
    // 参与聚合的服务）。total 与实际发起的协程数保持一致，保证
    // completed/total 配平、onComplete 正常触发。
    QList<ServerProfile> servers;
    for (const ServerProfile& profile : m_serverManager->servers()) {
        if (!profile.isValid()) {
            continue;
        }
        if (!ConfigStore::instance()->get<bool>(
                ConfigKeys::forServer(profile.id, ConfigKeys::AggregateEnabled),
                true)) {
            continue;
        }
        servers.append(profile);
    }
    if (servers.isEmpty()) {
        if (onComplete) onComplete(0, 0);
        return;
    }

    const int gen = ++m_generation;
    auto state = QSharedPointer<FanOutState>::create();
    state->total = servers.size();
    state->generation = gen;
    state->onComplete = std::move(onComplete);

    for (const ServerProfile& profile : servers) {
        if (!profile.isValid()) {
            ++state->completed;
            continue;
        }

        // 每个服务器一个协程：co_await 期间不阻塞 UI 线程，13+ 个协程
        // 在网络等待时交错执行 → 天然并发。
        //
        // 关键：fetchFromServer 是协程成员函数，所有数据通过按值参数
        // 传入 —— C++ 标准保证协程参数拷贝进 frame，与协程同生命周期。
        // （之前用 IIFE 捕获 lambda：闭包对象在表达式结束后销毁，协程
        // 恢复后访问捕获悬垂 → searchaggregator.cpp:153 写 NULL 崩溃，
        // PDB 符号化确认。）
        QCoro::connect(
            fetchFromServer(profile, pathTemplate, gen, onServerResult, state),
            this, []() {});
    }
}

QCoro::Task<void> SearchAggregator::fetchFromServer(
    ServerProfile profile, QString pathTemplate, int gen,
    ServerResultCallback onServerResult,
    QSharedPointer<FanOutState> state)
{
    // 独立临时 ApiClient——不依赖/不切换 active server。
    // 堆分配 + parent=SearchAggregator：析构自动回收，且 deleteLater
    // 之前协程一定已结束。
    auto *apiClient = new ApiClient(profile, m_network, this);

    QList<MediaItem> items;
    try {
        QString path = pathTemplate;
        path.replace(QStringLiteral("{uid}"), profile.userId);
        const QJsonObject response =
            co_await apiClient->get(path, kPerServerTimeoutMs);
        const QJsonArray arr =
            response.value(QStringLiteral("Items")).toArray();
        items.reserve(arr.size());
        for (const QJsonValue& v : arr) {
            // 跨服路由：标记 item 所属 server，详情/图片/播放按这个路由
            // 到对应 server（避免用 active server 404）。
            MediaItem item = MediaItem::fromJson(v.toObject());
            item.serverId = profile.id;
            items.append(item);
        }
        ++state->succeeded;
    } catch (...) {
        // 单服务器失败/超时不阻塞其他（失败隔离）。
        items.clear();
    }

    // 流式回调：结果立即上抛（若本 generation 仍有效）。
    if (onServerResult && gen == m_generation) {
        onServerResult(profile, items);
    }

    if (++state->completed == state->total) {
        if (state->onComplete && gen == m_generation) {
            state->onComplete(state->total, state->succeeded.load());
        }
    }

    // 本次请求已结束，释放临时 ApiClient（延迟到事件循环，此时所有
    // 回调已完成）。避免反复 startLoad 时对象在 SearchAggregator 上累积。
    apiClient->deleteLater();
    co_return;
}
