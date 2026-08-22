#include "media/StagedImageStore.h"

#include <QMutexLocker>

StagedImageStore::StagedImageStore(QObject *parent)
    : QObject(parent)
{
}

QString StagedImageStore::add(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return {};
    QMutexLocker locker(&m_mutex);
    if (m_entries.size() >= kMaxEntries)
        return {};
    const QString token = QString::number(m_nextToken++);
    m_entries.insert(token, bytes);
    return token;
}

void StagedImageStore::remove(const QString &token)
{
    if (token.isEmpty())
        return;
    QMutexLocker locker(&m_mutex);
    m_entries.remove(token);
}

void StagedImageStore::clear()
{
    QMutexLocker locker(&m_mutex);
    m_entries.clear();
}

QByteArray StagedImageStore::bytes(const QString &token) const
{
    if (token.isEmpty())
        return {};
    QMutexLocker locker(&m_mutex);
    return m_entries.value(token);
}

int StagedImageStore::count() const
{
    QMutexLocker locker(&m_mutex);
    return int(m_entries.size());
}
