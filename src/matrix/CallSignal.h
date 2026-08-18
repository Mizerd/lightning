// One inbound call-signaling observation crossing the backend boundary
// (2026-08-18 voice-call pipes). Deliberately SDP-free: the Rust side
// forwards has_offer/has_answer booleans and sanitized closed-set strings
// only, so this struct is structurally incapable of leaking a session
// description (which carries host IPs) toward QML or logs.
#pragma once

#include <QMetaType>
#include <QString>

struct CallSignal {
    enum class Kind {
        Invite,
        Answer,
        Hangup,
        Reject,
        SelectAnswer,
        RtcNotification,
        RtcDecline,
    };

    Kind kind = Kind::Invite;
    QString roomId;
    QString eventId;
    QString sender;
    // True when the sender is the local user (possibly another device).
    bool own = false;

    // Legacy m.call.* fields. callId/partyId are SENDER-CHOSEN opaque
    // text (ruma validates nothing about a VoipId): bounded at the Rust
    // edge (length + control characters), but still never to be logged or
    // rendered — only compared. Empty when not applicable to the kind.
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
    // Hangup only: closed-set reason string.
    QString reason;
    // SelectAnswer only.
    QString selectedPartyId;
    // RtcNotification only: sanitized intent; RtcDecline only: the
    // notification event this decline targets.
    QString callIntent;
    QString targetEventId;
    qint64 senderTs = 0;
};

Q_DECLARE_METATYPE(CallSignal)
