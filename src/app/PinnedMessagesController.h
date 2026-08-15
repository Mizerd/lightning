#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;

// v0.7.x pinned messages (`m.room.pinned_events`).
//
// The pinned list IS Matrix room state — Lightning stores nothing of its
// own. This controller owns the C++-side policy around it: which room is
// being tracked, when to re-read, what the viewer may do, and how an
// in-flight write reconciles with the authoritative state that follows.
//
// It tracks the ACTIVE room (AppController::setCurrentRoomId), not the Room
// Information panel's room, because `isPinned()` has to answer for the
// message the user is right-clicking in the open timeline.
//
// Honesty rules, matching the presence/facepile precedents:
//   - A failed fetch keeps the last known list rather than erasing it; only
//     an authoritative answer replaces an answer.
//   - `canPin` is the SDK's own power-level check against the room's real
//     required level for `m.room.pinned_events`. It is false until a
//     snapshot has actually said otherwise — never optimistically true.
//   - A pin/unpin is NOT applied optimistically. The write completes, then
//     the authoritative list is re-read; a server rejection therefore
//     cannot leave the UI showing a state the room does not have.
//   - Entry previews are decrypted message text in an encrypted room. They
//     live in memory only and are never written to CacheStore.
class PinnedMessagesController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString roomId READ roomId NOTIFY roomIdChanged)
    // Backend capability alone. QML hides the whole surface when false.
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    // Resolved rows, newest-pinned last, capped by the bridge. Each is a map
    // with eventId + available, and when available sender,
    // senderDisplayName, senderAvatarUrl, timestampMs, kind, preview.
    Q_PROPERTY(QVariantList entries READ entries NOTIFY stateChanged)
    // How many events the room actually pins. May exceed entries.count()
    // when the bridge capped resolution — `truncated` says so.
    Q_PROPERTY(int total READ total NOTIFY stateChanged)
    Q_PROPERTY(bool truncated READ truncated NOTIFY stateChanged)
    Q_PROPERTY(bool canPin READ canPin NOTIFY stateChanged)
    // A pin/unpin write is in flight. One at a time.
    Q_PROPERTY(bool pending READ pending NOTIFY stateChanged)
    // Sanitized, user-facing failure text for the LAST write; empty on
    // success. Cleared when a new write starts.
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    // Bumped on every state change. `isPinned()` and `canTogglePin()` are
    // Q_INVOKABLEs, so a QML binding cannot observe their inputs on its
    // own — reference this property in the binding (the PresenceManager
    // revision idiom) and it re-evaluates.
    Q_PROPERTY(int revision READ revision NOTIFY stateChanged)

public:
    explicit PinnedMessagesController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    // Called by AppController when the active room changes.
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

    // Pure read over the CURRENT room's complete id list — safe in a QML
    // binding, re-evaluate on stateChanged. Answers false for an empty id
    // or before the first snapshot: "not known to be pinned", which is the
    // only claim the data supports.
    Q_INVOKABLE bool isPinned(const QString &eventId) const;
    // Whether the pin/unpin action should be OFFERED for this event. The
    // policy lives here rather than in QML (architecture §5): it requires
    // pin permission, a loaded snapshot, a non-empty event id, no write
    // already in flight, and — for a pin — that the event is not already
    // pinned (and vice versa).
    Q_INVOKABLE bool canTogglePin(const QString &eventId, bool pin) const;
    Q_INVOKABLE void pin(const QString &eventId);
    Q_INVOKABLE void unpin(const QString &eventId);
    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void roomIdChanged();
    void supportedChanged();
    void stateChanged();
    // A pin/unpin write finished. `message` is empty on success. Main.qml's
    // pinActionNotice renders FAILURES from this — the action is usually
    // taken from the message context menu, which closes on trigger, so
    // `error` alone (only visible inside Room Information → Pinned) would
    // let a refused pin fail silently. Success is deliberately quiet: the
    // pinned list updating is the feedback.
    void pinActionFinished(const QString &roomId, const QString &eventId,
                           bool pin, bool ok, const QString &message);

private:
    // Bump the revision and notify. Every state mutation goes through here
    // so a QML binding on revision cannot miss one.
    void emitStateChanged();
    void fetch(bool allowRemote);
    void clearSnapshot();
    void setPinned(const QString &eventId, bool pin);

    MatrixClient *m_client = nullptr;
    QString m_roomId;
    quint64 m_fetchOp = 0;
    // A refresh became due while one was already in flight. The in-flight
    // read predates whatever made the refresh due (our own completed write,
    // or a remote change), so its answer is stale by construction and
    // dropping the request would leave the list wrong until the next
    // unrelated poke. Honoured when the in-flight read lands.
    bool m_refreshOwed = false;
    quint64 m_writeOp = 0;
    // The room a pending write targets. A write's answer must never be
    // applied to whatever room the user has since switched to.
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
    // Rooms whose pinned state has been fetched at least once this session.
    // The bridge's /state fallback (for rooms carrying NO pinned-events
    // state) is worth one request per room, not one per refresh.
    QSet<QString> m_remoteProbed;
};
