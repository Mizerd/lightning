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
    // Resolved outside the lock: stat() (or CreateFileW) is disk I/O, and a
    // retirement worker reporting a close must never wait behind it.
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
        if (storeClosed)
            m_records.removeAt(i);
        else
            m_records[i].state = State::Retiring;
        return;
    }
}

bool StoreOwnershipRegistry::wouldRefuse(const QString &storePath,
                                         quint64 *conflictSerial) const
{
    if (conflictSerial)
        *conflictSerial = 0;
    const FileIdentity id = m_resolver(storePath);
    // Two unreadable identities are never the same store: an id we could
    // not read proves nothing about sharing.
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
}

} // namespace lightning::accounts
