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

    // Media (v0.3). Non-empty only for Image/File events.
    QString mediaMxcUrl;
    QString mediaMimetype;
    QString mediaFilename;
    qint64  mediaSize = 0;
    int     mediaWidth = 0;
    int     mediaHeight = 0;
    QString mediaThumbnailMxcUrl;

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
};
