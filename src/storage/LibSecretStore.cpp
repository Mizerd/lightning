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

// Every item this build writes. libsecret adds the name as `xdg:schema` to
// each item and lookup, so older builds, which search the legacy name, never
// see these items and their sign-out never deletes them.
const SecretSchema *scopedSchema()
{
    static const SecretSchema schema = {
        "org.lightning_matrix.Lightning.Secret", SECRET_SCHEMA_NONE,
        {
            { "user_id", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { "key",     SECRET_SCHEMA_ATTRIBUTE_STRING },
            { "install", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { nullptr,   SECRET_SCHEMA_ATTRIBUTE_STRING },
        },
        // reserved
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    };
    return &schema;
}

// Builds before install scoping. Read only: any install may still use it.
const SecretSchema *legacySchema()
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
    return QStringLiteral("Lightning: %1 (%2)").arg(userId, key);
}

QByteArray toUtf8(const QString &s) { return s.toUtf8(); }

constexpr auto kNoScope = "no install scope set";

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

    // Reaching the service is not permission to use it: under Ubuntu Touch's
    // AppArmor confinement gnome-keyring answers on the bus but denies
    // Secret.Service.OpenSession. Probe with a real (harmless) lookup, which
    // uses the same session path as every read and write; a miss is success,
    // only an error means unusable.
    GError *probeErr = nullptr;
    gchar *probeValue = secret_password_lookup_sync(
        scopedSchema(), nullptr, &probeErr,
        "user_id", "__lightning_probe__",
        "key",     "__probe__",
        "install", "__probe__",
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
    // An unscoped write would be an item no install can find again.
    if (installScope().isEmpty()) {
        setError(QLatin1String(kNoScope));
        return false;
    }
    GError *err = nullptr;
    const QByteArray u = toUtf8(userId);
    const QByteArray k = toUtf8(key);
    const QByteArray i = toUtf8(installScope());
    const QByteArray v = toUtf8(value);
    const QByteArray label = toUtf8(makeLabel(userId, key));

    gboolean ok = secret_password_store_sync(
        scopedSchema(),
        SECRET_COLLECTION_DEFAULT,
        label.constData(),
        v.constData(),
        nullptr,
        &err,
        "user_id", u.constData(),
        "key",     k.constData(),
        "install", i.constData(),
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
    // Not "no such secret": this install's items exist, it just cannot name
    // them yet (CLAUDE.md §6).
    if (installScope().isEmpty()) {
        setError(QLatin1String(kNoScope));
        m_lastReadFailed = true;
        return {};
    }
    GError *err = nullptr;
    const QByteArray u = toUtf8(userId);
    const QByteArray k = toUtf8(key);
    const QByteArray i = toUtf8(installScope());

    gchar *raw = secret_password_lookup_sync(
        scopedSchema(), nullptr, &err,
        "user_id", u.constData(),
        "key",     k.constData(),
        "install", i.constData(),
        nullptr);

    if (err) {
        setError(QString::fromUtf8(err->message));
        g_error_free(err);
        // The backend could not answer (locked collection, dropped session
        // bus): not "no such secret".
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

QString LibSecretStore::readLegacySecret(const QString &userId,
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
        legacySchema(), nullptr, &err,
        "user_id", u.constData(),
        "key",     k.constData(),
        nullptr);

    if (err) {
        setError(QString::fromUtf8(err->message));
        g_error_free(err);
        m_lastReadFailed = true;
        qCWarning(lcLibSecret) << "readLegacySecret failed:" << m_lastError;
        return {};
    }
    m_lastReadFailed = false;
    if (!raw)
        return {};
    QString out = QString::fromUtf8(raw);
    secret_password_free(raw);
    return out;
}

bool LibSecretStore::deleteLegacySecret(const QString &userId,
                                        const QString &key)
{
    if (!m_available) {
        setError(QStringLiteral("libsecret unavailable"));
        return false;
    }
    GError *err = nullptr;
    const QByteArray u = toUtf8(userId);
    const QByteArray k = toUtf8(key);
    secret_password_clear_sync(
        legacySchema(), nullptr, &err,
        "user_id", u.constData(),
        "key",     k.constData(),
        nullptr);
    if (err) {
        setError(QString::fromUtf8(err->message));
        g_error_free(err);
        return false;
    }
    return true;
}

bool LibSecretStore::deleteSecret(const QString &userId, const QString &key)
{
    if (!m_available) {
        setError(QStringLiteral("libsecret unavailable"));
        return false;
    }
    // Without the scope this clear would match every install's items.
    if (installScope().isEmpty()) {
        setError(QLatin1String(kNoScope));
        return false;
    }
    GError *err = nullptr;
    const QByteArray u = toUtf8(userId);
    const QByteArray k = toUtf8(key);
    const QByteArray i = toUtf8(installScope());
    gboolean removed = secret_password_clear_sync(
        scopedSchema(), nullptr, &err,
        "user_id", u.constData(),
        "key",     k.constData(),
        "install", i.constData(),
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
    if (installScope().isEmpty()) {
        setError(QLatin1String(kNoScope));
        return false;
    }
    GError *err = nullptr;
    const QByteArray u = toUtf8(userId);
    const QByteArray i = toUtf8(installScope());
    // Without `key`: every entry of this account in this install. Legacy items
    // and other installs' items do not match (different schema name, or a
    // different install attribute).
    gboolean removed = secret_password_clear_sync(
        scopedSchema(), nullptr, &err,
        "user_id", u.constData(),
        "install", i.constData(),
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
QString LibSecretStore::readLegacySecret(const QString &, const QString &) const { return {}; }
bool LibSecretStore::deleteLegacySecret(const QString &, const QString &) { return false; }
bool LibSecretStore::deleteSecret(const QString &, const QString &) { return false; }
bool LibSecretStore::clearAccountSecrets(const QString &) { return false; }

#endif // HAVE_LIBSECRET
