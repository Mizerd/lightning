#include "storage/LibSecretStore.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QThread>

#include <atomic>

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
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    };
    return &schema;
}

const SecretSchema *legacySchema()
{
    static const SecretSchema schema = {
        "net.smetonis.matrixclient.Secret", SECRET_SCHEMA_NONE,
        {
            { "user_id", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { "key",     SECRET_SCHEMA_ATTRIBUTE_STRING },
            { nullptr,   SECRET_SCHEMA_ATTRIBUTE_STRING },
        },
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

enum class KeyringLock { Open, Unlocked, Locked };

// Process-wide: every LibSecretStore shares the one prompt.
std::atomic<bool> g_unlockPromptOffered{false};

bool onGuiThread()
{
    const QCoreApplication *app = QCoreApplication::instance();
    return app && QThread::currentThread() == app->thread();
}

// gnome-keyring keeps a locked item's attributes only hashed and without its
// schema name, so a lookup by schema misses it and libsecret never unlocks.
KeyringLock defaultCollectionLock()
{
    SecretService *svc =
        secret_service_get_sync(SECRET_SERVICE_NONE, nullptr, nullptr);
    if (!svc)
        return KeyringLock::Open;
    SecretCollection *collection = secret_collection_for_alias_sync(
        svc, SECRET_COLLECTION_DEFAULT, SECRET_COLLECTION_NONE, nullptr, nullptr);
    KeyringLock state = collection && secret_collection_get_locked(collection)
                            ? KeyringLock::Locked
                            : KeyringLock::Open;
    // Only the GUI thread prompts: a worker's read is bounded by a timeout.
    if (state == KeyringLock::Locked && onGuiThread()
        && !g_unlockPromptOffered.exchange(true)) {
        GList *objects = g_list_append(nullptr, collection);
        if (secret_service_unlock_sync(svc, objects, nullptr, nullptr, nullptr) > 0)
            state = KeyringLock::Unlocked;
        g_list_free(objects);
    }
    if (collection)
        g_object_unref(collection);
    g_object_unref(svc);
    return state;
}

}

LibSecretStore::LibSecretStore(QObject *parent)
    : TwoBackendSecretStore(parent)
{
    probe();
}

LibSecretStore::~LibSecretStore() = default;

void LibSecretStore::probe()
{
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
}

void LibSecretStore::detectBackendKind()
{
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
        0, nullptr,
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

    // libsecret would raise its own prompt for every item: the shared one stands in.
    if (onGuiThread() && throughSecretService(backend)
        && defaultCollectionLock() == KeyringLock::Locked) {
        setError(QStringLiteral("the keyring is locked"));
        qCWarning(lcLibSecret) << "storeSecret failed:" << m_lastError;
        return false;
    }

    gboolean ok = FALSE;
    if (backend == Backend::Service) {
        GHashTable *attributes = scopedAttributes(u, &k, i);
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

bool LibSecretStore::throughSecretService(Backend backend) const
{
    return backend == Backend::Service
           || m_backendKind == QLatin1String(kServiceKind);
}

void LibSecretStore::allowUnlockPrompt()
{
    g_unlockPromptOffered = false;
}

bool LibSecretStore::unlockedForWriting()
{
    return !m_available || !throughSecretService(Backend::Default)
           || defaultCollectionLock() != KeyringLock::Locked;
}

QString LibSecretStore::readFrom(Backend backend, const QString &userId,
                                 const QString &key) const
{
    if (!m_available) {
        setError(QStringLiteral("libsecret unavailable"));
        m_lastReadFailed = true;
        return {};
    }
    if (installScope().isEmpty()) {
        setError(QLatin1String(kNoScope));
        m_lastReadFailed = true;
        return {};
    }
    GError *err = nullptr;
    const QByteArray u = toUtf8(userId);
    const QByteArray k = toUtf8(key);
    const QByteArray i = toUtf8(installScope());

    QString out = lookup(backend, u, k, i, &err);
    if (!err && out.isEmpty() && throughSecretService(backend)) {
        if (defaultCollectionLock() == KeyringLock::Locked) {
            setError(QStringLiteral("the keyring is locked"));
            m_lastReadFailed = true;
            qCWarning(lcLibSecret) << "readSecret failed:" << m_lastError;
            return {};
        }
        out = lookup(backend, u, k, i, &err);
    }

    if (err) {
        setError(QString::fromUtf8(err->message));
        g_error_free(err);
        m_lastReadFailed = true;
        qCWarning(lcLibSecret) << "readSecret failed:" << m_lastError;
        return {};
    }
    m_lastReadFailed = false;
    return out;
}

QString LibSecretStore::lookup(Backend backend, const QByteArray &u,
                               const QByteArray &k, const QByteArray &i,
                               GError **err) const
{
    QString out;
    if (backend == Backend::Service) {
        GHashTable *attributes = scopedAttributes(u, &k, i);
        SecretValue *secret = secret_service_lookup_sync(
            nullptr, scopedSchema(), attributes, nullptr, err);
        g_hash_table_unref(attributes);
        if (secret) {
            if (const gchar *text = secret_value_get_text(secret))
                out = QString::fromUtf8(text);
            secret_value_unref(secret);
        }
    } else {
        gchar *raw = secret_password_lookup_sync(
            scopedSchema(), nullptr, err,
            "user_id", u.constData(),
            "key",     k.constData(),
            "install", i.constData(),
            nullptr);
        if (raw) {
            out = QString::fromUtf8(raw);
            secret_password_free(raw);
        }
    }
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

    const auto legacyLookup = [&] {
        return secret_password_lookup_sync(
            legacySchema(), nullptr, &err,
            "user_id", u.constData(),
            "key",     k.constData(),
            nullptr);
    };
    gchar *raw = legacyLookup();
    if (!err && !raw && throughSecretService(Backend::Default)) {
        if (defaultCollectionLock() == KeyringLock::Locked) {
            setError(QStringLiteral("the keyring is locked"));
            m_lastReadFailed = true;
            qCWarning(lcLibSecret) << "readLegacySecret failed:" << m_lastError;
            return {};
        }
        raw = legacyLookup();
    }

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
    if (installScope().isEmpty()) {
        setError(QLatin1String(kNoScope));
        return false;
    }
    GError *err = nullptr;
    const QByteArray u = toUtf8(userId);
    const QByteArray k = key ? toUtf8(*key) : QByteArray();
    const QByteArray i = toUtf8(installScope());
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
    return true;
}

#else

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
void LibSecretStore::allowUnlockPrompt() {}
bool LibSecretStore::unlockedForWriting() { return true; }

#endif
