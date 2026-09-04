#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantMap>

class MatrixClient;
class MediaBridge;

// v0.7.x message forwarding (task #14). Matrix has no forward primitive —
// matrix-sdk 0.18 exposes none either — so forwarding IS "read the source
// event's content, send a NEW, UNRELATED event into the target room",
// exactly like Element. See the contract this class follows (decisions
// already made, not relitigated here):
//
//   D1 media is RE-UPLOADED, never mxc-copied. Copying the source event's
//      url/file block into another room is wrong twice over: under
//      authenticated media the target room's members may not be entitled
//      to fetch that mxc at all, and in an encrypted source the `file`
//      block carries per-event decryption keys that would then exist in a
//      room those keys were never negotiated for. Re-uploading through the
//      normal attachment path (D2/D3) re-encrypts correctly for whatever
//      the TARGET room is, in both the plain->encrypted and
//      encrypted->plain directions.
//   D4 text carries the plain body ONLY — never formatted_body. The source
//      HTML can embed pills/permalinks naming the SOURCE room, and
//      sanitising that correctly for an arbitrary target is its own
//      problem; this round deliberately does not attempt it.
//   D5 the forward NEVER carries a relation — not a reply, not m.thread —
//      even when the source itself was a thread reply. CLAUDE.md §8: a
//      stray m.thread relation landing in a room that never negotiated
//      that thread is a real defect, not a cosmetic one. sendTextMessage(),
//      sendAttachmentBytes() and sendAttachmentBytesToRoom() attach no
//      relation by construction, so
//      this holds simply by using those calls and nothing else.
//   D6 nothing is optimistic: `forwarded()` — which AppController wires to
//      opening the target room — fires only once the send has actually
//      been DISPATCHED to the SDK: for text that is the ordinary
//      fire-and-forget sendTextMessage() call (exactly like the composer's
//      own send), for media it is a non-zero sendAttachmentBytes() op id.
//      A failure BEFORE dispatch (media fetch failure, or a zero op id) is
//      reported via `error` and the dialog stays open — never swallowed.
//      A failure AFTER dispatch cannot use `error`: the picker has closed
//      and the user has been navigated to the target. An attachment send is
//      a direct upload rather than a queued one, and the target timeline
//      was not open, so there is no local echo to fail visibly either —
//      without explicit handling a server refusal (no permission, rate
//      limit, over m.upload.size) would be completely silent. Those surface
//      on forwardFailed() instead; see m_dispatchedSends.
//   D7 redacted, local-echo, and undecrypted content is refused at
//      begin(), before any network activity — none of it has anything
//      safe to re-send.
//
// Selection identity: begin() takes an IMMUTABLE SNAPSHOT of the exact row
// the user activated, built by the QML delegate directly from its own
// `model` role data at the moment the menu item is clicked (see
// MessageDelegate.qml's "Forward" item) — the same discipline the GIF
// picker uses (GifSendController::sendToRoom/sendToThread take a captured
// QVariantMap, never a live index). It deliberately does NOT re-resolve
// the event later through TimelineModel: the model backing an open menu
// can be replaced, reordered, or redacted between activation and the
// user's eventual room choice, and re-resolving at forwardTo() time would
// let a stale click send whatever now sits at that identity instead of
// what was actually chosen. A `mediaKey` in the snapshot is still
// re-fetched FRESH through MediaBridge (which itself decrypts through the
// SDK, exactly like any other attachment) — only the classification
// (kind/body/filename/mime/dimensions) is frozen at activation, never the
// bytes themselves.
class ForwardController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(QString sourceRoomId READ sourceRoomId NOTIFY changed)
    Q_PROPERTY(QString sourceEventId READ sourceEventId NOTIFY changed)
    Q_PROPERTY(QString previewText READ previewText NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    /// A multi-message forward is in progress or has finished with results.
    Q_PROPERTY(bool selectionActive READ selectionActive NOTIFY changed)
    Q_PROPERTY(int progressDone READ progressDone NOTIFY changed)
    Q_PROPERTY(int progressTotal READ progressTotal NOTIFY changed)
    Q_PROPERTY(int failureCount READ failureCount NOTIFY changed)
    /// [{roomId, eventId, message}] — WHICH pair failed, not just how many.
    Q_PROPERTY(QVariantList failures READ failures NOTIFY changed)
    Q_PROPERTY(QString forwardMode READ forwardMode NOTIFY changed)

public:
    explicit ForwardController(QObject *parent = nullptr);

    // Connects loggedOut so a forward in flight (or a still-open dialog)
    // can never survive into a different account (matches GifSendController's
    // own cancelAll()-on-loggedOut wiring).
    void setClient(MatrixClient *client);
    // Media re-fetch path (D1/D2) — the SAME decrypting fetch every other
    // save/star action uses.
    void setMediaBridge(MediaBridge *bridge);

    bool active() const { return m_active; }
    QString sourceRoomId() const { return m_sourceRoomId; }
    QString sourceEventId() const { return m_sourceEventId; }
    QString previewText() const { return m_previewText; }
    bool busy() const { return m_busy; }
    QString error() const { return m_error; }

    // Opens the picker for the row the caller just activated. `sourceRoomId`
    // must already be the REAL room id (TimelineModel::realRoomIdForEvent —
    // the composite thread-timeline id must never reach this class; see
    // MessageDelegate.qml's call site). `snapshot` is the immutable capture
    // described in the class comment; see the keys read in the .cpp. Refuses
    // (sets `error`, leaves `active` false) for redacted / local-echo /
    // undecryptable content (D7) or an empty snapshot with nothing to send.
    Q_INVOKABLE void begin(const QString &sourceRoomId,
                          const QString &sourceEventId,
                          const QVariantMap &snapshot);
    // Closes the picker without sending.
    Q_INVOKABLE void cancel();
    // The user picked a target room. Dispatches the text or media send for
    // the frozen snapshot. Refuses silently while not active or already
    // busy (the dialog disables its own controls on `busy`, this is
    // defense in depth against a double-activation).
    Q_INVOKABLE void forwardTo(const QString &targetRoomId);

    // ── Multi-message, multi-destination forwarding (0.9) ────────────────
    //
    // The single-message path above is unchanged and still used by the
    // message context menu. This is the SELECTION path: N frozen snapshots
    // to M destinations, which is a different problem — it can partially
    // fail, and saying "sent" because one of twelve sends worked would be a
    // lie the user acts on.
    //
    // Destinations are {roomId, threadRootId} pairs. A thread root turns the
    // forward into a thread reply IN THE TARGET, which is a relation the
    // target room negotiated — unlike D5's refusal to carry the SOURCE's
    // relation, which named a thread the target knows nothing about.
    Q_INVOKABLE void beginSelection(const QString &sourceRoomId,
                                    const QVariantList &snapshots);
    /// "content" (a clean copy) or "context" (attributed with the original
    /// sender, room and time). Context is a CONSCIOUS choice: it discloses a
    /// private room's name and a sender to whoever receives the copy, so it
    /// is never the default.
    Q_INVOKABLE void setForwardMode(const QString &mode);
    Q_INVOKABLE void sendSelection(const QVariantList &targets);
    /// Re-dispatch only the pairs that failed.
    Q_INVOKABLE void retryFailures();

    bool selectionActive() const { return m_selectionActive; }
    int progressDone() const { return m_progressDone; }
    int progressTotal() const { return m_progressTotal; }
    int failureCount() const { return int(m_failures.size()); }
    QVariantList failures() const { return m_failures; }
    QString forwardMode() const { return m_mode; }

public:
    // Exposed for tests: a forwarded attachment's name is re-originated
    // under this account, so path structure and leading dots are stripped.
    // Not Q_INVOKABLE — QML has no business renaming a forward.
    static QString sanitizedForwardFilename(const QString &raw);

Q_SIGNALS:
    void changed();
    // A forward was actually dispatched. AppController opens
    // `targetRoomId`; this must never fire before that.
    void forwarded(const QString &targetRoomId);
    // A forward that was DISPATCHED was then refused by the server. Carries
    // the room it was aimed at, because by the time this arrives the picker
    // has closed and the user has already been navigated there.
    void forwardFailed(const QString &targetRoomId, const QString &message);

private Q_SLOTS:
    // A media send this controller dispatched has finished. Matched by op
    // id against m_dispatchedSends; inert for every other op in the app.
    void onAttachmentQueueFinished(quint64 opId, const QString &roomId,
                                   bool ok, const QString &category);
    // MediaBridge::mediaBytesForStar is broadcast to every listener for
    // every media key in flight (star, save, and now forward can all be
    // outstanding at once) — this filters to the ONE key this controller is
    // currently waiting on.
    void onMediaBytesForStar(const QString &mediaKey, bool ok,
                             const QByteArray &bytes, const QString &category);

private:
    // Clears to idle: bumps m_generation (invalidating any outstanding
    // mediaBytesForStar wait — see the header comment on m_generation) and
    // drops every piece of forward-in-progress state. Used by cancel(),
    // by begin() (a second begin() must discard whatever the first one was
    // waiting on), and by a successful finish.
    void resetToIdle();
    void setError(const QString &message);
    static bool snapshotIsMedia(const QVariantMap &snapshot);
    static QString buildPreview(const QVariantMap &snapshot);

    MatrixClient *m_client = nullptr;
    MediaBridge *m_mediaBridge = nullptr;

    bool m_active = false;
    bool m_busy = false;
    QString m_error;

    QString m_sourceRoomId;
    QString m_sourceEventId;
    QString m_previewText;

    // Frozen at begin() — never re-read from a live model.
    QVariantMap m_snapshot;

    // Generation counter, bumped by every resetToIdle() (i.e. every
    // begin()/cancel()/successful finish). Guards the async media fetch:
    // an mediaBytesForStar answer is honoured only when it names the exact
    // key THIS generation dispatched (m_pendingMediaKey) — a late answer
    // for a forward the user has since cancelled or replaced with a new
    // begin() is dropped instead of sending into whatever target the
    // CURRENT forward's dialog now shows.
    // ── Selection state ──────────────────────────────────────────────
    struct Pending {
        QVariantMap snapshot;
        QString eventId;
        QString targetRoomId;
        QString threadRootId;
    };
    void pumpQueue();
    void notePairResult(const Pending &pair, bool ok, const QString &message);
    QString contextPrefixFor(const QVariantMap &snapshot) const;

    bool m_selectionActive = false;
    QString m_mode = QStringLiteral("content");
    QVariantList m_selectionSnapshots;
    QList<Pending> m_queue;
    QList<Pending> m_failedPairs;
    QVariantList m_failures;
    int m_progressDone = 0;
    int m_progressTotal = 0;
    /// ONE media send at a time. A selection of twenty images into three
    /// rooms is sixty uploads, and dispatching them together is how a client
    /// saturates a link and times its own sends out.
    bool m_pumpBusy = false;

    quint64 m_generation = 0;
    quint64 m_pendingGeneration = 0;
    QString m_pendingMediaKey;
    // Media sends already handed to the SDK, by op id -> target room. They
    // survive resetToIdle(): the picker closes immediately but the send has
    // not finished, and a refusal must still be reportable. Keyed rather
    // than single-slot so a second forward cannot silence the first's.
    QHash<quint64, QString> m_dispatchedSends;
    QString m_pendingTargetRoomId;
};
