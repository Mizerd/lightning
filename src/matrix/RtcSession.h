#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>

struct RtcParticipant {
    QString userId;
    QString deviceId;
    QString rtcIdentity;
    QString hashedRtcIdentity;
    QString focusServiceUrl;
    QString focusSelection;
    QString intent;
    QString displayName;
    QString avatarMxc;
    qint64 joinedAtMs = 0;
    qint64 expiresAtMs = 0;
    QString wireFormat;
    /// The `m.call.member` state event that declared this membership.
    /// element-call's raised hand is an `m.reaction` annotating it, so this
    /// ties a hand to a participant. Opaque: compared, never rendered or
    /// logged. Empty means no hand can be matched to this device.
    QString membershipEventId;
    bool ownUser = false;
    bool ownDevice = false;
};

struct RtcSessionData {
    QString roomId;
    QVector<RtcParticipant> participants;

    bool slotPresent = false;
    /// ...and it said the session is closed. Absence is not closed: almost no
    /// deployment publishes a slot.
    bool slotClosed = false;

    QString focusServiceUrl;

    qint64 observedAtMs = 0;

    QString source;
    int rawMembershipEvents = 0;

    bool live() const { return !participants.isEmpty() && !slotClosed; }
};

Q_DECLARE_METATYPE(RtcParticipant)
Q_DECLARE_METATYPE(RtcSessionData)
