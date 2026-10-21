// The function table for the live-accounts (Labs) FFIs, and the existing ones
// a background host uses, so new code never calls mx_rust_* directly and a
// test can script every answer (tests/support/FakeRustBridge.h).
//
// Header-only and Qt Core only: it is included by code compiled in builds
// without the Rust backend, where the table is a null one whose available()
// is false and every call refuses. Nothing here keeps state besides the test
// override.
//
// Threads (contract §2): the GUI thread may call only the non-blocking
// entries; the four marked BLOCKING, and finalSessionTokens(), run on the
// HostRetirementPool, as do the token-ledger entries. The table is never
// asked which thread it is on.
//
// SENSITIVE: restore()/oauthRestore() take tokens, takeCustody() and
// finalSessionTokens() return them, setMediaStoreKey() takes a key. None of
// them is logged here, and a caller must never log what it passes or gets.
#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <optional>

#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix_rust.h"
#endif

namespace lightning::accounts {

// mx_rust_bg_status, in Qt types. One tick's state; filled without allocation.
struct BgStatus {
    quint64 sessionStateVersion = 0, tokenGeneration = 0, notableUpdatesTotal = 0;
    quint32 queuedEvents = 0, queuedCommands = 0;
    bool dirty = false, revoked = false;
};

// mx_rust_take_custody, parsed. SENSITIVE: never log any field but the flags.
struct CustodySnapshot {
    bool valid = false;        // false: "" (no client), "error: ...", or not JSON
    bool hasSession = false;
    QString accessToken, refreshToken;
    quint64 generation = 0;
    bool revoked = false;
    std::optional<bool> softLogout, hadRefreshToken;
    // The newest rotation the handle's token ledger recorded, read before the
    // tokens: answered with sessionTokensPersisted() once the pair is written
    // (rust/src/sessiontokens.rs). 0: none, or a Rust side without it.
    quint64 ledgerGeneration = 0;
};

// Parses the §3.4 JSON. Pure; also used by the fake so both read one shape.
inline CustodySnapshot parseCustodyJson(const QByteArray &json)
{
    CustodySnapshot out;
    if (json.isEmpty() || json.startsWith("error: "))
        return out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject())
        return out;
    const QJsonObject o = doc.object();
    const auto optionalBool = [](const QJsonValue &v) -> std::optional<bool> {
        if (v.isBool())
            return v.toBool();
        return std::nullopt;
    };
    out.valid = true;
    out.hasSession = o.value(QStringLiteral("has_session")).toBool(false);
    out.accessToken = o.value(QStringLiteral("access_token")).toString();
    out.refreshToken = o.value(QStringLiteral("refresh_token")).toString();
    out.generation = static_cast<quint64>(o.value(QStringLiteral("generation")).toDouble(0));
    out.revoked = o.value(QStringLiteral("revoked")).toBool(false);
    out.softLogout = optionalBool(o.value(QStringLiteral("soft_logout")));
    out.hadRefreshToken = optionalBool(o.value(QStringLiteral("had_refresh_token")));
    out.ledgerGeneration =
        static_cast<quint64>(o.value(QStringLiteral("ledger_generation")).toDouble(0));
    return out;
}

class RustBridgeApi
{
public:
    virtual ~RustBridgeApi() = default;
    virtual bool available() const = 0;
    virtual void labsEnable() = 0;
    virtual void *createBackground(const QString &storePath, const QByteArray &profileJson) = 0;
    virtual QString setMediaStoreKey(void *h, const QByteArray &key, bool admitEncrypted) = 0;
    virtual QString restore(void *h, const QString &hs, const QString &user, const QString &device,
                            const QString &access, const QString &refresh) = 0;
    virtual QString oauthRestore(void *h, const QString &hs, const QString &user,
                                 const QString &device, const QString &clientId,
                                 const QString &access, const QString &refresh) = 0;
    virtual void startSync(void *h) = 0;
    virtual bool stopSync(void *h) = 0;                       // BLOCKING
    virtual QString pollEvent(void *h) = 0;                   // "" = empty
    virtual QString pollCommandEvent(void *h) = 0;
    virtual bool bgPoll(void *h, BgStatus *out) = 0;
    virtual QByteArray bgTake(void *h, quint32 maxRows) = 0;
    virtual QByteArray bgTakeSessionState(void *h) = 0;
    virtual QString bgSetLocalRules(void *h, const QStringList &rooms) = 0;
    virtual QString bgSetSendQueueEnabled(void *h, bool enabled) = 0;
    virtual CustodySnapshot takeCustody(void *h) = 0;
    // The answer for one rotation (CustodySnapshot::ledgerGeneration): the
    // pair is in the keyring (saved) or the write was refused or not made.
    // Releases that rotation's wait inside matrix-sdk's refresh. No secret.
    virtual void sessionTokensPersisted(void *h, quint64 generation, bool saved) = 0;
    virtual void logout(void *h) = 0;                         // BLOCKING (dispatch; result: logged_out)
    virtual QString oauthLogout(void *h) = 0;                 // BLOCKING (dispatch; result: logged_out)
    virtual QString shutdownTasks(void *h) = 0;               // BLOCKING
    virtual QString finalSessionTokens(void *h) = 0;          // BLOCKING after shutdown; SENSITIVE
    virtual QString destroyAndReport(void *h) = 0;            // BLOCKING
    // The token ledger, which outlives the handle: retained before
    // destroyAndReport(), read once the runtime is shut down (nothing can
    // rotate any more), then released. latest: final_session_tokens' shape,
    // "" when the handle's last client never rotated. SENSITIVE.
    virtual void *tokenLedgerRetain(void *h) = 0;
    virtual QString tokenLedgerLatest(void *ledger) = 0;
    virtual void tokenLedgerRelease(void *ledger) = 0;
    virtual int storeAnyHeld(const QString &storePath) = 0;   // 1 held, 0 free, -1 bad path
    virtual quint64 handleSerial(void *h) = 0;
};

// The table of a build without the Rust backend: nothing is available and
// every call refuses, so a caller that forgot to check available() still
// creates nothing.
class NullRustBridgeApi final : public RustBridgeApi
{
public:
    bool available() const override { return false; }
    void labsEnable() override {}
    void *createBackground(const QString &, const QByteArray &) override { return nullptr; }
    QString setMediaStoreKey(void *, const QByteArray &, bool) override { return refused(); }
    QString restore(void *, const QString &, const QString &, const QString &, const QString &,
                    const QString &) override { return refused(); }
    QString oauthRestore(void *, const QString &, const QString &, const QString &,
                         const QString &, const QString &, const QString &) override
    { return refused(); }
    void startSync(void *) override {}
    bool stopSync(void *) override { return false; }
    QString pollEvent(void *) override { return {}; }
    QString pollCommandEvent(void *) override { return {}; }
    bool bgPoll(void *, BgStatus *) override { return false; }
    QByteArray bgTake(void *, quint32) override { return {}; }
    QByteArray bgTakeSessionState(void *) override { return {}; }
    QString bgSetLocalRules(void *, const QStringList &) override { return refused(); }
    QString bgSetSendQueueEnabled(void *, bool) override { return refused(); }
    CustodySnapshot takeCustody(void *) override { return {}; }
    void sessionTokensPersisted(void *, quint64, bool) override {}
    void logout(void *) override {}
    QString oauthLogout(void *) override { return refused(); }
    QString shutdownTasks(void *) override { return {}; }
    QString finalSessionTokens(void *) override { return {}; }
    QString destroyAndReport(void *) override { return {}; }
    void *tokenLedgerRetain(void *) override { return nullptr; }
    QString tokenLedgerLatest(void *) override { return {}; }
    void tokenLedgerRelease(void *) override {}
    int storeAnyHeld(const QString &) override { return -1; }
    quint64 handleSerial(void *) override { return 0; }

private:
    static QString refused() { return QStringLiteral("error: no rust backend in this build"); }
};

#ifdef ENABLE_RUST_SDK_BACKEND
// The production table: each entry is the one FFI it names, its char* freed
// with mx_rust_free_cstring.
class ProductionRustBridgeApi final : public RustBridgeApi
{
public:
    bool available() const override { return true; }
    void labsEnable() override { mx_rust_labs_enable(); }
    void *createBackground(const QString &storePath, const QByteArray &profileJson) override
    {
        const QByteArray path = storePath.toUtf8();
        return mx_rust_create_background(path.constData(), profileJson.constData());
    }
    QString setMediaStoreKey(void *h, const QByteArray &key, bool admitEncrypted) override
    {
        return take(mx_rust_set_media_store_key(
            h, reinterpret_cast<const unsigned char *>(key.constData()),
            static_cast<unsigned long long>(key.size()), admitEncrypted ? 1 : 0));
    }
    QString restore(void *h, const QString &hs, const QString &user, const QString &device,
                    const QString &access, const QString &refresh) override
    {
        const QByteArray a = hs.toUtf8(), b = user.toUtf8(), c = device.toUtf8(),
                         d = access.toUtf8(), e = refresh.toUtf8();
        return take(mx_rust_restore(h, a.constData(), b.constData(), c.constData(),
                                    d.constData(), e.constData()));
    }
    QString oauthRestore(void *h, const QString &hs, const QString &user, const QString &device,
                         const QString &clientId, const QString &access,
                         const QString &refresh) override
    {
        const QByteArray a = hs.toUtf8(), b = user.toUtf8(), c = device.toUtf8(),
                         d = clientId.toUtf8(), e = access.toUtf8(), f = refresh.toUtf8();
        return take(mx_rust_oauth_restore(h, a.constData(), b.constData(), c.constData(),
                                          d.constData(), e.constData(), f.constData()));
    }
    void startSync(void *h) override { mx_rust_start_sync(h); }
    bool stopSync(void *h) override { return mx_rust_stop_sync(h) == 1; }
    QString pollEvent(void *h) override { return take(mx_rust_poll_event(h)); }
    QString pollCommandEvent(void *h) override { return take(mx_rust_poll_command_event(h)); }
    bool bgPoll(void *h, BgStatus *out) override
    {
        mx_rust_bg_status raw{};
        if (!out || mx_rust_bg_poll(h, &raw) != 0)
            return false;
        out->sessionStateVersion = raw.session_state_version;
        out->tokenGeneration = raw.token_generation;
        out->notableUpdatesTotal = raw.notable_updates_total;
        out->queuedEvents = raw.queued_events;
        out->queuedCommands = raw.queued_commands;
        out->dirty = raw.dirty != 0;
        out->revoked = raw.revoked != 0;
        return true;
    }
    QByteArray bgTake(void *h, quint32 maxRows) override
    {
        return takeBytes(mx_rust_bg_take(h, maxRows));
    }
    QByteArray bgTakeSessionState(void *h) override
    {
        return takeBytes(mx_rust_bg_take_session_state(h));
    }
    QString bgSetLocalRules(void *h, const QStringList &rooms) override
    {
        const QByteArray json =
            QJsonDocument(QJsonArray::fromStringList(rooms)).toJson(QJsonDocument::Compact);
        return take(mx_rust_bg_set_local_rules(h, json.constData()));
    }
    QString bgSetSendQueueEnabled(void *h, bool enabled) override
    {
        return take(mx_rust_bg_set_send_queue_enabled(h, enabled ? 1 : 0));
    }
    CustodySnapshot takeCustody(void *h) override
    {
        // SENSITIVE: the JSON carries both tokens; parsed and dropped here.
        return parseCustodyJson(takeBytes(mx_rust_take_custody(h)));
    }
    void sessionTokensPersisted(void *h, quint64 generation, bool saved) override
    {
        mx_rust_session_tokens_persisted(h, generation, saved ? 1 : 0);
    }
    void logout(void *h) override { mx_rust_logout(h); }
    QString oauthLogout(void *h) override { return take(mx_rust_oauth_logout(h)); }
    QString shutdownTasks(void *h) override { return take(mx_rust_shutdown_tasks(h)); }
    QString finalSessionTokens(void *h) override { return take(mx_rust_final_session_tokens(h)); }
    QString destroyAndReport(void *h) override { return take(mx_rust_destroy_and_report(h)); }
    void *tokenLedgerRetain(void *h) override { return mx_rust_token_ledger_retain(h); }
    QString tokenLedgerLatest(void *ledger) override
    {
        return take(mx_rust_token_ledger_latest(ledger));
    }
    void tokenLedgerRelease(void *ledger) override { mx_rust_token_ledger_release(ledger); }
    int storeAnyHeld(const QString &storePath) override
    {
        const QByteArray path = storePath.toUtf8();
        return mx_rust_store_any_held(path.constData());
    }
    quint64 handleSerial(void *h) override { return mx_rust_handle_serial(h); }

private:
    static QString take(char *raw)
    {
        if (!raw)
            return {};
        QString out = QString::fromUtf8(raw);
        mx_rust_free_cstring(raw);
        return out;
    }
    static QByteArray takeBytes(char *raw)
    {
        if (!raw)
            return {};
        QByteArray out(raw);
        mx_rust_free_cstring(raw);
        return out;
    }
};
#endif

namespace detail {
inline RustBridgeApi *&rustBridgeApiTestOverride()
{
    static RustBridgeApi *override = nullptr;
    return override;
}
} // namespace detail

// The test override when set; else the production table (a Rust build) or the
// null one. Contract deviation: a NON-const reference, because every entry but
// available() is a non-const call (binding it to a const reference still
// works for available()).
inline RustBridgeApi &rustBridgeApi()
{
    if (RustBridgeApi *api = detail::rustBridgeApiTestOverride())
        return *api;
#ifdef ENABLE_RUST_SDK_BACKEND
    static ProductionRustBridgeApi table;
#else
    static NullRustBridgeApi table;
#endif
    return table;
}

// nullptr restores the real table. Tests only; set before anything resolves
// the table (SessionPool resolves it once, at construction).
inline void setRustBridgeApiForTest(RustBridgeApi *api)
{
    detail::rustBridgeApiTestOverride() = api;
}

} // namespace lightning::accounts
