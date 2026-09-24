// MatrixRTC session facts crossing the backend boundary. A backend value type
// carried by MatrixClient's signals (like CallSignal.h), so it lives under
// matrix/ rather than calls/.
//
// These are observations: who the homeserver says is in a room's call and
// whether a transport is reachable. No media state: a membership never says
// whether a microphone is live; that comes from the SFU, so media fields on
// RtcParticipant stay "unknown" until something authoritative fills them.
//
// Everything here was bounded and sanitized in rust/src/rtc.rs. `intent` is a
// closed set; `rtcIdentity`, `deviceId` and the transport URL are opaque:
// compared, never logged or rendered.
#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>

/// One participant device in a MatrixRTC session. The unit is a device, not a
/// user, as in other clients and on the SFU.
struct RtcParticipant {
    QString userId;
    QString deviceId;
    /// Identity this device uses on the SFU. Derived in Rust from the
    /// membership; used to match a media track to a Matrix device.
    QString rtcIdentity;
    /// "audio" or "video": the declared intent, collapsed to that closed set.
    /// Not a statement that a camera is on.
    QString intent;
    /// Room-resolved profile (a membership event carries none). Empty means not
    /// known here; the UI falls back to initials.
    QString displayName;
    QString avatarMxc;
    /// When this device joined (ms since epoch). Drives "oldest membership"
    /// ordering.
    qint64 joinedAtMs = 0;
    /// Absolute membership expiry, ms since epoch.
    qint64 expiresAtMs = 0;
    /// Wire format the membership was read from ("session" / "rtc").
    /// Diagnostics only — never shown in normal UI.
    QString wireFormat;
    /// The `m.call.member` state event that declared this membership.
    /// element-call's raised hand is an `m.reaction` annotating it, so this
    /// ties a hand to a participant. Opaque: compared, never rendered or
    /// logged. Empty means no hand can be matched to this device.
    QString membershipEventId;
    /// True when this is one of the local user's own devices.
    bool ownUser = false;
    /// True when this is specifically this device.
    bool ownDevice = false;
};

/// The observed state of one room's MatrixRTC session.
struct RtcSessionData {
    QString roomId;
    QVector<RtcParticipant> participants;

    /// A `org.matrix.msc4143.rtc.slot` state event existed.
    bool slotPresent = false;
    /// ...and it said the session is closed. Absence is not closed: almost no
    /// deployment publishes a slot.
    bool slotClosed = false;

    /// The focus the participants advertise, if any. Empty is a real answer: a
    /// call may be running that this client has no route into.
    QString focusServiceUrl;

    /// Server clock, ms, so a stale reply cannot overwrite a newer one.
    qint64 observedAtMs = 0;

    /// Where the memberships came from: "store", "server", "server-none"
    /// (asked; no membership state at all) or "store-fallback" (the request
    /// failed). Diagnostic only; tells a stale local store from a room with
    /// nobody in it.
    QString source;
    /// How many raw membership state events the read considered, before
    /// parsing, expiry and dedup.
    int rawMembershipEvents = 0;

    /// A session is "live" when somebody is in it and nothing closed it.
    bool live() const { return !participants.isEmpty() && !slotClosed; }
};

Q_DECLARE_METATYPE(RtcParticipant)
Q_DECLARE_METATYPE(RtcSessionData)
