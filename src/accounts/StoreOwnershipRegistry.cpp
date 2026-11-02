#include "accounts/StoreOwnershipRegistry.h"

#include <QFileInfo>
#include <QMutexLocker>

#include <utility>

namespace lightning::accounts {

StoreOwnershipRegistry::StoreOwnershipRegistry(FileIdentityResolver resolver)
    : m_resolver(resolver ? std::move(resolver) : FileIdentityResolver(fileIdentityOf))
{
}

StoreOwnershipRegistry &StoreOwnershipRegistry::process()
{
    static StoreOwnershipRegistry *registry = new StoreOwnershipRegistry;
    return *registry;
}

quint64 StoreOwnershipRegistry::recordOpen(const QString &storePath, const QString &slug,
                                           Role role)
{
    Record record;
    record.id = m_resolver(storePath);
    record.absPath = QFileInfo(storePath).absoluteFilePath();
    record.slug = slug;
    record.role = role;
    record.state = State::Live;
    QMutexLocker lock(&m_mutex);
    record.serial = m_nextSerial++;
    m_records.append(record);
    return record.serial;
}

void StoreOwnershipRegistry::noteReleasing(quint64 serial)
{
    if (serial == 0)
        return;
    QMutexLocker lock(&m_mutex);
    for (Record &record : m_records) {
        if (record.serial == serial) {
            record.state = State::Retiring;
            return;
        }
    }
}

void StoreOwnershipRegistry::noteClosed(quint64 serial, bool storeClosed)
{
    if (serial == 0)
        return;
    QMutexLocker lock(&m_mutex);
    for (qsizetype i = 0; i < m_records.size(); ++i) {
        if (m_records.at(i).serial != serial)
            continue;
        // A close the worker could not confirm leaves the store possibly
        // open, so the record stays, Retiring, and still counts as an owner.
        if (storeClosed) {
            m_records.removeAt(i);
        } else {
            m_records[i].state = State::Retiring;
            m_records[i].closeUnconfirmed = true;
        }
        return;
    }
}

bool StoreOwnershipRegistry::wouldRefuse(const QString &storePath,
                                         quint64 *conflictSerial) const
{
    if (conflictSerial)
        *conflictSerial = 0;
    const FileIdentity id = m_resolver(storePath);
    if (!id.valid)
        return false;
    QMutexLocker lock(&m_mutex);
    for (const Record &record : m_records) {
        if (record.id.valid && record.id == id) {
            if (conflictSerial)
                *conflictSerial = record.serial;
            return true;
        }
    }
    return false;
}

QList<StoreOwnershipRegistry::Record> StoreOwnershipRegistry::records() const
{
    QMutexLocker lock(&m_mutex);
    return m_records;
}

QList<StoreOwnershipRegistry::Record>
StoreOwnershipRegistry::recordsFor(const FileIdentity &id) const
{
    QList<Record> out;
    if (!id.valid)
        return out;
    QMutexLocker lock(&m_mutex);
    for (const Record &record : m_records) {
        if (record.id.valid && record.id == id)
            out.append(record);
    }
    return out;
}

void StoreOwnershipRegistry::clearForTest()
{
    QMutexLocker lock(&m_mutex);
    m_records.clear();
    m_nextSerial = 1;
    m_enforced.store(false);
}

void StoreOwnershipRegistry::setEnforced(bool on)
{
    if (on)
        m_enforced.store(true);
}

bool StoreOwnershipRegistry::enforced() const
{
    return m_enforced.load();
}

const StoreOwnershipRegistry::Record *
StoreOwnershipRegistry::conflictLocked(const FileIdentity &id, const QString &absPath,
                                       quint64 exceptSerial) const
{
    for (const Record &record : m_records) {
        if (record.serial == exceptSerial)
            continue;
        const bool same = (id.valid && record.id.valid) ? record.id == id
                                                        : record.absPath == absPath;
        if (same)
            return &record;
    }
    return nullptr;
}

StoreOwnershipRegistry::ClaimVerdict StoreOwnershipRegistry::verdictFor(const Record &record)
{
    if (record.state == State::Retiring)
        return ClaimVerdict::RefusedRetiring;
    if (record.role == Role::Starting)
        return ClaimVerdict::RefusedStarting;
    return ClaimVerdict::RefusedLive;
}

StoreOwnershipRegistry::ClaimVerdict
StoreOwnershipRegistry::claimOpen(const QString &storePath, const QString &slug, Role role,
                                  quint64 *serialOut, quint64 *conflictSerial)
{
    if (conflictSerial)
        *conflictSerial = 0;
    if (serialOut)
        *serialOut = 0;
    if (!m_enforced.load()) {
        const quint64 serial = recordOpen(storePath, slug, role);
        if (serialOut)
            *serialOut = serial;
        return ClaimVerdict::Granted;
    }
    Record record;
    record.id = m_resolver(storePath);
    record.absPath = QFileInfo(storePath).absoluteFilePath();
    record.slug = slug;
    record.role = role;
    record.state = State::Live;
    QMutexLocker lock(&m_mutex);
    if (const Record *other = conflictLocked(record.id, record.absPath, 0)) {
        if (conflictSerial)
            *conflictSerial = other->serial;
        if (!slug.isEmpty() && !other->slug.isEmpty() && other->slug != slug)
            return ClaimVerdict::RefusedDuplicateRecord;
        return verdictFor(*other);
    }
    record.serial = m_nextSerial++;
    m_records.append(record);
    if (serialOut)
        *serialOut = record.serial;
    return ClaimVerdict::Granted;
}

void StoreOwnershipRegistry::promote(quint64 serial, Role role)
{
    if (serial == 0)
        return;
    QMutexLocker lock(&m_mutex);
    for (Record &record : m_records) {
        if (record.serial == serial) {
            record.role = role;
            return;
        }
    }
}

StoreOwnershipRegistry::ClaimVerdict
StoreOwnershipRegistry::claimForDeletion(const QString &storePath, quint64 ownSerial) const
{
    if (!m_enforced.load())
        return ClaimVerdict::Granted;
    const FileIdentity id = m_resolver(storePath);
    const QString absPath = QFileInfo(storePath).absoluteFilePath();
    QMutexLocker lock(&m_mutex);
    if (const Record *other = conflictLocked(id, absPath, ownSerial))
        return verdictFor(*other);
    return ClaimVerdict::Granted;
}

StoreOwnershipRegistry::Hold StoreOwnershipRegistry::holdOn(const QString &storePath,
                                                            const QString &slug) const
{
    const FileIdentity id = m_resolver(storePath);
    const QString absPath = QFileInfo(storePath).absoluteFilePath();
    QMutexLocker lock(&m_mutex);
    Hold hold = Hold::None;
    for (const Record &record : m_records) {
        const bool same = (id.valid && record.id.valid) ? record.id == id
                                                        : record.absPath == absPath;
        if (!same)
            continue;
        if (!slug.isEmpty() && !record.slug.isEmpty() && record.slug != slug)
            return Hold::Duplicate;
        if (record.closeUnconfirmed)
            hold = Hold::Unconfirmed;
        else if (hold == Hold::None)
            hold = Hold::Held;
    }
    return hold;
}

const char *StoreOwnershipRegistry::verdictName(ClaimVerdict verdict)
{
    switch (verdict) {
    case ClaimVerdict::Granted:
        return "granted";
    case ClaimVerdict::RefusedLive:
        return "refused_live";
    case ClaimVerdict::RefusedStarting:
        return "refused_starting";
    case ClaimVerdict::RefusedRetiring:
        return "refused_retiring";
    case ClaimVerdict::RefusedDuplicateRecord:
        return "refused_duplicate_record";
    }
    return "unknown";
}

}
