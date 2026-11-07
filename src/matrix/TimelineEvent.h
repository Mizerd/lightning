#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

struct Reaction {
    QString key;
    int count = 0;
    bool byMe = false;
    QString myEventId;
    // Reactor user ids. The Rust bridge sends a bounded window (16) with the
    // local user first; `count` stays the uncapped total. Presentation resolves
    // names at read time. Empty on the mock and HTTP backends.
    QStringList senders;
};

struct ReadReceipt {
    QString userId;
    qint64 tsMs = 0;
};

struct PollAnswer {
    QString id;
    QString text;
    int count = 0;
    bool byMe = false;
};

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
        Video,
        Audio,
        Sticker,
        Poll,
        CallEvent,
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
    QString stateSentence;
    QString stateTarget;
    QString membershipChange;
    QString profileNameChange;
    QString profileNameOld;
    QString profileNameNew;
    bool profileAvatarChanged = false;

    QString callEventKind;
    bool callIsVideo = false;
    int callDeclinedCount = 0;
    QString formattedBody;
    QDateTime timestamp;
    Type type = TextMessage;
    Status status = Sent;
    bool edited = false;
    bool redacted = false;

    QString threadRootId;
    QString replyToEventId;
    QString replyToSender;
    QString replyToSenderId;
    QString replyToPreview;
    QString replyToMediaKey;
    QString replyToKind;
    int replyToCount = 0;

    // SDK thread summary on thread roots. The reply count is the server's
    // bundled aggregation; -1 means none and the model counts loaded replies
    // instead (mock/HTTP). threadUnread is a conservative receipt-based hint,
    // never an exact count.
    bool isThreadRoot = false;
    int threadReplyCount = -1;
    QString threadLatestPreview;
    QString threadLatestKind;
    QString threadLatestSender;
    QString threadLatestSenderDisplayName;
    QString threadLatestSenderAvatarUrl;
    QDateTime threadLatestTimestamp;
    bool threadUnread = false;

    bool mentionsMe = false;
    bool mentionsRoom = false;
    bool roomMentionHighlighted = false;

    enum class PushVerdict : quint8 { Unknown, Quiet, Notify };
    PushVerdict pushVerdict = PushVerdict::Unknown;
    bool pushHighlight = false;
    bool pushSound = false;
    bool backlog = false;

    bool isEdit = false;
    QString replacesEventId;
    QString editNewBody;
    bool editNewBodyAvailable = false;

    QString mediaMxcUrl;
    QString mediaMimetype;
    QString mediaFilename;
    qint64  mediaSize = 0;
    int     mediaWidth = 0;
    int     mediaHeight = 0;
    QString mediaThumbnailMxcUrl;
    qint64  mediaDurationMs = 0;
    bool    mediaIsVoice = false;
    QList<int> mediaWaveform;

    // Media bridge (Rust backend only). `mediaKey` identifies the item for
    // MatrixClient::fetchMedia; the actual source (which embeds content keys in
    // encrypted rooms) never leaves Rust. The flags say whether bytes and a
    // server thumbnail are available.
    QString mediaKey;
    bool    mediaSourceAvailable = false;
    bool    mediaThumbAvailable = false;
    // MSC4193: shown covered until the reader reveals it.
    bool    mediaSpoiler = false;
    QString mediaSpoilerReason;
    QList<GalleryItem> galleryItems;

    bool    senderNameAmbiguous = false;

    QList<Reaction> reactions;

    QList<ReadReceipt> readBy;
    int readByTotal = 0;

    QString pollQuestion;
    QString pollKind;
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
    double locationUncertaintyM = 0.0;
    QString locationDescription;
    QString locationAsset;
    bool locationLive = false;
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

Q_DECLARE_TYPEINFO(Reaction, Q_RELOCATABLE_TYPE);
Q_DECLARE_TYPEINFO(ReadReceipt, Q_RELOCATABLE_TYPE);
Q_DECLARE_TYPEINFO(PollAnswer, Q_RELOCATABLE_TYPE);
Q_DECLARE_TYPEINFO(TimelineEvent, Q_RELOCATABLE_TYPE);
