#ifndef BILIBILIDANMAKUPROVIDER_H
#define BILIBILIDANMAKUPROVIDER_H

#include "../../api/networkmanager.h"
#include "../../models/danmaku/danmakumodels.h"

class NetworkManager;

// Bilibili danmaku source. Matches media to Bilibili videos via the
// web search (search_type=video), resolves each bvid to a cid through
// /x/player/pagelist, then reads the danmaku from comment.bilibili.com.
// Requires a logged-in Bilibili account (BiliBiliAuthService).
class BiliBiliDanmakuProvider
{
public:
    explicit BiliBiliDanmakuProvider(NetworkManager *networkManager);

    QCoro::Task<QList<DanmakuMatchCandidate>> searchCandidates(
        DanmakuMediaContext context,
        DanmakuProviderConfig config,
        QString manualKeyword = QString()) const;

    QCoro::Task<QList<DanmakuComment>> fetchComments(
        DanmakuMatchCandidate candidate,
        DanmakuProviderConfig config) const;

private:
    NetworkManager *m_networkManager;
};

#endif // BILIBILIDANMAKUPROVIDER_H
