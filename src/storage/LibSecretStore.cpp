#include "storage/LibSecretStore.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcLibSecret, "matrix.secret.libsecret")

#ifdef HAVE_LIBSECRET

// glib's gdbusintrospection.h has a public member named `signals`, which
// clashes with Qt's `#define signals public`. Undef around the libsecret
// include and restore afterwards. Standard Qt+glib interop workaround.
#pragma push_macro("signals")
#undef signals
#include <libsecret/secret.h>
#pragma pop_macro("signals")

namespace {

// Application-scoped schema. Attributes are used both as identity for
// libsecret and as a search filter for clearAccountSecrets().
const SecretSchema *matrixClientSchema()
{
    static const SecretSchema schema = {
        "net.smetonis.matrixclient.Secret", SECRET_SCHEMA_NONE,
        {
            { "user_id", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { "key",     SECRET_SCHEMA_ATTRIBUTE_STRING },
            { nullptr,   SECRET_SCHEMA_ATTRIBUTE_STRING },
        },
        // reserved
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    };
    return &schema;
}

QString makeLabel(const QString &userId, const QString &key)
{
    return QStringLiteral("matrix-client: %1 (%2)").arg(userId, key);
}

QByteArray toUtf8(const QString &s) { return s.toUtf8(); }

} // namespace

LibSecretStore::LibSecretStore(QObject *parent)
    : SecretStore(parent)
{
    probe();
}

LibSecretStore::~LibSecretStore() = default;

void LibSecretStore::probe()
{
    // Try to reach the Secret Service. On headless / CI systems this fails
    // and we mark ourselves unavailable so the factory can fall back.
    GError *err = nullptr;
    SecretService *svc = secret_service_get_sync(SECRET_SERVICE_NONE,
                                                 nullptr, &err);
    if (!svc || err) {
        m_available = false;
        setError(err ? QString::fromUtf8(err->message)
                     : QStringLiteral("secret_service_get_sync returned null"));
        qCInfo(lcLibSecret) << "libsecret unavailable:" << m_lastError;
        if (err) g_error_free(err);
        return;
    }
    g_object_unref(svc);

    // Reaching the service is NOT permission to use it. Under Ubuntu Touch's
    // AppArmor confinement gnome-keyring answers on the bus — so the call
    // above succeeds — while every real operation is denied at
    // Secret.Service.OpenSession, and there is no policy group a click can
    // request to change that. Probing with the bus proxy alone therefore
    // reported "backend ready" and the factory never fell back, so the token
    // was written nowhere and the next launch could not restore the session.
    //
    // A real (harmless) lookup is the honest probe: it exercises the same
    // session-opening path every read and write needs. A miss is success —
    // the key is not expected to exist; only an ERROR means unusable.
    GError *probeErr = nullptr;
    gchar *probeValue = secret_password_lookup_sync(
        matrixClientSchema(), nullptr, &probeErr,
        "user_id", "__lightning_probe__",
        "key",     "__probe__",
        nullptr);
    if (probeValue)
        secret_password_free(probeValue);
    if (probeErr) {
        m_available = false;
        setError(QString::fromUtf8(probeErr->message));
        qCInfo(lcLibSecret) << "libsecret reachable but unusable:"
                            << m_lastError;
        g_error_free(probeErr);
        return;
    }

    m_available = true;
    qCInfo(lcLibSecret) << "libsecret backend ready";
}

bool LibSecretStore::isAvailable() const
{
    return m_available;
}

QString LibSecretStore::backendName() const
{
    return QStringLiteral("libsecret (Secret Service)");
}

bool LibSecretStore::storeSecret(const QString &userId,
                                  const QString &key,
                                  const QString &value)
{
    if (!m_available) {
        setError(QStringLiteral("libsecret unavailable"));
        return false;
    }
    GError *err = nullptr;
    const QByteArray u = toUtf8(userId);
    const QByteArray k = toUtf8(key);
    const QByteArray v = toUtf8(value);
    const QByteArray label = toUtf8(makeLabel(userId, key));

    gboolean ok = secret_password_store_sync(
        matrixClientSchema(),
        SECRET_COLLECTION_DEFAULT,
        label.constData(),
        v.constData(),
        nullptr,
        &err,
        "user_id", u.constData(),
        "key",     k.constData(),
        nullptr);

    if (!ok || err) {
        setError(err ? QString::fromUtf8(err->message)
                     : QStringLiteral("secret_password_store_sync failed"));
        if (err) g_error_free(err);
        qCWarning(lcLibSecret) << "storeSecret failed:" << m_lastError;
        return false;
    }
    return true;
}

QString LibSecretStore::readSecret(const QString &userId,
                                    const QString &key) const
{
    if (!m_available) {
        setError(QStringLiteral("libsecret unavailable"));
        m_lastReadFailed = true;
        return {};
    }
    GError *err = nullptr;
    const QByteArray u = toUtf8(userId);
    const QByteArray k = toUtf8(key);

    gchar *raw = secret_password_lookup_sync(
        matrixClientSchema(), nullptr, &err,
        "user_id", u.constData(),
        "key",     k.constData(),
        nullptr);

    if (err) {
        setError(QString::fromUtf8(err->message));
        g_error_free(err);
        // The backend could not answer — a locked collection, a dropped
        // session bus. NOT "no such secret". Callers must be able to tell
        // these apart before treating an empty result as evidence that an
        // account has no saved sign-in.
        m_lastReadFailed = true;
        qCWarning(lcLibSecret) << "readSecret failed:" << m_lastError;
        return {};
    }
    // The lookup completed. Absent is a real answer, not a failure.
    m_lastReadFailed = false;
    if (!raw)
        return {};
    QString out = QString::fromUtf8(raw);
    secret_password_free(raw);
    return out;
}

bool LibSecretStore::deleteSecret(const QString &userId, const QString &key)
{
    if (!m_available) {
        setError(QStringLiteral("libsecret unavailable"));
        return false;
    }
    GError *err = nullptr;
    const QByteArray u = toUtf8(userId);
    const QByteArray k = toUtf8(key);
    gboolean removed = secret_password_clear_sync(
        matrixClientSchema(), nullptr, &err,
        "user_id", u.constData(),
        "key",     k.constData(),
        nullptr);
    if (err) {
        setError(QString::fromUtf8(err->message));
        g_error_free(err);
        return false;
    }
    // 'removed' is false only when there was nothing to remove — treat as success.
    Q_UNUSED(removed);
    return true;
}

bool LibSecretStore::clearAccountSecrets(const QString &userId)
{
    if (!m_available) {
        setError(QStringLiteral("libsecret unavailable"));
        return false;
    }
    GError *err = nullptr;
    const QByteArray u = toUtf8(userId);
    // Passing only user_id (without key) matches every entry whose user_id
    // attribute equals the given value.
    gboolean removed = secret_password_clear_sync(
        matrixClientSchema(), nullptr, &err,
        "user_id", u.constData(),
        nullptr);
    if (err) {
        setError(QString::fromUtf8(err->message));
        g_error_free(err);
        return false;
    }
    Q_UNUSED(removed);
    return true;
}

#else // HAVE_LIBSECRET

LibSecretStore::LibSecretStore(QObject *parent) : SecretStore(parent) {}
LibSecretStore::~LibSecretStore() = default;

bool LibSecretStore::isAvailable() const { return false; }
QString LibSecretStore::backendName() const
{
    return QStringLiteral("libsecret (not compiled in)");
}
bool LibSecretStore::storeSecret(const QString &, const QString &, const QString &) { return false; }
QString LibSecretStore::readSecret(const QString &, const QString &) const { return {}; }
bool LibSecretStore::deleteSecret(const QString &, const QString &) { return false; }
bool LibSecretStore::clearAccountSecrets(const QString &) { return false; }

#endif // HAVE_LIBSECRET
