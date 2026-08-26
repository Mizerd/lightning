// MatrixRTC session facts crossing the backend boundary.
//
// Lives under matrix/ beside CallSignal.h for the same reason: this is a
// BACKEND value type that MatrixClient's signals carry, not a piece of the
// calls/ subsystem. Putting it under calls/ made the Matrix bridge header
// depend on calls/, inverting the ownership.
//
// These are OBSERVATIONS of a room's call: who the homeserver says is in it,
// and whether a media transport is reachable. They deliberately carry no
// media state — an observed membership tells you a device joined, never
// whether its microphone is live. Media state arrives from the SFU, which is
// a separate layer, so every media field on RtcParticipant stays at its
// "unknown" default until something authoritative fills it.
//
// Everything here originated as remote JSON and was bounded and sanitized in
// rust/src/rtc.rs. `intent` is a closed set. `rtcIdentity`, `deviceId` and
// the transport URL are opaque: compared, never logged, never rendered.
#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>

/// One participant device in a MatrixRTC session.
///
/// The unit is a DEVICE, not a user: the same person joining from a laptop
/// and a phone is two participants, which is what every other client shows
/// and what the SFU sees.
struct RtcParticipant {
    QString userId;
    QString deviceId;
    /// Identity this device uses on the SFU. Derived in Rust from the
    /// membership; used to match a media track to a Matrix device.
    QString rtcIdentity;
    /// "audio" or "video" — the sender's declared intent, already collapsed
    /// to that closed set. NOT a statement that a camera is on.
    QString intent;
    /// Room-resolved profile. A membership state event carries no profile,
    /// so these come from the room's member state; empty means "not known
    /// here", which the UI degrades to initials rather than inventing.
    QString displayName;
    QString avatarMxc;
    /// When this device joined, ms since epoch. Drives "oldest membership"
    /// ordering, so it is meaningful even before any media exists.
    qint64 joinedAtMs = 0;
    /// Absolute membership expiry, ms since epoch.
    qint64 expiresAtMs = 0;
    /// Wire format the membership was read from ("session" / "rtc").
    /// Diagnostics only — never shown in normal UI.
    QString wireFormat;
    /// The `m.call.member` STATE EVENT that declared this membership.
    ///
    /// element-call addresses a raised hand to it — the hand is an
    /// `m.reaction` annotating the raiser's own membership event — so this is
    /// the only thing that ties one to a participant. Opaque: compared
    /// against ids we already hold, never rendered, never logged.
    ///
    /// Empty is a real answer (a membership read from a source with no
    /// envelope), and it reads as "no hand can be matched to this device",
    /// never as a match.
    QString membershipEventId;
    /// True when this is one of the local user's own devices.
    bool ownUser = false;
    /// True when this is specifically THIS device.
    bool ownDevice = false;
};

/// The observed state of one room's MatrixRTC session.
struct RtcSessionData {
    QString roomId;
    QVector<RtcParticipant> participants;

    /// A `org.matrix.msc4143.rtc.slot` state event existed.
    bool slotPresent = false;
    /// ...and it said the session is closed. Absence is NOT closed: almost
    /// no deployment publishes a slot, so treating a missing one as closed
    /// would hide every real call.
    bool slotClosed = false;

    /// The focus the participants advertise, when any do. Empty is a real
    /// answer: a call can be in progress that this client has no route into.
    QString focusServiceUrl;

    /// Server clock, ms. Used so a stale reply cannot overwrite a newer one.
    qint64 observedAtMs = 0;

    /// A session is "live" when somebody is in it and nothing closed it.
    bool live() const { return !participants.isEmpty() && !slotClosed; }
};

Q_DECLARE_METATYPE(RtcParticipant)
Q_DECLARE_METATYPE(RtcSessionData)
