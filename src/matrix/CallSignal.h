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
        Negotiate,
        StreamMetadata,
    };

    Kind kind = Kind::Invite;
    QString roomId;
    QString eventId;
    QString sender;
    bool own = false;

    QString callId;
    QString partyId;
    QString invitee;
    qint64 lifetimeMs = 0;
    qint64 originServerTs = 0;
    QString version;
    QString sessionType;
    bool hasDescription = false;
    bool hasVideo = false;
    bool hasStreamMetadata = false;
    bool remoteAudioMuted = false;
    bool remoteVideoMuted = false;
    QString reason;
    QString selectedPartyId;
    QString callIntent;
    QString targetEventId;
    qint64 senderTs = 0;
    QString notificationType;
    QStringList mentionUserIds;
    bool mentionsRoom = false;
    bool mentionsMe = false;
};

Q_DECLARE_METATYPE(CallSignal)
