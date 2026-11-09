#pragma once

#include <QObject>
#include <QString>
#include <functional>
#include <memory>

class SecretStore : public QObject
{
    Q_OBJECT
public:
    explicit SecretStore(QObject *parent = nullptr) : QObject(parent) {}
    ~SecretStore() override = default;

    static std::unique_ptr<SecretStore> createDefault(QObject *parent = nullptr);

    virtual bool isSecure() const = 0;

    virtual bool isAvailable() const = 0;

    virtual QString backendName() const = 0;

    virtual bool storeSecret(const QString &userId,
                             const QString &key,
                             const QString &value) = 0;
    virtual QString readSecret(const QString &userId,
                               const QString &key) const = 0;
    virtual bool deleteSecret(const QString &userId, const QString &key) = 0;

    virtual bool clearAccountSecrets(const QString &userId) = 0;

    virtual QString lastError() const = 0;

    virtual bool lastReadFailed() const { return false; }

    // A store that may prompt to unlock its keyring may do so once more.
    virtual void allowUnlockPrompt() {}

    /// Whether a miss is inconclusive by construction: true for the whole life
    /// of a store standing in for a native backend it could not open.
    /// Destructive decisions key on this structural fact, not on
    /// lastReadFailed(), which can report individual reads as trustworthy.
    virtual bool missesAreInconclusive() const { return false; }

    virtual bool isSharedBetweenInstalls() const { return false; }

    virtual QString backendKind() const { return {}; }

    virtual QString backendKindFor(const QString &userId) const
    {
        Q_UNUSED(userId);
        return backendKind();
    }

    using RecordedBackendLookup = std::function<QString(const QString &userId)>;
    void setRecordedBackendLookup(RecordedBackendLookup lookup)
    {
        m_recordedBackend = std::move(lookup);
    }
    QString recordedBackendKind(const QString &userId) const
    {
        return m_recordedBackend ? m_recordedBackend(userId) : QString();
    }
    void setInstallScope(const QString &scope) { m_installScope = scope; }
    QString installScope() const { return m_installScope; }

    virtual QString readLegacySecret(const QString &userId,
                                     const QString &key) const
    {
        Q_UNUSED(userId);
        Q_UNUSED(key);
        return {};
    }
    virtual bool deleteLegacySecret(const QString &userId, const QString &key)
    {
        Q_UNUSED(userId);
        Q_UNUSED(key);
        return true;
    }

private:
    QString m_installScope;
    RecordedBackendLookup m_recordedBackend;
};
