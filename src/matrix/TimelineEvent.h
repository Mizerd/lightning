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
};
