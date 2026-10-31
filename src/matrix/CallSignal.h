// One inbound call-signaling observation crossing the backend boundary.
// Deliberately SDP-free: Rust forwards has_offer/has_answer booleans and
// sanitized closed-set strings only, so this struct cannot leak a session
// description (which carries host IPs) to QML or logs.
#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>

struct CallSignal {
    enum class Kind {
        Invite,
        Answer,
        Hangup,
        Reject,
        SelectAnswer,
        RtcNotification,
        RtcDecline,
        // Legacy, on an established call: renegotiation, and a mute changing
        // on a stream. MSC2746 / MSC3077.
        Negotiate,
        StreamMetadata,
    };

    Kind kind = Kind::Invite;
    QString roomId;
    QString eventId;
    QString sender;
    // True when the sender is the local user (possibly another device).
    bool own = false;

    // Legacy m.call.* fields. callId/partyId are sender-chosen opaque text
    // (ruma does not validate a VoipId): bounded at the Rust edge, but only
    // ever compared, never logged or rendered. Empty when not applicable.
    QString callId;
    QString partyId;
    // Invite only: the targeted user (empty = any), lifetime, server ts,
    // VoIP version ("0"/"1"/"other"), sanitized offer type, and whether a
    // non-empty SDP was present (the SDP itself never crosses).
    QString invitee;
    qint64 lifetimeMs = 0;
    qint64 originServerTs = 0;
    QString version;
    QString sessionType;
    bool hasDescription = false;
    // Invite, answer and negotiate: the description has a live m=video
    // section. Negotiate's description type is in sessionType.
    bool hasVideo = false;
    // Invite, answer, negotiate and stream metadata: the sender stated the
    // mute of its m.usermedia stream (MSC3077). Stream ids do not cross.
    bool hasStreamMetadata = false;
    bool remoteAudioMuted = false;
    bool remoteVideoMuted = false;
    // Hangup only: closed-set reason string.
    QString reason;
    // SelectAnswer only.
    QString selectedPartyId;
    // RtcNotification only: sanitized intent; RtcDecline only: the
    // notification event this decline targets.
    QString callIntent;
    QString targetEventId;
    qint64 senderTs = 0;
    // RtcNotification only: "ring" or "notification" as the sender stated it
    // (empty when absent), and who the announcement addresses, from its
    // m.mentions. In an unambiguous 1:1 DM (calls::oneToOneDmPeer: one
    // m.direct target, at most two active members) a call from the other
    // person rings whatever the type, because Lightning 0.10.x and earlier
    // announced DM calls as "notification". Elsewhere a "notification"
    // announces a call and never rings, and a "ring" rings only the users it
    // mentions (or everyone with @room). `mentionsMe` is decided in Rust
    // against the whole m.mentions list; mentionUserIds is bounded at the
    // bridge edge and here, so it can miss us. Ids are only compared, never
    // rendered or logged.
    QString notificationType;
    QStringList mentionUserIds;
    bool mentionsRoom = false;
    bool mentionsMe = false;
};

Q_DECLARE_METATYPE(CallSignal)
