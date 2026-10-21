#pragma once

#include "accounts/FileIdentity.h"

#include <QHash>
#include <QList>
#include <QMutex>
#include <QString>
#include <QtGlobal>

#include <atomic>

namespace lightning::accounts {

// Which Rust handle uses which account store, keyed by what the store IS on
// disk (FileIdentity), not by how its path was spelled.
//
// RECORD-ONLY UNLESS ENFORCED. Without live accounts (Labs) nothing here
// refuses anything: recordOpen() and claimOpen() always record, claimOpen()
// and claimForDeletion() answer Granted, and wouldRefuse() is the verdict
// computed so the client can log it. setEnforced(true), once per process and
// only from Labs (RustSdkMatrixClient::setLiveAccountsLabs), makes the two
// claims an atomic check-and-record: a store another handle opened, is
// opening, or has not confirmed closed is refused, and nothing is opened,
// moved aside or deleted on a refusal.
//
// Records carry the absolute store path, which contains the Matrix localpart:
// it is for the process's own bookkeeping and is never logged here.
class StoreOwnershipRegistry
{
public:
    enum class Role : quint8 { Foreground, Host, Starting };
    enum class State : quint8 { Live, Retiring };
    enum class ClaimVerdict : quint8 {
        Granted,
        RefusedLive,             // a Foreground or Host record holds it
        RefusedStarting,         // a host is restoring into it
        RefusedRetiring,         // a handle released it and has not confirmed the close
        RefusedDuplicateRecord   // another account's store path is this same store
    };
    struct Record {
        quint64 serial = 0;
        FileIdentity id;
        QString absPath;
        QString slug;
        Role role = Role::Foreground;
        State state = State::Live;
        // noteClosed(serial, false): the worker reported the close and could
        // not confirm it. Still Retiring and still an owner; unlike a close
        // still in progress, nothing will ever change it in this process.
        bool closeUnconfirmed = false;
    };

    explicit StoreOwnershipRegistry(FileIdentityResolver resolver = fileIdentityOf);

    // Leaked process instance (like retiredSignIns()): retirement workers may
    // still report a close while static destructors run.
    static StoreOwnershipRegistry &process();

    // Thread-safe (internal QMutex); noteClosed is called from retirement workers.
    quint64 recordOpen(const QString &storePath, const QString &slug, Role role); // serial > 0
    void noteReleasing(quint64 serial);                 // Live -> Retiring
    void noteClosed(quint64 serial, bool storeClosed);  // true: record removed; false: stays Retiring
    // Phase 1 verdict, computed but NOT enforced in Phase 0: another record (Live or Retiring)
    // has the same valid identity.
    bool wouldRefuse(const QString &storePath, quint64 *conflictSerial = nullptr) const;
    QList<Record> records() const;

    // One-way per process: true stays true (a runtime "off" keeps the safe
    // rules until restart). false is ignored once true.
    void setEnforced(bool on);
    bool enforced() const;
    // Atomic check-and-record. Not enforced: records exactly like recordOpen()
    // and returns Granted (*conflictSerial 0). Enforced: refused when any
    // record (Live or Retiring) is the same store; *serialOut is set only on
    // Granted. A store whose identity cannot be read is compared by its
    // absolute path instead, never waved through.
    ClaimVerdict claimOpen(const QString &storePath, const QString &slug, Role role,
                           quint64 *serialOut, quint64 *conflictSerial = nullptr);
    // Starting -> Host once the host's restore succeeded.
    void promote(quint64 serial, Role role);
    // Before a quarantine, move-aside, reset or delete of a store. Not
    // enforced: Granted. Enforced: refused while any record other than
    // `ownSerial` (the caller's own, already released handle) is that store.
    ClaimVerdict claimForDeletion(const QString &storePath, quint64 ownSerial = 0) const;
    // Who holds a store, for a caller deciding whether to wait (live
    // accounts' pool): nothing; a record whose close may still come (Live, or
    // Retiring with the close in progress); a close reported unconfirmed,
    // which nothing in this process will change; or a record of ANOTHER
    // account (another slug) on this same store. Compared as claimOpen()
    // compares, the identity, else the absolute path. `slug` is the caller's
    // effectiveStoreSlug(), as every claim passes it.
    enum class Hold : quint8 { None, Held, Unconfirmed, Duplicate };
    Hold holdOn(const QString &storePath, const QString &slug) const;
    // Fixed words for a log line.
    static const char *verdictName(ClaimVerdict verdict);
    QList<Record> recordsFor(const FileIdentity &id) const;
    void clearForTest();

private:
    FileIdentityResolver m_resolver;
    mutable QMutex m_mutex;
    // Serial order, so records() and every "first conflict" answer are stable.
    QList<Record> m_records;
    quint64 m_nextSerial = 1;
    std::atomic<bool> m_enforced{false};

    // Under m_mutex. The first record (serial order) that is the same store.
    const Record *conflictLocked(const FileIdentity &id, const QString &absPath,
                                 quint64 exceptSerial) const;
    static ClaimVerdict verdictFor(const Record &record);
};

} // namespace lightning::accounts
