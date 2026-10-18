#pragma once

#include "storage/SecretStore.h"

// A store whose items are in one of two backends, the choice made per process
// by a library it does not control: libsecret keeps a Flatpak's or snap's
// items in a sandbox-private FILE keyring whenever the Secret portal answers
// its check at startup, and in the Secret Service when it does not (a portal
// still starting at login, a desktop whose portal gained or lost a Secret
// backend; docs/open-items.md). SettingsManager records, per account, the
// backend its sign-in was last written in (recordedBackendKind()); this reads
// and writes each account there, so a portal that came or went never turns a
// saved sign-in into "no such secret":
//   - recorded "service", this process "file": the Secret Service is asked
//     directly, as the process that wrote the items did.
//   - recorded "file", this process "service": the file keyring's key comes
//     only from the portal, which did not answer, so every read fails
//     ("cannot tell", CLAUDE.md §6). A write goes to the Secret Service and
//     the record follows it.
// Subclasses provide the two backends; the routing is here, shared with the
// fake keyring the tests use.
// No Q_OBJECT: it adds no meta-object of its own.
class TwoBackendSecretStore : public SecretStore
{
public:
    using SecretStore::SecretStore;

    static constexpr auto kFileKind = "file";
    static constexpr auto kServiceKind = "service";

    enum class Route {
        Default,     // nothing recorded, or recorded where this process is
        Service,     // the Secret Service, past this process's file keyring
        Unreachable, // the file keyring, from a process on the Secret Service
    };
    static Route routeFor(const QString &recorded, const QString &current)
    {
        if (recorded == QLatin1String(kServiceKind)
            && current == QLatin1String(kFileKind))
            return Route::Service;
        if (recorded == QLatin1String(kFileKind)
            && current == QLatin1String(kServiceKind))
            return Route::Unreachable;
        return Route::Default;
    }
    Route routeFor(const QString &userId) const
    {
        return routeFor(recordedBackendKind(userId), backendKind());
    }

    QString backendKindFor(const QString &userId) const override
    {
        return routeFor(userId) == Route::Service
                   ? QString::fromLatin1(kServiceKind)
                   : backendKind();
    }

    bool storeSecret(const QString &userId, const QString &key,
                     const QString &value) override
    {
        return storeIn(routeFor(userId) == Route::Service ? Backend::Service
                                                          : Backend::Default,
                       userId, key, value);
    }

    QString readSecret(const QString &userId, const QString &key) const override
    {
        switch (routeFor(userId)) {
        case Route::Service:
            return readFrom(Backend::Service, userId, key);
        case Route::Unreachable:
            // Not "no such secret": the keyring that holds it did not open.
            setError(QStringLiteral(
                "this sign-in is in the sandbox's file keyring, which did not "
                "open in this launch (the Secret portal did not answer)"));
            m_lastReadFailed = true;
            return {};
        case Route::Default:
            break;
        }
        return readFrom(Backend::Default, userId, key);
    }

    bool deleteSecret(const QString &userId, const QString &key) override
    {
        return clearIn(userId, &key);
    }

    bool clearAccountSecrets(const QString &userId) override
    {
        return clearIn(userId, nullptr);
    }

    QString lastError() const override { return m_lastError; }
    bool lastReadFailed() const override { return m_lastReadFailed; }

protected:
    enum class Backend {
        Default, // this process's own choice
        Service, // the Secret Service, whatever this process chose
    };
    // The backends. A read sets m_lastReadFailed: true when the backend could
    // not answer, false when it answered, found or not. A null key clears
    // every item of the account.
    virtual QString readFrom(Backend backend, const QString &userId,
                             const QString &key) const = 0;
    virtual bool storeIn(Backend backend, const QString &userId,
                         const QString &key, const QString &value) = 0;
    virtual bool clearFrom(Backend backend, const QString &userId,
                           const QString *key) = 0;

    void setError(const QString &error) const { m_lastError = error; }

    mutable bool m_lastReadFailed = false;
    mutable QString m_lastError;

private:
    bool clearIn(const QString &userId, const QString *key)
    {
        switch (routeFor(userId)) {
        case Route::Service: {
            // Both: whatever either holds of this account goes. The Secret
            // Service, where the record says the sign-in is, decides first.
            const bool service = clearFrom(Backend::Service, userId, key);
            const QString serviceError = m_lastError;
            const bool own = clearFrom(Backend::Default, userId, key);
            if (!service)
                setError(serviceError);
            return service && own;
        }
        case Route::Unreachable:
            // What the Secret Service holds goes; the file keyring, where the
            // record says the sign-in is, cannot be reached, and a removal
            // that left it there is not reported as done.
            clearFrom(Backend::Default, userId, key);
            setError(QStringLiteral(
                "this sign-in is in the sandbox's file keyring, which did not "
                "open in this launch; nothing was removed from it"));
            return false;
        case Route::Default:
            break;
        }
        return clearFrom(Backend::Default, userId, key);
    }
};
