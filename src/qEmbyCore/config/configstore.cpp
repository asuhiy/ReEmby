#include "configstore.h"
#include "config_keys.h"
#include <QDir>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QDebug>
#include <utils/apppaths.h>
#include <utility>

ConfigStore* ConfigStore::instance() {
    
    static ConfigStore* s_instance = new ConfigStore(qApp);
    return s_instance;
}

QString ConfigStore::canonicalStorageKey(const QString& key) {
    
    
    
    if (key.startsWith(QStringLiteral("general/"))) {
        return QStringLiteral("app/") + key.mid(8);
    }
    return key;
}

QStringList ConfigStore::legacyStorageKeys(const QString& key) {
    if (!key.startsWith(QStringLiteral("general/"))) {
        return {};
    }

    const QString suffix = key.mid(8);
    return {
        key,
        QStringLiteral("General/") + suffix,
    };
}

ConfigStore::ConfigStore(QObject* parent) : QObject(parent) {
    
    QString configPath = AppPaths::dataRoot();
    QDir().mkpath(configPath);
    QString configFile = configPath + "/config.ini";

    
    m_settings = new QSettings(configFile, QSettings::IniFormat, this);
    qDebug() << "ConfigStore: using config file" << configFile;
    migrateLegacyGeneralSettings();

    
    if (!m_settings->contains(ConfigKeys::ShowLatestAdded)) {
        m_settings->setValue(ConfigKeys::ShowLatestAdded, true);
    }
    if (!m_settings->contains(ConfigKeys::ShowMediaLibraries)) {
        m_settings->setValue(ConfigKeys::ShowMediaLibraries, true);
    }
    if (!m_settings->contains(ConfigKeys::ShowEachLibrary)) {
        m_settings->setValue(ConfigKeys::ShowEachLibrary, true);
    }
    if (!m_settings->contains(ConfigKeys::ShowMediaTooltips)) {
        m_settings->setValue(ConfigKeys::ShowMediaTooltips, true);
    }
    if (!m_settings->contains(ConfigKeys::ImageQuality)) {
        m_settings->setValue(ConfigKeys::ImageQuality, "high");
    }
    if (!m_settings->contains(ConfigKeys::DataCacheDuration)) {
        m_settings->setValue(ConfigKeys::DataCacheDuration, "24");
    }
    if (!m_settings->contains(ConfigKeys::ImageCacheDuration)) {
        m_settings->setValue(ConfigKeys::ImageCacheDuration, "7");
    }
    if (!m_settings->contains(canonicalStorageKey(ConfigKeys::CheckForUpdates))) {
        m_settings->setValue(canonicalStorageKey(ConfigKeys::CheckForUpdates), true);
    }
}

ConfigStore::~ConfigStore() {
    if (m_settings) {
        m_settings->sync(); 
    }
}

void ConfigStore::migrateLegacyGeneralSettings() {
    const QStringList generalKeys = {
        ConfigKeys::Language,
        ConfigKeys::RememberServer,
        ConfigKeys::LastSelectedServerId,
        ConfigKeys::CloseToTray,
        ConfigKeys::SingleApplication,
        ConfigKeys::CheckForUpdates,
        ConfigKeys::IgnoredUpdateVersion,
        ConfigKeys::LogEnable,
        ConfigKeys::ApiTimeout,
        ConfigKeys::ImageCacheLimit,
    };

    bool migrated = false;
    for (const QString& key : generalKeys) {
        const QString storageKey = canonicalStorageKey(key);
        if (m_settings->contains(storageKey)) {
            continue;
        }

        for (const QString& legacyKey : legacyStorageKeys(key)) {
            if (!m_settings->contains(legacyKey)) {
                continue;
            }

            m_settings->setValue(storageKey, m_settings->value(legacyKey));
            qDebug() << "ConfigStore: migrated legacy key" << legacyKey
                     << "to" << storageKey;
            migrated = true;
            break;
        }
    }

    if (migrated) {
        m_settings->sync();
        if (m_settings->status() != QSettings::NoError) {
            qWarning() << "ConfigStore: failed to sync migrated config"
                       << m_settings->fileName()
                       << "| status:" << m_settings->status();
        }
    }
}

void ConfigStore::set(const QString& key, const QVariant& value) {
    const QString storageKey = canonicalStorageKey(key);
    m_mutex.lock();

    
    QVariant oldValue;
    if (m_cache.contains(key)) {
        oldValue = m_cache.value(key);
    } else if (m_cache.contains(storageKey)) {
        oldValue = m_cache.value(storageKey);
    } else {
        oldValue = m_settings->value(storageKey);
        if (!oldValue.isValid()) {
            for (const QString& legacyKey : legacyStorageKeys(key)) {
                oldValue = m_settings->value(legacyKey);
                if (oldValue.isValid()) {
                    break;
                }
            }
        }
    }

    
    if (oldValue == value && oldValue.isValid()) {
        m_mutex.unlock();
        return;
    }

    
    m_cache.insert(key, value);
    m_cache.insert(storageKey, value);
    
    m_settings->setValue(storageKey, value);
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        qWarning() << "ConfigStore: failed to sync config"
                   << m_settings->fileName() << "| status:" << m_settings->status();
    }

    
    m_mutex.unlock();

    
    emit valueChanged(key, value);
}

void ConfigStore::remove(const QString& key) {
    const QString storageKey = canonicalStorageKey(key);
    m_mutex.lock();

    
    const bool hadValue = m_cache.contains(key) || m_cache.contains(storageKey) ||
                          m_settings->contains(storageKey);

    m_cache.remove(key);
    m_cache.remove(storageKey);
    m_settings->remove(storageKey);
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        qWarning() << "ConfigStore: failed to sync config"
                   << m_settings->fileName() << "| status:" << m_settings->status();
    }

    m_mutex.unlock();

    
    if (hadValue) {
        emit valueChanged(key, QVariant());
    }
}

void ConfigStore::removeByPrefix(const QString& prefix) {
    if (prefix.isEmpty()) {
        return;
    }
    const QString groupPrefix = prefix + QLatin1Char('/');
    const auto matches = [&prefix, &groupPrefix](const QString& k) {
        return k == prefix || k.startsWith(groupPrefix);
    };

    m_mutex.lock();

    QStringList removedKeys;

    
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        if (matches(it.key())) {
            if (!removedKeys.contains(it.key())) {
                removedKeys.append(it.key());
            }
            it = m_cache.erase(it);
        } else {
            ++it;
        }
    }

    
    // 注：此处不能命名 allKeys——会遮蔽同名成员函数 ConfigStore::allKeys()。
    const QStringList storedKeys = m_settings->allKeys();
    for (const QString& k : storedKeys) {
        if (matches(k)) {
            if (!removedKeys.contains(k)) {
                removedKeys.append(k);
            }
            m_settings->remove(k);
        }
    }
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        qWarning() << "ConfigStore: failed to sync config"
                   << m_settings->fileName() << "| status:" << m_settings->status();
    }

    m_mutex.unlock();

    
    for (const QString& k : std::as_const(removedKeys)) {
        emit valueChanged(k, QVariant());
    }
    if (!removedKeys.isEmpty()) {
        qInfo() << "ConfigStore: removed" << removedKeys.size()
                << "key(s) under" << prefix;
    }
}

bool ConfigStore::has(const QString& key) const {
    QMutexLocker locker(&m_mutex);
    const QString storageKey = canonicalStorageKey(key);
    if (m_cache.contains(key) || m_cache.contains(storageKey)) {
        return true;
    }
    if (m_settings->contains(storageKey)) {
        return true;
    }
    for (const QString& legacyKey : legacyStorageKeys(key)) {
        if (m_settings->contains(legacyKey)) {
            return true;
        }
    }
    return false;
}

void ConfigStore::sync() {
    QMutexLocker locker(&m_mutex);
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        qWarning() << "ConfigStore: failed to sync config"
                   << m_settings->fileName() << "| status:" << m_settings->status();
    }
}

QString ConfigStore::filePath() const {
    QMutexLocker locker(&m_mutex);
    return m_settings->fileName();
}

QStringList ConfigStore::allKeys() const {
    QMutexLocker locker(&m_mutex);

    QStringList keys = m_settings->allKeys();
    for (auto it = m_cache.constBegin(); it != m_cache.constEnd(); ++it) {
        if (!keys.contains(it.key())) {
            keys.append(it.key());
        }
    }
    return keys;
}
