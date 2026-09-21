#ifndef ICONPACKSERVICE_H
#define ICONPACKSERVICE_H

#include "../../qEmbyCore_global.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <qcorotask.h>

class QNetworkAccessManager;

// 图标源里的一个图标条目，对应 JSON 里的 {name, url}。
struct QEMBYCORE_EXPORT IconPackEntry {
    QString name;
    QString url;

    bool isValid() const { return !url.trimmed().isEmpty(); }
};

// 图标源下拉列表里的一项。
// 内置源的 displayName 是固定文案（因为同一个作者的多个 JSON 文件名
// 可能完全一样，只靠 JSON 里的 name 分不出来）；自定义源的 displayName
// 在拉取成功后由 JSON 的 name 补齐并持久化。
struct QEMBYCORE_EXPORT IconPackSource {
    QString url;
    QString displayName;
    bool builtin = false;

    bool isValid() const { return !url.trimmed().isEmpty(); }
};

// 一次拉取的结果。失败时 errorMessage 非空、entries 为空。
struct QEMBYCORE_EXPORT IconPack {
    QString sourceUrl;
    QString name;         // JSON 的 name（已去掉 "@作者" 后缀）
    QString description;  // JSON 的 description，显示在对话框第二行
    QList<IconPackEntry> entries;
    QString errorMessage;

    bool succeeded() const { return errorMessage.isEmpty(); }
    int count() const { return static_cast<int>(entries.size()); }
};

// 图标源服务。
//
// 网络层用**自己**的 QNetworkAccessManager + QNetworkDiskCache，不走
// NetworkManager：后者没有挂磁盘缓存，而且非 2xx 会直接抛异常，没法做
// 条件请求。
//
// Qt 的 QNetworkDiskCache 会自动带上 If-None-Match / If-Modified-Since
// 并在服务端返回 304 时把缓存内容当作响应体交回来，所以"判断图标源有没有
// 更新"不需要自己实现 —— 这也是它比手写 ETag 更可靠的原因：
//   缓存新鲜（GitHub raw 是 Cache-Control: max-age=300）=> 不发请求
//   缓存过期                                  => 发条件请求，304 复用缓存
class QEMBYCORE_EXPORT IconPackService : public QObject
{
    Q_OBJECT

public:
    static IconPackService *instance();

    IconPackService(const IconPackService &) = delete;
    IconPackService &operator=(const IconPackService &) = delete;

    // 内置的 6 个图标源，顺序固定（与设计稿一致）。
    static QList<IconPackSource> builtinSources();

    // 全部源 = 内置 + 自定义（自定义按添加顺序追加在后面）。
    QList<IconPackSource> sources() const;

    bool isBuiltinSource(const QString &url) const;

    // 添加自定义源。url 非法 / 已存在（含与内置重复）时返回 false。
    bool addCustomSource(const QString &rawUrl);

    // 删除自定义源。内置源返回 false。
    bool removeCustomSource(const QString &rawUrl);

    // 拉取成功后用 JSON 里的 name 刷新自定义源的显示名（内置源忽略）。
    void updateCustomSourceName(const QString &rawUrl, const QString &name);

    // 拉取并解析一个图标源。走磁盘缓存，见类注释。
    QCoro::Task<IconPack> loadPack(QString sourceUrl);

    // 下载单个图标图片的原始字节。同样走磁盘缓存。
    QCoro::Task<QByteArray> downloadIcon(QString url);

Q_SIGNALS:
    // 自定义源列表发生变化（添加 / 删除 / 显示名刷新）。
    void sourcesChanged();

private:
    explicit IconPackService(QObject *parent = nullptr);

    // 只接受 http/https 的合法地址，其余返回空串。
    static QString normalizeUrl(const QString &rawUrl);

    // 去掉 JSON name 里的 "@作者" 后缀。
    static QString cleanName(const QString &rawName);

    static IconPack parsePack(const QByteArray &payload, const QString &sourceUrl);

    void loadCustomSources();
    void saveCustomSources();

    QNetworkAccessManager *m_manager = nullptr;
    QList<IconPackSource> m_customSources;
};

#endif  // ICONPACKSERVICE_H
