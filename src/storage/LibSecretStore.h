#pragma once

#include "storage/TwoBackendSecretStore.h"

#include <atomic>

typedef struct _GError GError;

class LibSecretStore final : public TwoBackendSecretStore
{
    Q_OBJECT
public:
    explicit LibSecretStore(QObject *parent = nullptr);
    ~LibSecretStore() override;

    bool isSecure() const override { return true; }
    bool isAvailable() const override;
    QString backendName() const override;
    bool isSharedBetweenInstalls() const override { return true; }

    QString readLegacySecret(const QString &userId,
                             const QString &key) const override;
    bool deleteLegacySecret(const QString &userId,
                            const QString &key) override;

    QString backendKind() const override { return m_backendKind; }
    void allowUnlockPrompt() override { m_unlockPromptOffered = false; }

protected:
    // Backend::Default is libsecret's password API, which goes wherever
    // libsecret chose for this process; Backend::Service is the Secret Service
    // API, which reaches the Secret Service whatever that choice was, with the
    // same schema, attributes and collection.
    QString readFrom(Backend backend, const QString &userId,
                     const QString &key) const override;
    bool storeIn(Backend backend, const QString &userId, const QString &key,
                 const QString &value) override;
    bool clearFrom(Backend backend, const QString &userId,
                   const QString *key) override;

private:
    void probe();
    void detectBackendKind();
    QString lookup(Backend backend, const QByteArray &userId,
                   const QByteArray &key, const QByteArray &install,
                   GError **err) const;

    bool m_available = false;
    QString m_backendKind;
    // At most one unlock prompt until allowUnlockPrompt(): a dismissed one
    // must not come back on every read.
    mutable std::atomic<bool> m_unlockPromptOffered{false};
};
