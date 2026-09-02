#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;
class SettingsManager;

// v0.9 (phase 11): "Send later".
//
// TWO mechanisms, never presented as one:
//   * SERVER — an MSC4140 delayed message event. The homeserver holds the
//     message and sends it at the deadline whether or not Lightning is
//     running. Used only when the server supports it (probed), the room is
//     KNOWN to be unencrypted (the endpoint stores the content as given —
//     see rooms.rs), and the message is a plain room message (no thread /
//     reply relation). The client keeps the delay id; there is no endpoint
//     to list delayed events after the fact, so a server-held entry is
//     RETIRED once its deadline has passed (the server sent it).
//   * LOCAL — a queue in this client. Honest label: Lightning must be
//     running and connected at the scheduled time. Entries for
//     UNENCRYPTED rooms persist account-scoped across restarts (a missed
//     deadline fires once on the next start); entries for ENCRYPTED rooms
//     are MEMORY-ONLY — plaintext scheduled for an encrypted room is never
//     written to disk (CLAUDE.md §6) — and the UI says so.
//
// Every server-side mutation is SERIALIZED on the entry's single in-flight
// op: a reschedule or edit of a server-held message cancels first, and
// only the server's "cancelled" answer triggers the replacement — a failed
// cancel is reported and never followed by a second delayed event (that
// would deliver the message twice). Changes asked for while the original
// schedule is still in flight are applied once the delay id arrives.
//
// A local dispatch goes through the ROOM-level send (`sendRoomMessage`),
// which works for any room, not only the open timeline, and answers with a
// real result: the entry stays "sending" until the room accepted the
// message, and reports failure instead of pretending. Duplicate-send
// protection: an entry is marked "sending" and persisted BEFORE its
// dispatch, so a crash between the two leaves a "sending" row that is
// reported as unsent rather than re-fired.
class ScheduledSendController : public QObject
{
    Q_OBJECT
    // [{id, roomId, roomName, body, html, sendAtMs, mode ("server"|"local"),
    //   status ("pending"|"sending"|"failed"), busy, error, volatile,
    //   delayId, threadRootId, replyToEventId}] sorted by sendAtMs.
    Q_PROPERTY(QVariantList pending READ pending NOTIFY pendingChanged)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY pendingChanged)
    // -1 unknown (not probed / no answer), 0 not supported, 1 supported.
    Q_PROPERTY(int serverScheduling READ serverScheduling NOTIFY supportChanged)

public:
    explicit ScheduledSendController(QObject *parent = nullptr);
    void setClient(MatrixClient *client);
    void setSettings(SettingsManager *settings);

    QVariantList pending() const;
    int pendingCount() const;
    int serverScheduling() const { return m_serverScheduling; }

    Q_INVOKABLE void probeSupport();
    // `message` is MessageComposer::composedMessage() (or the rich bridge's
    // composeDocument merged onto it): {roomId, body, html, mentionIds,
    // threadRootId, replyToEventId}. Returns the entry id, or "" when
    // refused (empty body, past deadline, no room).
    Q_INVOKABLE QString schedule(const QVariantMap &message, qint64 sendAtMs);
    Q_INVOKABLE void cancel(const QString &id);
    Q_INVOKABLE void sendNow(const QString &id);
    Q_INVOKABLE void reschedule(const QString &id, qint64 sendAtMs);
    Q_INVOKABLE void updateText(const QString &id, const QString &body,
                                const QString &html);
    Q_INVOKABLE QVariantList pendingForRoom(const QString &roomId) const;
    // Whether a message for this room would be scheduled server-side.
    Q_INVOKABLE bool wouldUseServer(const QString &roomId,
                                    const QString &threadRootId,
                                    const QString &replyToEventId) const;
    Q_INVOKABLE bool roomIsEncrypted(const QString &roomId) const;

    // A server-held entry whose deadline is this far in the past is taken
    // as sent by the server and retired.
    static constexpr qint64 kServerRetireGraceMs = 60 * 1000;

Q_SIGNALS:
    void pendingChanged();
    void supportChanged();

private:
    struct Entry {
        QString id;
        QString roomId;
        QString body;
        QString html;
        QStringList mentionIds;
        QString threadRootId;
        QString replyToEventId;
        qint64 sendAtMs = 0;
        QString mode;      // "server" | "local"
        QString status;    // "pending" | "sending" | "failed"
        QString error;
        bool isVolatile = false; // encrypted room: memory-only
        QString delayId;   // server mode
        quint64 op = 0;    // in-flight server op (schedule / cancel / send)
        quint64 sendOp = 0; // in-flight local room send
        // Server mode: what to apply once the in-flight op has answered.
        bool cancelRequested = false;
        bool resubmitAfterCancel = false;
        qint64 nextSendAtMs = -1;
        bool hasNextText = false;
        QString nextBody;
        QString nextHtml;
    };

    QVariantMap toMap(const Entry &e) const;
    Entry *find(const QString &id);
    int indexOf(const QString &id) const;
    bool busy(const Entry &e) const;
    void persist();
    void load();
    void armTimer();
    void fireDue();
    void dispatchLocal(Entry &e);
    void submitServer(Entry &e);
    void beginServerCancel(Entry &e);
    void applyDeferredChanges(Entry &e);
    void becomeLocal(Entry &e);
    void clearAll();
    QVariantMap bodySpecFor(const Entry &e) const;
    bool connected() const;

    MatrixClient *m_client = nullptr;
    SettingsManager *m_settings = nullptr;
    QList<Entry> m_entries;
    QTimer m_timer;
    int m_serverScheduling = -1;
    bool m_loaded = false;
};
