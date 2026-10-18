#include "storage/LibSecretStore.h"

#include <QLoggingCategory>

#ifdef HAVE_LIBSECRET
#include <dlfcn.h>
#endif

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

// The attributes the password API builds from its arguments, for the Secret
// Service API, which takes them as a table. A null key matches every key of
// the account.
GHashTable *scopedAttributes(const QByteArray &userId, const QByteArray *key,
                             const QByteArray &install)
{
    GHashTable *attributes =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(attributes, g_strdup("user_id"),
                        g_strdup(userId.constData()));
    if (key)
        g_hash_table_insert(attributes, g_strdup("key"),
                            g_strdup(key->constData()));
    g_hash_table_insert(attributes, g_strdup("install"),
                        g_strdup(install.constData()));
    return attributes;
}

} // namespace

LibSecretStore::LibSecretStore(QObject *parent)
    : TwoBackendSecretStore(parent)
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
    detectBackendKind();
    qCInfo(lcLibSecret) << "libsecret backend ready"
                        << "items_in=" << (m_backendKind.isEmpty()
                                               ? QStringLiteral("unknown")
                                               : m_backendKind);
}

namespace {
struct BackendAnswer {
    GMainLoop *loop = nullptr;
    QByteArray typeName;
    bool done = false;
};
} // namespace

void LibSecretStore::detectBackendKind()
{
    // The password API's own choice, asked the way it asks it: libsecret picks
    // the file backend inside a Flatpak or snap whose Secret portal answers,
    // and the Secret Service otherwise, once per process. On a private
    // context, so nothing else is dispatched while waiting, and bounded: a
    // portal that never answers leaves the kind unknown, never the start hung.
    // On a timeout the answer is abandoned (its context is never iterated
    // again, so its callback never runs) and the small struct is leaked.
    //
    // secret_backend_get() is exported since libsecret 0.20 but declared only
    // in a header libsecret keeps out of <libsecret/secret.h> as "subject to
    // change", so it is looked up at run time: an older or changed libsecret
    // leaves the kind unknown instead of failing to link or start.
    using BackendGet = void (*)(int, GCancellable *, GAsyncReadyCallback, gpointer);
    using BackendGetFinish = GObject *(*)(GAsyncResult *, GError **);
    static const auto backendGet =
        reinterpret_cast<BackendGet>(dlsym(RTLD_DEFAULT, "secret_backend_get"));
    static const auto backendGetFinish = reinterpret_cast<BackendGetFinish>(
        dlsym(RTLD_DEFAULT, "secret_backend_get_finish"));
    if (!backendGet || !backendGetFinish)
        return;
    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);
    auto *answer = new BackendAnswer;
    answer->loop = g_main_loop_new(context, FALSE);
    backendGet(
        0 /* SECRET_BACKEND_NONE */, nullptr,
        [](GObject *, GAsyncResult *result, gpointer data) {
            auto *answer = static_cast<BackendAnswer *>(data);
            GObject *backend = backendGetFinish(result, nullptr);
            if (backend) {
                answer->typeName = G_OBJECT_TYPE_NAME(backend);
                g_object_unref(backend);
            }
            answer->done = true;
            g_main_loop_quit(answer->loop);
        },
        answer);
    GSource *timeout = g_timeout_source_new(3000);
    g_source_set_callback(
        timeout,
        [](gpointer loop) -> gboolean {
            g_main_loop_quit(static_cast<GMainLoop *>(loop));
            return G_SOURCE_REMOVE;
        },
        answer->loop, nullptr);
    g_source_attach(timeout, context);
    // A backend libsecret already holds may answer before the loop runs.
    if (!answer->done)
        g_main_loop_run(answer->loop);
    g_source_destroy(timeout);
    g_source_unref(timeout);
    g_main_context_pop_thread_default(context);
    const bool done = answer->done;
    const QByteArray typeName = answer->typeName;
    if (done) {
        g_main_loop_unref(answer->loop);
        delete answer;
    }
    g_main_context_unref(context);
    if (typeName == "SecretFileBackend")
        m_backendKind = QStringLiteral("file");
    else if (!typeName.isEmpty())
        m_backendKind = QStringLiteral("service");
}

bool LibSecretStore::isAvailable() const
{
    return m_available;
}

QString LibSecretStore::backendName() const
{
    return QStringLiteral("libsecret (Secret Service)");
}

bool LibSecretStore::storeIn(Backend backend, const QString &userId,
                             const QString &key, const QString &value)
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

    gboolean ok = FALSE;
    if (backend == Backend::Service) {
        GHashTable *attributes = scopedAttributes(u, &k, i);
        // As the password API stores it.
        SecretValue *secret = secret_value_new(v.constData(), -1, "text/plain");
        ok = secret_service_store_sync(nullptr, scopedSchema(), attributes,
                                       SECRET_COLLECTION_DEFAULT,
                                       label.constData(), secret, nullptr,
                                       &err);
        secret_value_unref(secret);
        g_hash_table_unref(attributes);
    } else {
        ok = secret_password_store_sync(
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
    }

    if (!ok || err) {
        setError(err ? QString::fromUtf8(err->message)
                     : QStringLiteral("secret_password_store_sync failed"));
        if (err) g_error_free(err);
        qCWarning(lcLibSecret) << "storeSecret failed:" << m_lastError;
        return false;
    }
    return true;
}

QString LibSecretStore::readFrom(Backend backend, const QString &userId,
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

    QString out;
    if (backend == Backend::Service) {
        GHashTable *attributes = scopedAttributes(u, &k, i);
        SecretValue *secret = secret_service_lookup_sync(
            nullptr, scopedSchema(), attributes, nullptr, &err);
        g_hash_table_unref(attributes);
        if (secret) {
            // Text only, as the password API reads it.
            if (const gchar *text = secret_value_get_text(secret))
                out = QString::fromUtf8(text);
            secret_value_unref(secret);
        }
    } else {
        gchar *raw = secret_password_lookup_sync(
            scopedSchema(), nullptr, &err,
            "user_id", u.constData(),
            "key",     k.constData(),
            "install", i.constData(),
            nullptr);
        if (raw) {
            out = QString::fromUtf8(raw);
            secret_password_free(raw);
        }
    }

    if (err) {
        setError(QString::fromUtf8(err->message));
        g_error_free(err);
        // The backend could not answer (locked collection, dropped session
        // bus, a Secret Service not running): not "no such secret".
        m_lastReadFailed = true;
        qCWarning(lcLibSecret) << "readSecret failed:" << m_lastError;
        return {};
    }
    // The lookup completed. Absent is a real answer, not a failure.
    m_lastReadFailed = false;
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

bool LibSecretStore::clearFrom(Backend backend, const QString &userId,
                               const QString *key)
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
    const QByteArray k = key ? toUtf8(*key) : QByteArray();
    const QByteArray i = toUtf8(installScope());
    // Without `key`: every entry of this account in this install. Legacy items
    // and other installs' items do not match (different schema name, or a
    // different install attribute).
    if (backend == Backend::Service) {
        GHashTable *attributes = scopedAttributes(u, key ? &k : nullptr, i);
        secret_service_clear_sync(nullptr, scopedSchema(), attributes, nullptr,
                                  &err);
        g_hash_table_unref(attributes);
    } else if (key) {
        secret_password_clear_sync(
            scopedSchema(), nullptr, &err,
            "user_id", u.constData(),
            "key",     k.constData(),
            "install", i.constData(),
            nullptr);
    } else {
        secret_password_clear_sync(
            scopedSchema(), nullptr, &err,
            "user_id", u.constData(),
            "install", i.constData(),
            nullptr);
    }
    if (err) {
        setError(QString::fromUtf8(err->message));
        g_error_free(err);
        return false;
    }
    // A clear that matched nothing returns FALSE without an error: there was
    // nothing to remove, which is success.
    return true;
}

#else // HAVE_LIBSECRET

LibSecretStore::LibSecretStore(QObject *parent) : TwoBackendSecretStore(parent) {}
LibSecretStore::~LibSecretStore() = default;

bool LibSecretStore::isAvailable() const { return false; }
QString LibSecretStore::backendName() const
{
    return QStringLiteral("libsecret (not compiled in)");
}
bool LibSecretStore::storeIn(Backend, const QString &, const QString &, const QString &) { return false; }
QString LibSecretStore::readFrom(Backend, const QString &, const QString &) const { return {}; }
QString LibSecretStore::readLegacySecret(const QString &, const QString &) const { return {}; }
bool LibSecretStore::deleteLegacySecret(const QString &, const QString &) { return false; }
bool LibSecretStore::clearFrom(Backend, const QString &, const QString *) { return false; }

#endif // HAVE_LIBSECRET
