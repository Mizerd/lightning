#include "media/MediaVisibilityStore.h"

MediaVisibilityStore::MediaVisibilityStore(QObject *parent)
    : QObject(parent)
{
}

bool MediaVisibilityStore::isHidden(const QString &key) const
{
    return !key.isEmpty() && m_hidden.contains(key);
}

void MediaVisibilityStore::hide(const QString &key)
{
    if (key.isEmpty() || m_hidden.contains(key))
        return;
    m_hidden.insert(key);
    m_order.append(key);
    while (m_order.size() > kMaxHidden) {
        const QString evicted = m_order.takeFirst();
        if (m_hidden.remove(evicted))
            Q_EMIT hiddenChanged(evicted, false);
    }
    Q_EMIT hiddenChanged(key, true);
    Q_EMIT hiddenCountChanged();
}

void MediaVisibilityStore::show(const QString &key)
{
    if (key.isEmpty() || !m_hidden.remove(key))
        return;
    m_order.removeAll(key);
    Q_EMIT hiddenChanged(key, false);
    Q_EMIT hiddenCountChanged();
}

void MediaVisibilityStore::setHidden(const QString &key, bool hidden)
{
    if (hidden)
        hide(key);
    else
        show(key);
}

void MediaVisibilityStore::clear()
{
    if (m_hidden.isEmpty())
        return;
    const QStringList keys = m_order;
    m_hidden.clear();
    m_order.clear();
    // Announced per key, not as one blanket signal: the rows re-query by key,
    // and a row whose key was never in the set must not be told it changed.
    for (const QString &key : keys)
        Q_EMIT hiddenChanged(key, false);
    Q_EMIT hiddenCountChanged();
}
