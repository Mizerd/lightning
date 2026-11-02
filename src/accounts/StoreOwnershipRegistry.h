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
        RefusedLive,
        RefusedStarting,
        RefusedRetiring,
        RefusedDuplicateRecord
    };
    struct Record {
        quint64 serial = 0;
        FileIdentity id;
        QString absPath;
        QString slug;
        Role role = Role::Foreground;
        State state = State::Live;
        bool closeUnconfirmed = false;
    };

    explicit StoreOwnershipRegistry(FileIdentityResolver resolver = fileIdentityOf);

    static StoreOwnershipRegistry &process();

    quint64 recordOpen(const QString &storePath, const QString &slug, Role role);
    void noteReleasing(quint64 serial);
    void noteClosed(quint64 serial, bool storeClosed);
    bool wouldRefuse(const QString &storePath, quint64 *conflictSerial = nullptr) const;
    QList<Record> records() const;

    // One-way per process: true stays true (a runtime "off" keeps the safe
    // rules until restart). false is ignored once true.
    void setEnforced(bool on);
    bool enforced() const;
    ClaimVerdict claimOpen(const QString &storePath, const QString &slug, Role role,
                           quint64 *serialOut, quint64 *conflictSerial = nullptr);
    void promote(quint64 serial, Role role);
    ClaimVerdict claimForDeletion(const QString &storePath, quint64 ownSerial = 0) const;
    enum class Hold : quint8 { None, Held, Unconfirmed, Duplicate };
    Hold holdOn(const QString &storePath, const QString &slug) const;
    static const char *verdictName(ClaimVerdict verdict);
    QList<Record> recordsFor(const FileIdentity &id) const;
    void clearForTest();

private:
    FileIdentityResolver m_resolver;
    mutable QMutex m_mutex;
    QList<Record> m_records;
    quint64 m_nextSerial = 1;
    std::atomic<bool> m_enforced{false};

    const Record *conflictLocked(const FileIdentity &id, const QString &absPath,
                                 quint64 exceptSerial) const;
    static ClaimVerdict verdictFor(const Record &record);
};

}
