// Shared by the live-accounts QML suites (AccountBadgesQmlTest,
// LabsConcurrentAccountsCardTest): a real AppController on the mock backend
// with N saved accounts, with Labs offered (the default) or forced off.
//
// Labs needs `lightning::accounts::rustBridgeApi().available()`. A mock build
// has no Rust bridge, so these suites install `InertRustBridge`: available,
// and every call a no-op that creates nothing (a background handle is
// nullptr). The QML suites drive AccountActivityModel directly; they test the
// presentation, not the hosts (SessionPoolTest and BackgroundHostTest do).
//
// The suites give SettingsManager an InMemorySecretStore: a process-local
// store, so the KeyringWorker (if a pin ever starts one) works through the GUI
// thread's instance and never builds a second, real keyring backend.
#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include "accounts/RustBridgeApi.h"
#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "storage/InMemorySecretStore.h"

namespace labsqml {

class InertRustBridge final : public lightning::accounts::RustBridgeApi
{
public:
    bool available() const override { return true; }
    void labsEnable() override {}
    void *createBackground(const QString &, const QByteArray &) override { return nullptr; }
    QString setMediaStoreKey(void *, const QByteArray &, bool) override
    { return QStringLiteral("error: inert"); }
    QString restore(void *, const QString &, const QString &, const QString &,
                    const QString &, const QString &) override
    { return QStringLiteral("error: inert"); }
    QString oauthRestore(void *, const QString &, const QString &, const QString &,
                         const QString &, const QString &, const QString &) override
    { return QStringLiteral("error: inert"); }
    void startSync(void *) override {}
    bool stopSync(void *) override { return true; }
    QString pollEvent(void *) override { return {}; }
    QString pollCommandEvent(void *) override { return {}; }
    bool bgPoll(void *, lightning::accounts::BgStatus *) override { return false; }
    QByteArray bgTake(void *, quint32) override { return {}; }
    QByteArray bgTakeSessionState(void *) override { return {}; }
    QString bgSetLocalRules(void *, const QStringList &) override
    { return QStringLiteral("error: inert"); }
    QString bgSetSendQueueEnabled(void *, bool) override
    { return QStringLiteral("error: inert"); }
    lightning::accounts::CustodySnapshot takeCustody(void *) override { return {}; }
    void sessionTokensPersisted(void *, quint64, bool) override {}
    void logout(void *) override {}
    QString oauthLogout(void *) override { return {}; }
    QString shutdownTasks(void *) override { return {}; }
    QString finalSessionTokens(void *) override { return {}; }
    QString destroyAndReport(void *) override { return {}; }
    void *tokenLedgerRetain(void *) override { return nullptr; }
    QString tokenLedgerLatest(void *) override { return {}; }
    void tokenLedgerRelease(void *) override {}
    int storeAnyHeld(const QString &) override { return 0; }
    quint64 handleSerial(void *) override { return 0; }
};

// Read once, in AppController's constructor. Labs is offered by default
// (2026-10-20); the variable only forces it off. `on` leaves the default
// (unset); off sets "0", the test override.
inline void setHiddenSwitch(bool on)
{
    if (on)
        qunsetenv("LIGHTNING_LABS_LIVE_ACCOUNTS");
    else
        qputenv("LIGHTNING_LABS_LIVE_ACCOUNTS", "0");
}

// A mock-backend AppController with `userIds` saved (on homeservers
// https://hs<N>.example), none of them active yet. The caller owns it. The
// override (forced off, or the default) is set before construction, and stays
// set until the next call.
inline AppController *makeController(bool hiddenSwitch, const QStringList &userIds)
{
    setHiddenSwitch(hiddenSwitch);
    auto *controller = new AppController(AppController::MockBackend);
    controller->settings()->setSecretStore(new InMemorySecretStore(controller));
    for (int i = 0; i < userIds.size(); ++i) {
        controller->settings()->saveSession(
            QStringLiteral("https://hs%1.example").arg(i + 1), userIds.at(i),
            QStringLiteral("DEV%1").arg(i + 1),
            QStringLiteral("fixture-token-%1").arg(i + 1));
    }
    return controller;
}

} // namespace labsqml
