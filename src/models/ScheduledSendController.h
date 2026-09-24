#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;
class SettingsManager;

// "Send later", with two mechanisms never presented as one:
//   * SERVER: an MSC4140 delayed event. The homeserver sends it at the
//     deadline whether or not Lightning runs. Used only when supported
//     (probed), the room is known to be unencrypted (the server stores the
//     content as given, see rooms.rs), and there is no thread/reply
//     relation. Delayed events cannot be listed later, so a server-held
//     entry is retired once its deadline has passed.
//   * LOCAL: a queue in this client; Lightning must be running and connected
//     at the time. Unencrypted-room entries persist per account (a missed
//     deadline fires once on next start). Encrypted-room entries are
//     memory-only: their plaintext is never written to disk, and the UI says
//     so.
//
// Server-side mutations are serialized on the entry's single in-flight op: a
// reschedule or edit cancels first and replaces only after the server
// confirms. A failed cancel is reported and never followed by a second
// delayed event (double delivery). Changes requested while the original
// schedule is in flight apply once the delay id arrives.
//
// Local dispatch uses the room-level sendRoomMessage, which works for any room
// and reports a real result. An entry is marked "sending" and persisted
// before dispatch, so a crash leaves a row reported as unsent, not re-fired.
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
    // `message` is MessageComposer::composedMessage() (optionally merged with
    // the rich bridge's composeDocument): {roomId, body, html, mentionIds,
    // threadRootId, replyToEventId}. Returns the entry id, or "" when refused
    // (empty body, past deadline, no room).
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

    // A server-held entry this far past its deadline is taken as sent and
    // retired.
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
