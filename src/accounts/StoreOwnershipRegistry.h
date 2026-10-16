#pragma once

#include "accounts/FileIdentity.h"

#include <QHash>
#include <QList>
#include <QMutex>
#include <QString>
#include <QtGlobal>

namespace lightning::accounts {

// Which Rust handle uses which account store, keyed by what the store IS on
// disk (FileIdentity), not by how its path was spelled.
//
// PHASE 0 IS RECORD-ONLY. Nothing here refuses anything: recordOpen() always
// records, and wouldRefuse() is the Phase 1 verdict computed so the client can
// log it, never enforced. The point is to learn, from real sessions, whether
// that verdict would ever have fired on today's single-handle path before it
// is allowed to stop one.
//
// Records carry the absolute store path, which contains the Matrix localpart:
// it is for the process's own bookkeeping and is never logged here.
class StoreOwnershipRegistry
{
public:
    enum class Role : quint8 { Foreground, Host, Starting };   // Phase 0 records Foreground only
    enum class State : quint8 { Live, Retiring };
    struct Record {
        quint64 serial = 0;
        FileIdentity id;
        QString absPath;
        QString slug;
        Role role = Role::Foreground;
        State state = State::Live;
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
    QList<Record> recordsFor(const FileIdentity &id) const;
    void clearForTest();

private:
    FileIdentityResolver m_resolver;
    mutable QMutex m_mutex;
    // Serial order, so records() and every "first conflict" answer are stable.
    QList<Record> m_records;
    quint64 m_nextSerial = 1;
};

} // namespace lightning::accounts
