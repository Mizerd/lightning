#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;

// Pinned messages (`m.room.pinned_events`) for the active room. The list is
// room state; this controller owns the policy around it.
//
//   - A failed fetch keeps the last known list.
//   - `canPin` is the SDK's power-level check and false until a snapshot
//     says otherwise.
//   - Pin/unpin is not optimistic: the list is re-read after each write.
//   - Previews may be decrypted text; they live in memory only and are never
//     written to CacheStore.
class PinnedMessagesController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString roomId READ roomId NOTIFY roomIdChanged)
    // Backend capability; QML hides the surface when false.
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    // Resolved rows, newest-pinned last, capped by the bridge. Each is a map
    // with eventId + available, and when available sender,
    // senderDisplayName, senderAvatarUrl, timestampMs, kind, preview.
    Q_PROPERTY(QVariantList entries READ entries NOTIFY stateChanged)
    // May exceed entries.count() when the bridge capped resolution.
    Q_PROPERTY(int total READ total NOTIFY stateChanged)
    Q_PROPERTY(bool truncated READ truncated NOTIFY stateChanged)
    Q_PROPERTY(bool canPin READ canPin NOTIFY stateChanged)
    // A pin/unpin write is in flight (one at a time).
    Q_PROPERTY(bool pending READ pending NOTIFY stateChanged)
    // Sanitized failure text for the last write; empty on success.
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    // Bumped on every state change. Reference it in bindings that call
    // isPinned()/canTogglePin(), which QML cannot track on their own.
    Q_PROPERTY(int revision READ revision NOTIFY stateChanged)
    // The complete id list (entries are capped); used by room search.
    Q_PROPERTY(QStringList ids READ ids NOTIFY stateChanged)

public:
    explicit PinnedMessagesController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void setRoomId(const QString &roomId);

    QString roomId() const { return m_roomId; }
    bool supported() const;
    bool loading() const { return m_fetchOp != 0; }
    QVariantList entries() const { return m_entries; }
    int total() const { return m_total; }
    bool truncated() const { return m_truncated; }
    bool canPin() const { return m_canPin; }
    bool pending() const { return m_writeOp != 0; }
    QString error() const { return m_error; }
    int revision() const { return m_revision; }
    QStringList ids() const { return m_ids; }

    // False for an empty id or before the first snapshot.
    Q_INVOKABLE bool isPinned(const QString &eventId) const;
    // Whether to offer pin (or unpin) for this event: requires permission, a
    // loaded snapshot, no write in flight, and a state change to make.
    Q_INVOKABLE bool canTogglePin(const QString &eventId, bool pin) const;
    Q_INVOKABLE void pin(const QString &eventId);
    Q_INVOKABLE void unpin(const QString &eventId);
    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void roomIdChanged();
    void supportedChanged();
    void stateChanged();
    // `message` is empty on success. Main.qml shows failures from this,
    // since the context menu that triggered the write has already closed.
    void pinActionFinished(const QString &roomId, const QString &eventId,
                           bool pin, bool ok, const QString &message);

private:
    // Every state mutation goes through here so `revision` never misses one.
    void emitStateChanged();
    void fetch(bool allowRemote);
    void clearSnapshot();
    void setPinned(const QString &eventId, bool pin);

    MatrixClient *m_client = nullptr;
    QString m_roomId;
    quint64 m_fetchOp = 0;
    // A refresh requested during an in-flight read; that read's answer is
    // already stale, so it is re-issued when the read lands.
    bool m_refreshOwed = false;
    quint64 m_writeOp = 0;
    // A write's answer must never apply to a room the user switched to.
    QString m_writeRoomId;
    QString m_writeEventId;
    bool m_writePin = false;
    QVariantList m_entries;
    QStringList m_ids;
    QSet<QString> m_idSet;
    int m_total = 0;
    bool m_truncated = false;
    bool m_canPin = false;
    int m_revision = 0;
    QString m_error;
    // Rooms already probed this session; the /state fallback runs once per
    // room.
    QSet<QString> m_remoteProbed;
};
