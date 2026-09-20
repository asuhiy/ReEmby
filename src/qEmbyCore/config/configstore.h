#ifndef CONFIGSTORE_H
#define CONFIGSTORE_H

#include <QObject>
#include <QVariant>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QSettings>
#include <QMutex>
#include <QMutexLocker>


#include "../qEmbyCore_global.h"

class QEMBYCORE_EXPORT ConfigStore : public QObject {
    Q_OBJECT
public:
    
    static ConfigStore* instance();

    
    template<typename T>
    T get(const QString& key, const T& defaultValue = T()) const {
        QMutexLocker locker(&m_mutex);
        const QString storageKey = canonicalStorageKey(key);
        const QStringList legacyKeys = legacyStorageKeys(key);

        
        if (m_cache.contains(key)) {
            return m_cache.value(key).value<T>();
        }
        if (m_cache.contains(storageKey)) {
            return m_cache.value(storageKey).value<T>();
        }
        for (const QString& legacyKey : legacyKeys) {
            if (m_cache.contains(legacyKey)) {
                return m_cache.value(legacyKey).value<T>();
            }
        }

        
        QVariant val = m_settings->value(storageKey);
        if (!val.isValid()) {
            for (const QString& legacyKey : legacyKeys) {
                val = m_settings->value(legacyKey);
                if (val.isValid()) {
                    break;
                }
            }
        }
        if (!val.isValid()) {
            val = QVariant::fromValue(defaultValue);
        }
        return val.value<T>();
    }

    
    void set(const QString& key, const QVariant& value);
    // 移除一个键（内存缓存 + 磁盘存储）。用于清理已失效的配置，
    // 例如已删除服务器遗留的 per-server 设置。
    void remove(const QString& key);
    // 按前缀批量移除一组键：匹配 "prefix" 本身及其子键 "prefix/..."。
    // 用于删除服务器时清理其全部残留 per-server 配置（server/<id>/...）。
    void removeByPrefix(const QString& prefix);
    void sync();
    QString filePath() const;
    QStringList allKeys() const;

    // True when the key exists in cache or persistent storage (including
    // legacy keys). Lets callers distinguish "explicitly set" from "missing
    // -> use caller default" without a special sentinel value.
    bool has(const QString& key) const;

signals:
    
    void valueChanged(const QString& key, const QVariant& newValue);

private:
    explicit ConfigStore(QObject* parent = nullptr);
    ~ConfigStore() override;
    static QString canonicalStorageKey(const QString& key);
    static QStringList legacyStorageKeys(const QString& key);
    void migrateLegacyGeneralSettings();

    
    Q_DISABLE_COPY(ConfigStore)

    QSettings* m_settings;
    mutable QMutex m_mutex;             
    QHash<QString, QVariant> m_cache;   
};

#endif 
