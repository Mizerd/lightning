#pragma once

#include "storage/SecretStore.h"

#include <QSettings>
#include <memory>

class InsecureFallbackSecretStore final : public SecretStore
{
    Q_OBJECT
public:
    /// `substitutedForNative` = a native backend was compiled in and probed
    /// unavailable, so this store is standing in for one that may well hold
    /// the user's secrets. Never set it when no native backend exists: there
    /// this store IS the backend and its answers are authoritative.
    explicit InsecureFallbackSecretStore(QObject *parent = nullptr,
                                         bool substitutedForNative = false);

    bool isSecure() const override { return false; }
    bool isAvailable() const override { return true; }
    bool missesAreInconclusive() const override
    {
        return m_substitutedForNative;
    }

    bool lastReadFailed() const override
    {
        if (m_lastReadFailed)
            return true;
        if (!m_substitutedForNative)
            return false;
        if (m_lastReadFound)
            return false;
        return !m_lastReadUserHasRecord;
    }
    QString backendName() const override;

    bool storeSecret(const QString &userId,
                     const QString &key,
                     const QString &value) override;
    QString readSecret(const QString &userId,
                       const QString &key) const override;
    bool deleteSecret(const QString &userId, const QString &key) override;
    bool clearAccountSecrets(const QString &userId) override;

    QString lastError() const override { return m_lastError; }

private:
    static QString accountGroup(const QString &userId);
    QString settingsKey(const QString &userId, const QString &key) const;
    bool hasAnySecretFor(const QString &userId) const;

    std::unique_ptr<QSettings> m_store;
    mutable QString m_lastError;
    mutable bool m_lastReadFound = false;
    mutable bool m_lastReadUserHasRecord = false;
    mutable bool m_lastReadFailed = false;
    bool m_substitutedForNative = false;
};
