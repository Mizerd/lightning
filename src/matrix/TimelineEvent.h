#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

// A single emoji reaction bucket on a target event.
struct Reaction {
    QString key;             // The emoji / reaction key ("👍", etc.).
    int count = 0;           // Total distinct senders that reacted with this key.
    bool byMe = false;       // True if the current user is one of them.
    QString myEventId;       // If byMe: the event_id of our reaction, needed to redact.
    // Reactor user ids. The Rust bridge sends a bounded window (16) with the
    // local user first; `count` stays the uncapped total. Presentation resolves
    // names at read time. Empty on the mock and HTTP backends.
    QStringList senders;
};

// One user's read receipt (Rust backend): the reader's user id and the
// receipt timestamp (0 when absent). The SDK attaches each receipt to the
// latest message-like event it applies to. Thread rows always carry none:
// the thread builders disable receipt tracking because the SDK's receipt
// handling is not thread-aware and would attach room receipts to thread rows.
struct ReadReceipt {
    QString userId;
    qint64 tsMs = 0;
};

// One MSC3381 poll answer with its SDK-aggregated tally. `count` is 0 for
// undisclosed polls that have not ended; hidden tallies never cross the FFI.
// `byMe` reflects the user's latest valid response.
struct PollAnswer {
    QString id;              // Stable answer id from the poll-start event.
    QString text;
    int count = 0;
    bool byMe = false;
};

// One attachment of an MSC4274 media gallery (Rust backend). Metadata only:
// `mediaKey` addresses the source in the Rust media registry, and the source
// (with content keys in encrypted rooms) never leaves Rust. `kind` is a
// closed set enforced at ingest: "image", "video", "audio" or "file".
struct GalleryItem {
    QString mediaKey;
    QString kind;
    QString filename;
    QString mimetype;
    qint64 size = 0;
    int width = 0;
    int height = 0;
    qint64 durationMs = 0;
    bool thumbAvailable = false;
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
        // Virtual SDK timeline rows, never persisted. New kinds are appended so
        // the integer values CacheStore persisted stay stable.
        DateDivider,
        ReadMarker,
        TimelineStart,
        // Typed media rows; appended to keep persisted values stable.
        Video,
        Audio,
        Sticker,
        // MSC3381 polls (Rust backend only); appended to keep persisted values
        // stable.
        Poll,
        // A call somebody started (SDK CallInvite / RtcNotification). Its own
        // kind rather than a StateChange, so it is not folded into the
        // collapsed room-activity group. Appended to keep persisted values
        // stable.
        CallEvent,
        // A shared place: static `m.location` (MSC3488) or a live beacon
        // (MSC3672), rendered with a map link. Appended to keep persisted
        // values stable.
        Location,
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
    // What a membership state row did, as a closed set: "" (unknown) / "joined"
    // / "left" / "invited" / "kicked" / "banned" / "unbanned" / "revoked". Kept
    // separate from the sentence so presentation can pick a glyph without
    // parsing translated text. Unknown stays unknown.
    QString membershipChange;
    // m.room.member profile change (stateKind == "member_profile"), typed so
    // presentation can translate it with the actor's resolved name.
    // profileNameChange: "" (none) / "set" / "changed" / "cleared". The names
    // are bounded to 255 chars in Rust and are untrusted plain text: render as
    // PlainText, never rich text. Only the avatar fact crosses, never the mxc.
    QString profileNameChange;
    QString profileNameOld;
    QString profileNameNew;
    bool profileAvatarChanged = false;

    // Typed call row (type == CallEvent). Every field is presentation-safe and
    // closed-set: the row carries a Join control, and sender-written text must
    // never label a control the reader is invited to click.
    //   callEventKind: "invite" (legacy m.call.invite) | "notification"
    //                  (MatrixRTC m.rtc.notification), empty when unstated. Not
    //                  branched on: Join is gated on the room having a live
    //                  MatrixRTC session, as in RoomCallBanner.
    //   callIsVideo:   the caller's stated video intent. False means "not
    //                  known to be video", never "audio only".
    //   callDeclinedCount: how many people declined; a count, never the ids.
    QString callEventKind;
    bool callIsVideo = false;
    int callDeclinedCount = 0;
    QString formattedBody;
    QDateTime timestamp;
    Type type = TextMessage;
    Status status = Sent;
    bool edited = false;
    bool redacted = false;

    // Relations.
    QString threadRootId;
    QString replyToEventId;
    QString replyToSender;
    // The replied-to sender's MXID. The Rust backend resolves the display name
    // at the source and sends the id separately so the quote uses the same
    // identity colour as the header. Backends that only know the id leave this
    // empty and put the id in replyToSender.
    QString replyToSenderId;
    QString replyToPreview;    // Short preview of the replied-to body, best-effort.
    // Media-bridge key for an image reply target (empty otherwise), registered
    // in the Rust registry like a row's own media.
    QString replyToMediaKey;
    // What the replied-to event is when there are no words to quote ("image",
    // "gif", "video", "audio", "file", "sticker", "poll", "text" …, as in
    // threadLatestKind), and its attachment count for a gallery (0 otherwise),
    // so the quote can say "Image" or "2 images".
    QString replyToKind;
    int replyToCount = 0;

    // SDK thread summary on thread roots. The reply count is the server's
    // bundled aggregation; -1 means none and the model counts loaded replies
    // instead (mock/HTTP). threadUnread is a conservative receipt-based hint,
    // never an exact count.
    bool isThreadRoot = false;
    int threadReplyCount = -1;
    QString threadLatestPreview;
    QString threadLatestKind;   // text/notice/image/gif/file/redacted/encrypted…
    QString threadLatestSender; // MXID of the latest reply's sender
    QString threadLatestSenderDisplayName;
    QString threadLatestSenderAvatarUrl; // mxc:// only, via the safe media path
    QDateTime threadLatestTimestamp;
    bool threadUnread = false;

    // Mention metadata from the event's m.mentions (SDK-parsed), never derived
    // by substring matching.
    bool mentionsMe = false;
    bool mentionsRoom = false;

    // Media. Non-empty only for media rows (Image/File/Video/Audio/Sticker).
    QString mediaMxcUrl;
    QString mediaMimetype;
    QString mediaFilename;
    qint64  mediaSize = 0;
    int     mediaWidth = 0;
    int     mediaHeight = 0;
    QString mediaThumbnailMxcUrl;
    // Duration (audio/video) and the MSC3245 voice-message marker from Matrix
    // `info` metadata, so the UI can reserve geometry before bytes arrive.
    qint64  mediaDurationMs = 0;
    bool    mediaIsVoice = false;
    // MSC3245 waveform (bridge-normalized 0..=100, at most 96 buckets). Empty
    // when absent; the UI then shows a plain progress track, never a fabricated
    // waveform.
    QList<int> mediaWaveform;

    // Media bridge (Rust backend only). `mediaKey` identifies the item for
    // MatrixClient::fetchMedia; the actual source (which embeds content keys in
    // encrypted rooms) never leaves Rust. The flags say whether bytes and a
    // server thumbnail are available.
    QString mediaKey;
    bool    mediaSourceAvailable = false;
    bool    mediaThumbAvailable = false;
    // An MSC4274 gallery (two or more attachments in one event). The media
    // fields above describe the primary item so single-attachment surfaces stay
    // truthful; this lists every item, primary included, in sender order. Empty
    // for other rows; a one-item gallery is a single attachment.
    QList<GalleryItem> galleryItems;

    // SDK-reported display-name ambiguity (two active members share the name).
    // The UI appends a compact MXID; identity is always the user id.
    bool    senderNameAmbiguous = false;

    // Reactions attached to this event.
    QList<Reaction> reactions;

    // Receipts of every user whose receipt points at this event, including the
    // local user. TimelineModel excludes the local user and the sender; the
    // mirror stays faithful to the SDK. The Rust bridge sends a bounded
    // newest-first window (16); readByTotal keeps the uncapped count.
    QList<ReadReceipt> readBy;
    // Total receipts before the FFI cap (>= readBy.size()). The ingest clamps
    // it to the delivered list size, so rows that never set it stay consistent.
    int readByTotal = 0;

    // MSC3381 poll presentation (type == Poll, Rust backend only). Aggregation
    // is SDK/ruma-owned; these fields carry only the outcome.
    QString pollQuestion;
    QString pollKind;            // "disclosed" | "undisclosed"
    int pollMaxSelections = 1;
    int pollTotalVoters = 0;
    bool pollEnded = false;
    QList<PollAnswer> pollAnswers;

    // A shared place (type == Location).
    //
    // `locationHasPoint` is load-bearing: a geo URI that does not parse, or
    // names a point not on Earth, leaves the coordinates unset rather than 0,0
    // (a real spot in the Atlantic). False means render the body and no map
    // link.
    bool locationHasPoint = false;
    double locationLat = 0.0;
    double locationLon = 0.0;
    /// The sender's stated accuracy in metres, 0 when they gave none.
    double locationUncertaintyM = 0.0;
    QString locationDescription;
    /// "m.self" (the sender's own position) or "m.pin" (a place they point at).
    QString locationAsset;
    /// A live share (MSC3672) rather than a single point.
    bool locationLive = false;
    /// Whether the live share is still current (the SDK checks the flag and
    /// `ts + timeout`); an expired share must not be shown as live.
    bool locationLiveActive = false;

    // Encryption flags, populated by the Rust backend; HTTP and Mock leave the
    // defaults. Metadata only; never derive plaintext from them:
    //   isEncrypted   - the event was m.room.encrypted or decrypted from one.
    //   isDecrypted   - the SDK produced usable plaintext for `body`; implies
    //                   isEncrypted.
    //   undecryptable - the SDK could not decrypt it. Body is the localised
    //                   placeholder; ciphertext never crosses the FFI.
    //   errorKind     - SDK hint: "no_key", "session_missing", or empty. Never
    //                   crypto material.
    bool    isEncrypted = false;
    bool    isDecrypted = false;
    bool    undecryptable = false;
    QString errorKind;

    // Live SDK timeline metadata (Rust matrix-sdk-ui path only).
    //   itemId            - the SDK's stable timeline-item id, the row identity
    //                       for diffs; survives echo reconciliation and late
    //                       decryption.
    //   transactionId     - send-queue id while a local echo; for
    //                       retryFailedSend.
    //   isLocalEcho       - true until the remote echo is reconciled.
    //   sendErrorCategory - coarse category ("network", "rejected") when
    //                       status == Failed.
    //   uploadedBytes /   - media-upload progress while Sending, from the SDK
    //   uploadTotalBytes    send queue. Both zero means the total is unknown (a
    //                       spinner, never a 0% bar).
    QString itemId;
    QString transactionId;
    bool    isLocalEcho = false;
    QString sendErrorCategory;
    qint64  uploadedBytes = 0;
    qint64  uploadTotalBytes = 0;

    bool isVirtual() const
    {
        return type == DateDivider || type == ReadMarker || type == TimelineStart;
    }
};

// All members are implicitly shared Qt types or trivial, so these structs
// are relocatable; otherwise QList moves them through the copy constructor on
// every insert/prepend (measurable on pagination).
Q_DECLARE_TYPEINFO(Reaction, Q_RELOCATABLE_TYPE);
Q_DECLARE_TYPEINFO(ReadReceipt, Q_RELOCATABLE_TYPE);
Q_DECLARE_TYPEINFO(PollAnswer, Q_RELOCATABLE_TYPE);
Q_DECLARE_TYPEINFO(TimelineEvent, Q_RELOCATABLE_TYPE);
