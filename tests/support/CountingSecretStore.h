// A SecretStore that counts per-account reads and writes, with injectable
// latency, failure and lastReadFailed(), for the live-accounts tests
// (contract §5.1). Thread-safe: KeyringWorker calls it on its own thread
// while the test thread inspects it. Holds fixture values only, never a real
// secret.
#pragma once

#include "storage/SecretStore.h"

#include <QHash>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QString>
#include <QThread>

namespace lightning::accounts::testing {

class CountingSecretStore final : public SecretStore
{
public:
    explicit CountingSecretStore(QObject *parent = nullptr) : SecretStore(parent) {}

    // ---- scripting -----------------------------------------------------------
    void setSecure(bool on) { QMutexLocker l(&m_mutex); m_secure = on; }
    void setShared(bool on) { QMutexLocker l(&m_mutex); m_shared = on; }
    void setReadLatencyMs(int ms) { QMutexLocker l(&m_mutex); m_readLatencyMs = ms; }
    void setWriteLatencyMs(int ms) { QMutexLocker l(&m_mutex); m_writeLatencyMs = ms; }
    // Reads answer "" and lastReadFailed() true: a locked collection.
    void setFailReads(bool fail) { QMutexLocker l(&m_mutex); m_failReads = fail; }
    void setFailWrites(bool fail) { QMutexLocker l(&m_mutex); m_failWrites = fail; }
    void setValue(const QString &userId, const QString &key, const QString &value)
    {
        QMutexLocker l(&m_mutex);
        m_values.insert(mapKey(userId, key), value);
    }
    QString value(const QString &userId, const QString &key) const
    {
        QMutexLocker l(&m_mutex);
        return m_values.value(mapKey(userId, key));
    }

    // ---- inspection -----------------------------------------------------------
    int reads(const QString &userId) const { QMutexLocker l(&m_mutex); return m_reads.value(userId); }
    int writes(const QString &userId) const { QMutexLocker l(&m_mutex); return m_writes.value(userId); }
    int deletes(const QString &userId) const { QMutexLocker l(&m_mutex); return m_deletes.value(userId); }
    int totalReads() const
    {
        QMutexLocker l(&m_mutex);
        int n = 0;
        for (auto it = m_reads.cbegin(); it != m_reads.cend(); ++it)
            n += it.value();
        return n;
    }
    QSet<QString> readUsers() const
    {
        QMutexLocker l(&m_mutex);
        QSet<QString> out;
        for (auto it = m_reads.cbegin(); it != m_reads.cend(); ++it)
            out.insert(it.key());
        return out;
    }
    QList<QThread *> callThreads() const { QMutexLocker l(&m_mutex); return m_threads; }
    void resetCounts()
    {
        QMutexLocker l(&m_mutex);
        m_reads.clear();
        m_writes.clear();
        m_deletes.clear();
        m_threads.clear();
    }

    // ---- SecretStore ------------------------------------------------------------
    bool isSecure() const override { QMutexLocker l(&m_mutex); return m_secure; }
    bool isAvailable() const override { return true; }
    QString backendName() const override { return QStringLiteral("counting (test)"); }
    bool isSharedBetweenInstalls() const override { QMutexLocker l(&m_mutex); return m_shared; }

    bool storeSecret(const QString &userId, const QString &key, const QString &value) override
    {
        sleepFor(&CountingSecretStore::m_writeLatencyMs);
        QMutexLocker l(&m_mutex);
        ++m_writes[userId];
        m_threads.append(QThread::currentThread());
        if (m_failWrites) {
            m_error = QStringLiteral("simulated write failure");
            return false;
        }
        m_values.insert(mapKey(userId, key), value);
        return true;
    }
    QString readSecret(const QString &userId, const QString &key) const override
    {
        sleepFor(&CountingSecretStore::m_readLatencyMs);
        QMutexLocker l(&m_mutex);
        ++m_reads[userId];
        m_threads.append(QThread::currentThread());
        m_lastReadFailed = m_failReads;
        if (m_failReads)
            return {};
        return m_values.value(mapKey(userId, key));
    }
    bool deleteSecret(const QString &userId, const QString &key) override
    {
        QMutexLocker l(&m_mutex);
        ++m_deletes[userId];
        return m_values.remove(mapKey(userId, key)) > 0;
    }
    bool clearAccountSecrets(const QString &userId) override
    {
        QMutexLocker l(&m_mutex);
        ++m_deletes[userId];
        const QString prefix = userId + QLatin1Char('\x1f');
        for (auto it = m_values.begin(); it != m_values.end();) {
            if (it.key().startsWith(prefix))
                it = m_values.erase(it);
            else
                ++it;
        }
        return true;
    }
    QString lastError() const override { QMutexLocker l(&m_mutex); return m_error; }
    bool lastReadFailed() const override { QMutexLocker l(&m_mutex); return m_lastReadFailed; }

private:
    static QString mapKey(const QString &userId, const QString &key)
    {
        return userId + QLatin1Char('\x1f') + key;
    }
    void sleepFor(int CountingSecretStore::*field) const
    {
        int ms = 0;
        {
            QMutexLocker l(&m_mutex);
            ms = this->*field;
        }
        if (ms > 0)
            QThread::msleep(ulong(ms));
    }

    mutable QMutex m_mutex;
    QHash<QString, QString> m_values;
    mutable QHash<QString, int> m_reads;
    QHash<QString, int> m_writes;
    QHash<QString, int> m_deletes;
    mutable QList<QThread *> m_threads;
    QString m_error;
    int m_readLatencyMs = 0;
    int m_writeLatencyMs = 0;
    mutable bool m_lastReadFailed = false;
    bool m_failReads = false;
    bool m_failWrites = false;
    bool m_secure = true;
    bool m_shared = false;
};

} // namespace lightning::accounts::testing
