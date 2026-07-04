#pragma once

#include "storage/SecretStore.h"

#include <QSettings>
#include <memory>

// QSettings-backed fallback used only when no native secure store is
// reachable. Stores secrets in plaintext under a dedicated group so it is
// trivial to audit ("secrets/*"). Always reports insecure so the UI can
// warn the user.
class InsecureFallbackSecretStore final : public SecretStore
{
    Q_OBJECT
public:
    explicit InsecureFallbackSecretStore(QObject *parent = nullptr);

    bool isSecure() const override { return false; }
    bool isAvailable() const override { return true; }
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
    QString settingsKey(const QString &userId, const QString &key) const;

    std::unique_ptr<QSettings> m_store;
    mutable QString m_lastError;
};
