// Bounded, single-shot, memory-only store for remote session descriptions.
// Filled by the backend bridge in media-capable mode and consumed once by
// CallController when answering or completing an outbound handshake.
//
// SDP carries host IPs: never log, persist or expose it to QML. take()
// removes on read; clear() runs on sign-out/detach.
#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

namespace calls {

class SdpStore
{
public:
    // Only a handful of call events are ever live at once.
    static constexpr int kCapacity = 8;

    void insert(const QString &eventId, const QString &sdp)
    {
        if (eventId.isEmpty() || sdp.isEmpty())
            return;
        if (!m_byEvent.contains(eventId))
            m_order.append(eventId);
        m_byEvent.insert(eventId, sdp);
        while (m_order.size() > kCapacity)
            m_byEvent.remove(m_order.takeFirst());
    }

    // Single-shot: the description leaves the store on read.
    QString take(const QString &eventId)
    {
        const auto it = m_byEvent.find(eventId);
        if (it == m_byEvent.end())
            return {};
        const QString sdp = it.value();
        m_byEvent.erase(it);
        m_order.removeOne(eventId);
        return sdp;
    }

    void clear()
    {
        m_byEvent.clear();
        m_order.clear();
    }

    int size() const { return m_byEvent.size(); }

private:
    QHash<QString, QString> m_byEvent;
    QStringList m_order;
};

} // namespace calls
