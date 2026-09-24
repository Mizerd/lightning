#include "media/MediaVisibilityStore.h"

#include "app/SettingsManager.h"

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
    persist();
}

void MediaVisibilityStore::show(const QString &key)
{
    if (key.isEmpty() || !m_hidden.remove(key))
        return;
    m_order.removeAll(key);
    Q_EMIT hiddenChanged(key, false);
    Q_EMIT hiddenCountChanged();
    persist();
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
    // Announced per key; rows re-query by key.
    for (const QString &key : keys)
        Q_EMIT hiddenChanged(key, false);
    Q_EMIT hiddenCountChanged();
    persist();
}

void MediaVisibilityStore::resetForSession()
{
    if (m_hidden.isEmpty())
        return;
    const QStringList keys = m_order;
    m_hidden.clear();
    m_order.clear();
    // Not persisted (see the header). Announced so rows on screen reveal.
    for (const QString &key : keys)
        Q_EMIT hiddenChanged(key, false);
    Q_EMIT hiddenCountChanged();
}

void MediaVisibilityStore::setSettings(SettingsManager *settings)
{
    if (m_settings == settings)
        return;
    m_settings = settings;
    reloadForAccount();
}

void MediaVisibilityStore::reloadForAccount()
{
    // Announce the old keys as shown first, so rows still on screen from the
    // previous account reveal.
    const QStringList previous = m_order;
    m_hidden.clear();
    m_order.clear();

    if (m_settings) {
        // A user-editable INI: drop empty keys and apply the cap on read too.
        const QStringList stored = m_settings->hiddenMediaKeys();
        for (const QString &key : stored) {
            if (key.isEmpty() || m_hidden.contains(key))
                continue;
            m_hidden.insert(key);
            m_order.append(key);
            if (m_order.size() >= kMaxHidden)
                break;
        }
    }

    for (const QString &key : previous) {
        if (!m_hidden.contains(key))
            Q_EMIT hiddenChanged(key, false);
    }
    for (const QString &key : std::as_const(m_order))
        Q_EMIT hiddenChanged(key, true);
    Q_EMIT hiddenCountChanged();
}

void MediaVisibilityStore::persist()
{
    if (m_settings)
        m_settings->setHiddenMediaKeys(m_order);
}
