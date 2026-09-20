#ifndef SEARCHAGGREGATOR_H
#define SEARCHAGGREGATOR_H

#include "../../qEmbyCore_global.h"
#include "../../models/media/mediaitem.h"
#include "../../models/profile/serverprofile.h"
#include <qcorotask.h>
#include <QObject>
#include <QList>
#include <QSharedPointer>
#include <atomic>
#include <functional>

class ServerManager;
class NetworkManager;
class ApiClient;

// fan-out 完成状态：由 fanOut() 与各协程共享，QSharedPointer 保证
// 最后一个协程结束后才释放。
struct FanOutState {
    std::atomic<int> completed{0};
    std::atomic<int> succeeded{0};
    int total = 0;
    int generation = 0;
    std::function<void(int attempted, int succeeded)> onComplete;
};

// =============================================================================
// SearchAggregator — 跨服务器聚合查询抽象层
//
// 背景：qEmby 支持同时添加多个 Emby/Jellyfin 服务器（ServerManager::servers()），
// 但 MediaService 只对「当前 active server」工作（内部用 activeClient()）。
// 聚合搜索/聚合历史/聚合收藏需要「对每一个已添加服务器同时发起查询，结果
// 按服务器分组展示」——这就是本类的职责。
//
// 设计要点：
//  1. 每个 server 创建独立的临时 ApiClient（ApiClient(profile, network)），
//     不依赖/不修改 active server —— 聚合视图不切服。
//  2. 流式回调：onServerResult 在每个服务器返回时立即调用（先返回先展示），
//     onComplete 在所有服务器都返回（或超时/失败）后调用。
//  3. 失败隔离：单个服务器超时(5s)或网络错误不影响其他服务器。
//  4. 防竞态：generation 计数 + cancel()。用户快速改关键词时，旧查询的
//     迟到结果会被丢弃。
//
// 注意：本类方法不阻塞 UI 线程（QCoro 协程 + NetworkManager 异步 IO）。
// =============================================================================
class QEMBYCORE_EXPORT SearchAggregator : public QObject {
    Q_OBJECT
public:
    explicit SearchAggregator(ServerManager* serverManager,
                              NetworkManager* network,
                              QObject* parent = nullptr);

    // 单个服务器的一批结果。items 为空表示该服务器无匹配 / 无内容（不视为失败）。
    using ServerResultCallback =
        std::function<void(const ServerProfile& profile, QList<MediaItem> items)>;
    // 全部完成后回调：attempted = 尝试的服务器数, succeeded = 成功返回的服务器数。
    using CompleteCallback =
        std::function<void(int attempted, int succeeded)>;

    // 聚合搜索：在所有已添加服务器中搜索 query。
    // includeItemTypes: "Movie,Series,BoxSet,Person" 或按需过滤。
    // perServerLimit: 每个服务器最多返回条数。
    void searchAll(const QString& query,
                   const QString& includeItemTypes,
                   int perServerLimit,
                   ServerResultCallback onServerResult,
                   CompleteCallback onComplete);

    // 聚合「继续观看」：每个服务器的可续看条目（IsResumable）。
    void getResumeItemsAllServers(int perServerLimit,
                                  ServerResultCallback onServerResult,
                                  CompleteCallback onComplete);

    // 聚合收藏：每个服务器的收藏条目（IsFavorite），可按类型过滤。
    void getFavoritesAllServers(const QString& includeItemTypes,
                                int perServerLimit,
                                ServerResultCallback onServerResult,
                                CompleteCallback onComplete);

    // 取消所有在途查询。已返回的结果回调会被跳过（generation 校验）。
    void cancel();

private:
    // 核心 fan-out 实现：按 ServerManager 里的服务器顺序，并发发起请求，
    // 结果流式回调（onServerResult 每 server 返回即调用）。
    void fanOut(const QString& pathTemplate,
                ServerResultCallback onServerResult,
                CompleteCallback onComplete);

    // 单个服务器的查询协程。
    //
    // 关键：必须用「协程成员函数 + 按值参数」，不能用捕获 lambda——
    // C++ 协程 frame 只拷贝参数（含闭包对象的 this 指针），IIFE 临时
    // lambda 的闭包在表达式结束后销毁，协程恢复后访问捕获即悬垂
    // （searchaggregator.cpp:153 曾因此崩溃）。按值参数会被拷贝进
    // frame，与协程同生命周期。
    QCoro::Task<void> fetchFromServer(ServerProfile profile,
                                      QString pathTemplate,
                                      int gen,
                                      ServerResultCallback onServerResult,
                                      QSharedPointer<FanOutState> state);

    ServerManager* m_serverManager = nullptr;
    NetworkManager* m_network = nullptr;
    int m_generation = 0;
};

#endif // SEARCHAGGREGATOR_H
