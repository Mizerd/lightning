#include "app/DraftStore.h"

#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"

DraftStore::DraftStore(QObject *parent)
    : QObject(parent)
{
}

void DraftStore::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    m_memory.clear();
    if (m_client) {
        connect(m_client, &MatrixClient::loggedOut, this,
                [this] { m_memory.clear(); });
    }
}

bool DraftStore::draftIsEmpty(const QVariantMap &draft)
{
    return draft.value(QStringLiteral("text")).toString().isEmpty()
        && draft.value(QStringLiteral("replyToEventId")).toString().isEmpty();
}

bool DraftStore::persistAllowed(const QString &realRoomId) const
{
    if (!m_client || realRoomId.isEmpty())
        return false; // unknown room → fail closed, memory only
    const RoomInfo info = m_client->roomInfo(realRoomId);
    if (info.id.isEmpty())
        return false; // room not in the authoritative list yet
    // Review H1: persistence needs an AFFIRMATIVE "not encrypted". A room
    // whose m.room.encryption state has not synced yet (a just-joined room
    // inside its first sync window) reads encrypted=false but
    // encryptionKnown=false — that is "unknown", never "plaintext is fine".
    return info.encryptionKnown && !info.encrypted;
}

void DraftStore::save(const QString &draftKey, const QString &realRoomId,
                      const QVariantMap &draft)
{
    if (draftKey.isEmpty())
        return;
    // Review M2: no session, no draft — a save landing during or after
    // sign-out (a late debounce, a close() running from the loggedOut
    // signal) must not re-insert the departing account's text into memory
    // the next account would inherit, nor re-create a persisted entry the
    // account wipe just removed.
    if (!m_client || !m_client->isLoggedIn())
        return;
    if (draftIsEmpty(draft)) {
        clear(draftKey);
        return;
    }
    if (persistAllowed(realRoomId)) {
        m_memory.remove(draftKey);
        if (m_settings)
            m_settings->setRoomDraft(draftKey, draft);
        return;
    }
    // Encrypted (or unknown) room: memory only, and make sure no stale
    // persisted copy survives from before the room became encrypted.
    m_memory.insert(draftKey, draft);
    if (m_settings)
        m_settings->setRoomDraft(draftKey, {});
}

QVariantMap DraftStore::load(const QString &draftKey) const
{
    if (draftKey.isEmpty())
        return {};
    const auto it = m_memory.constFind(draftKey);
    if (it != m_memory.constEnd())
        return it.value();
    return m_settings ? m_settings->roomDraft(draftKey) : QVariantMap{};
}

void DraftStore::clear(const QString &draftKey)
{
    if (draftKey.isEmpty())
        return;
    m_memory.remove(draftKey);
    if (m_settings)
        m_settings->setRoomDraft(draftKey, {});
}
