#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

// A single emoji reaction bucket on a target event.
struct Reaction {
    QString key;             // The emoji / reaction key ("👍", etc.).
    int count = 0;           // Total distinct senders that reacted with this key.
    bool byMe = false;       // True if the current user is one of them.
    QString myEventId;       // If byMe: the event_id of our reaction, needed to redact.
};

struct TimelineEvent {
    enum Type {
        TextMessage,
        Emote,
        Notice,
        Image,
        File,
        StateChange,
        Unknown,
        // v0.5.7: virtual SDK timeline rows. Appended after Unknown so the
        // integer values CacheStore persisted for the HTTP backend stay
        // stable. Virtual rows are never persisted.
        DateDivider,
        ReadMarker,
        TimelineStart,
    };

    enum Status {
        Sent,
        Sending,
        Failed,
    };

    QString eventId;
    QString roomId;
    QString sender;
    QString senderDisplayName;
    QString senderAvatarUrl;
    QString body;
    QString stateKind;
    // Typed room-activity target (for example the affected member's display
    // name or MXID). State presentation never needs raw event JSON.
    QString stateTarget;
    QString formattedBody;
    QDateTime timestamp;
    Type type = TextMessage;
    Status status = Sent;
    bool edited = false;
    bool redacted = false;

    // Relations (v0.3).
    QString threadRootId;
    QString replyToEventId;
    QString replyToSender;
    QString replyToPreview;    // Short preview of the replied-to body, best-effort.

    // v0.6.0: SDK-provided thread summary on thread ROOT events. The reply
    // count is the server's bundled aggregation (authoritative, kept live by
    // sync); -1 means "no SDK summary" and the model falls back to counting
    // locally loaded replies (mock/HTTP backends). threadUnread is a
    // conservative receipt-based hint, never an exact count.
    bool isThreadRoot = false;
    int threadReplyCount = -1;
    QString threadLatestPreview;
    QString threadLatestKind;   // text/notice/image/gif/file/redacted/encrypted…
    QString threadLatestSender; // MXID of the latest reply's sender
    QString threadLatestSenderDisplayName;
    QString threadLatestSenderAvatarUrl; // mxc:// only, via the safe media path
    QDateTime threadLatestTimestamp;
    bool threadUnread = false;

    // v0.6.0 checkpoint 11: authoritative mention metadata from the event's
    // m.mentions (SDK-parsed) — never derived by substring matching.
    bool mentionsMe = false;
    bool mentionsRoom = false;

    // Media (v0.3). Non-empty only for Image/File events.
    QString mediaMxcUrl;
    QString mediaMimetype;
    QString mediaFilename;
    qint64  mediaSize = 0;
    int     mediaWidth = 0;
    int     mediaHeight = 0;
    QString mediaThumbnailMxcUrl;

    // Media bridge (v0.5.9, Rust backend only). `mediaKey` identifies the
    // item's media for MatrixClient::fetchMedia; the actual source (which
    // for encrypted rooms embeds content keys) never leaves Rust. The flags
    // tell the UI whether bytes can be fetched and whether a server-side
    // thumbnail exists.
    QString mediaKey;
    bool    mediaSourceAvailable = false;
    bool    mediaThumbAvailable = false;

    // v0.5.9: SDK-reported display-name ambiguity for the sender (two
    // active members share the name). UI appends a compact MXID
    // disambiguator; identity always remains the user id.
    bool    senderNameAmbiguous = false;

    // Reactions attached to this event (v0.3).
    QList<Reaction> reactions;

    // Encryption flags (v0.5.0-prep+6). Populated by the Rust backend
    // when it parses events out of the Matrix Rust SDK; HTTP and Mock
    // leave everything at its default (all false / empty). The C++ UI
    // must never derive plaintext from these flags — they carry only
    // metadata:
    //   isEncrypted  — the on-wire event was m.room.encrypted or the
    //                  SDK decrypted it from one.
    //   isDecrypted  — the SDK produced usable plaintext for `body`.
    //                  Implies isEncrypted == true.
    //   undecryptable — the SDK could not decrypt this event. Body is
    //                   the localised placeholder; original ciphertext
    //                   is deliberately NOT forwarded through the FFI.
    //   errorKind    — best-effort hint from the SDK: "no_key",
    //                  "session_missing", or empty. Never contains
    //                  crypto material.
    bool    isEncrypted = false;
    bool    isDecrypted = false;
    bool    undecryptable = false;
    QString errorKind;

    // Live SDK timeline metadata (v0.5.7). Populated only by the Rust
    // backend's matrix-sdk-ui timeline path.
    //   itemId            — the SDK's stable unique timeline-item id. The
    //                       authoritative row identity for diff application;
    //                       survives local-echo → remote reconciliation and
    //                       undecryptable → decrypted replacement.
    //   transactionId     — send-queue transaction id while the row is a
    //                       local echo; used for retryFailedSend.
    //   isLocalEcho       — true until the SDK reconciles the remote echo.
    //   sendErrorCategory — coarse non-secret category ("network",
    //                       "rejected") when status == Failed.
    QString itemId;
    QString transactionId;
    bool    isLocalEcho = false;
    QString sendErrorCategory;

    bool isVirtual() const
    {
        return type == DateDivider || type == ReadMarker || type == TimelineStart;
    }
};
