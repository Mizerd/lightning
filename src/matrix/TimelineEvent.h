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
    // Who reacted, as stable user ids — the Rust bridge sends a bounded
    // window (16, the read-receipt cap) with the local user first, while
    // `count` above stays the UNCAPPED total. Presentation resolves these
    // to room display names at role-read time; the ids themselves are what
    // the SDK reported, never a name the bridge guessed. Empty on the mock
    // and HTTP backends, which report no reactor identities.
    QStringList senders;
};

// One user's read receipt on the event (Rust backend, SDK receipt
// tracking). Carries ONLY the public receipt metadata the server already
// shares: the reader's stable user id and the receipt timestamp (0 when
// the receipt carried none). The SDK attaches each user's receipt to the
// LATEST message-like event it applies to, so receipts advance between
// rows via ordinary Set diffs. Thread-timeline rows always carry an empty
// list: Lightning's thread builders deliberately leave receipt tracking
// Disabled, because the SDK's receipt handling is not thread-aware and
// enabling it there would attach the room's unthreaded receipts to thread
// rows (wrong data, not merely missing data).
struct ReadReceipt {
    QString userId;
    qint64 tsMs = 0;
};

// One MSC3381 poll answer with its SDK-aggregated tally (v0.7). `count` is
// 0 for undisclosed polls that have not ended — hidden tallies never cross
// the FFI. `byMe` reflects the user's latest valid response.
struct PollAnswer {
    QString id;              // Stable answer id from the poll-start event.
    QString text;
    int count = 0;
    bool byMe = false;
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
        // v0.7: typed media rows (previously collapsed into File/Unknown).
        // Appended last so persisted integer values stay stable.
        Video,
        Audio,
        Sticker,
        // v0.7: MSC3381 polls (Rust backend only). Appended last so
        // persisted integer values stay stable.
        Poll,
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
    // m.room.member profile change (stateKind == "member_profile"), typed so
    // the presentation layer can translate it instead of receiving an
    // English sentence built in the bridge — which could neither be
    // translated nor use the actor's resolved display name.
    // profileNameChange: "" (no name change) / "set" / "changed" /
    // "cleared". The old/new names are bounded at 255 chars in Rust and are
    // UNTRUSTED plain text: render them as PlainText, never as rich text.
    // Only the avatar FACT crosses — never the mxc URI.
    QString profileNameChange;
    QString profileNameOld;
    QString profileNameNew;
    bool profileAvatarChanged = false;
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
    // 2026-08-18: media-bridge key for an IMAGE reply target (empty
    // otherwise) — the embedded event's media is registered in the Rust
    // registry under this key, exactly like a row's own media.
    QString replyToMediaKey;

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

    // Media (v0.3). Non-empty only for media events (Image/File and the
    // v0.7 typed Video/Audio/Sticker rows).
    QString mediaMxcUrl;
    QString mediaMimetype;
    QString mediaFilename;
    qint64  mediaSize = 0;
    int     mediaWidth = 0;
    int     mediaHeight = 0;
    QString mediaThumbnailMxcUrl;
    // v0.7: duration (audio/video) and the MSC3245 voice-message marker,
    // straight from Matrix `info` metadata — the UI reserves type-correct
    // geometry before any bytes arrive.
    qint64  mediaDurationMs = 0;
    bool    mediaIsVoice = false;
    // v0.7: real MSC3245 waveform envelope (bridge-normalized 0..=100,
    // at most 96 buckets). Empty when the event carried none — the UI
    // then shows a plain progress track, never a fabricated waveform.
    QList<int> mediaWaveform;

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

    // Read receipts of every user whose receipt points at this event,
    // including the local user (see struct ReadReceipt). Presentation
    // excludes the local user and the row's sender (Element convention) in
    // TimelineModel, not here — the mirror stays a faithful copy of what
    // the SDK reported. The Rust bridge sends a bounded newest-first
    // window (16 entries); readByTotal below keeps the uncapped count.
    QList<ReadReceipt> readBy;
    // Total receipts the SDK reported for this event BEFORE the FFI cap
    // (>= readBy.size()). The ingest clamps it to at least the delivered
    // list size, so mock/HTTP rows that never set it stay consistent.
    int readByTotal = 0;

    // v0.7: MSC3381 poll presentation (type == Poll, Rust backend only).
    // Aggregation is SDK/ruma-owned; these fields carry only the outcome.
    QString pollQuestion;
    QString pollKind;            // "disclosed" | "undisclosed"
    int pollMaxSelections = 1;
    int pollTotalVoters = 0;
    bool pollEnded = false;
    QList<PollAnswer> pollAnswers;

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
    //   uploadedBytes /   — real media-upload progress while status ==
    //   uploadTotalBytes    Sending, from the SDK send queue's own
    //                       MediaUpload reports. BOTH ZERO means the total
    //                       is not known: a text send has no upload at all,
    //                       and the first diff of a media send can arrive
    //                       before the first progress report. That is a
    //                       spinner, never a 0% bar — the presentation side
    //                       must not invent a denominator.
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

// Every member is an implicitly-shared Qt value type (or trivial), so a
// TimelineEvent can be moved by memcpy. Without this, QList treats the
// struct as non-relocatable and every insert/prepend/removeAt shifts
// elements through the full copy-constructor chain (~30 QString refcount
// round trips each) — measurable on every pagination prepend.
Q_DECLARE_TYPEINFO(Reaction, Q_RELOCATABLE_TYPE);
Q_DECLARE_TYPEINFO(ReadReceipt, Q_RELOCATABLE_TYPE);
Q_DECLARE_TYPEINFO(PollAnswer, Q_RELOCATABLE_TYPE);
Q_DECLARE_TYPEINFO(TimelineEvent, Q_RELOCATABLE_TYPE);
