#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantMap>

class MatrixClient;
class SettingsManager;

// Composer drafts, scoped to (account, room[, thread root]).
//
// Drafts in unencrypted rooms persist (account-scoped QSettings, bounded LRU).
// Drafts in encrypted rooms are memory-only, because encrypted-room plaintext
// is never persisted; a room whose encryption state is unknown counts as
// encrypted. Drafts hold text, mentions and the reply target only, and are
// never logged.
class DraftStore : public QObject
{
    Q_OBJECT

public:
    explicit DraftStore(QObject *parent = nullptr);

    void setSettings(SettingsManager *settings) { m_settings = settings; }
    void setClient(MatrixClient *client);

    // `draftKey` identifies the composer surface (room id or thread composite);
    // `realRoomId` is used only for the encryption decision. An empty draft
    // clears the entry.
    void save(const QString &draftKey, const QString &realRoomId,
              const QVariantMap &draft);
    QVariantMap load(const QString &draftKey) const;
    void clear(const QString &draftKey);

    // On account switch or sign-out. Persisted drafts are account-scoped and
    // wiped with the account.
    void clearMemoryDrafts() { m_memory.clear(); }

    static bool draftIsEmpty(const QVariantMap &draft);

private:
    bool persistAllowed(const QString &realRoomId) const;

    SettingsManager *m_settings = nullptr;
    MatrixClient *m_client = nullptr;
    QHash<QString, QVariantMap> m_memory;
};
