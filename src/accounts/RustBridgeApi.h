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

struct BgStatus {
    quint64 sessionStateVersion = 0, tokenGeneration = 0, notableUpdatesTotal = 0;
    quint32 queuedEvents = 0, queuedCommands = 0;
    bool dirty = false, revoked = false;
};

// mx_rust_take_custody, parsed. SENSITIVE: never log any field but the flags.
struct CustodySnapshot {
    bool valid = false;
    bool hasSession = false;
    QString accessToken, refreshToken;
    quint64 generation = 0;
    bool revoked = false;
    std::optional<bool> softLogout, hadRefreshToken;
    quint64 ledgerGeneration = 0;
};

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
    virtual bool stopSync(void *h) = 0;
    virtual QString pollEvent(void *h) = 0;
    virtual QString pollCommandEvent(void *h) = 0;
    virtual bool bgPoll(void *h, BgStatus *out) = 0;
    virtual QByteArray bgTake(void *h, quint32 maxRows) = 0;
    virtual QByteArray bgTakeSessionState(void *h) = 0;
    virtual QString bgSetLocalRules(void *h, const QStringList &rooms) = 0;
    virtual QString bgSetSendQueueEnabled(void *h, bool enabled) = 0;
    virtual CustodySnapshot takeCustody(void *h) = 0;
    virtual void sessionTokensPersisted(void *h, quint64 generation, bool saved) = 0;
    virtual void logout(void *h) = 0;
    virtual QString oauthLogout(void *h) = 0;
    virtual QString shutdownTasks(void *h) = 0;
    virtual QString finalSessionTokens(void *h) = 0;
    virtual QString destroyAndReport(void *h) = 0;
    virtual void *tokenLedgerRetain(void *h) = 0;
    virtual QString tokenLedgerLatest(void *ledger) = 0;
    virtual void tokenLedgerRelease(void *ledger) = 0;
    virtual int storeAnyHeld(const QString &storePath) = 0;
    virtual quint64 handleSerial(void *h) = 0;
};

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
}

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

inline void setRustBridgeApiForTest(RustBridgeApi *api)
{
    detail::rustBridgeApiTestOverride() = api;
}

}
