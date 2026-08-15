#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantMap>

class MatrixClient;
class SettingsManager;

// v0.7.x composer drafts, scoped (account, room[, thread root]).
//
// POLICY (confirmed by the maintainer, 2026-08-15): drafts in UNENCRYPTED
// rooms persist locally (account-scoped QSettings, bounded LRU); drafts in
// ENCRYPTED rooms are memory-only — they survive room/thread/Settings
// switches within the session but never restart, because encrypted-room
// plaintext is not written to CacheStore or any other persistent C++
// storage. A room whose encryption state is UNKNOWN (not yet synced) is
// treated as encrypted: fail closed, never persist by accident.
//
// Draft payloads carry text, mention refs and the reply target — never
// credentials, never attachments, never edit state. Nothing here is logged.
class DraftStore : public QObject
{
    Q_OBJECT

public:
    explicit DraftStore(QObject *parent = nullptr);

    void setSettings(SettingsManager *settings) { m_settings = settings; }
    void setClient(MatrixClient *client);

    // `draftKey` identifies the composer surface (room id, or the internal
    // thread-timeline composite); `realRoomId` is the actual Matrix room,
    // used only for the encryption policy decision. An effectively empty
    // draft clears the entry.
    void save(const QString &draftKey, const QString &realRoomId,
              const QVariantMap &draft);
    QVariantMap load(const QString &draftKey) const;
    void clear(const QString &draftKey);

    // Account switch / sign-out: encrypted-room drafts (memory) must never
    // survive into another account's session. Persisted drafts live under
    // the account's own settings group and are wiped with it.
    void clearMemoryDrafts() { m_memory.clear(); }

    static bool draftIsEmpty(const QVariantMap &draft);

private:
    bool persistAllowed(const QString &realRoomId) const;

    SettingsManager *m_settings = nullptr;
    MatrixClient *m_client = nullptr;
    QHash<QString, QVariantMap> m_memory;
};
