import QtQuick
import MatrixClient

// v0.7.x Matrix presence: the one shared presence indicator. Anchor it to
// an avatar's bottom-right corner, set `userId` and the `ring` colour of
// the surface it sits on — everything else (watch/unwatch bookkeeping with
// PresenceManager, state → colour, hiding when unknown) lives here so no
// surface can get the lifecycle wrong.
//
// Unknown presence renders NOTHING (visible: false): an unanswered lookup,
// a backend without presence and a server that disabled presence are all
// the same honest absence, never a fabricated offline.
//
// That includes the case PresenceManager can name — `unavailable`, i.e. an
// unsupported backend or a server that refused presence for everyone this
// session. This component deliberately does NOT consult it: a dot has no
// room for prose, so the only thing it could do with that knowledge is
// paint a fourth colour, which is a fabricated indicator by another name.
// The disclosure belongs where there is space for a sentence — the member
// profile popover.
Rectangle {
    id: dot

    // Poisoned-context inoculation (review M4; the 30ee39b precedent, same
    // shape as Avatar.qml's `bridge` guard): when a view creates this
    // delegate synchronously from inside a property-change handler, the
    // FIRST unqualified `app` context lookup of that nested creation can
    // resolve undefined. So the service is resolved through ONE guarded
    // property — which must stay the first `app` reference in this file —
    // and re-resolved idempotently from _sync(). While unresolved the dot
    // is honestly absent: no watch, no render, no warning.
    property var presenceService:
        (typeof app !== "undefined" && app) ? app.presence : null

    function resolvePresence() {
        if (!presenceService && typeof app !== "undefined" && app)
            presenceService = app.presence
    }

    // The user whose presence this dot shows. While the dot exists it
    // holds one watch reference on PresenceManager, so only on-screen
    // users are ever polled.
    property string userId: ""
    // The colour of the surface the dot overlaps (drawn as its ring, the
    // SpacesRail self-dot idiom).
    property color ring: AppTheme.surface
    property int dotSize: 10

    // Re-evaluated whenever any cached presence changes (revision bump);
    // stateFor is a pure read and safe in a binding.
    readonly property string presenceState:
        userId !== "" && presenceService && presenceService.revision >= 0
            ? presenceService.stateFor(userId) : ""

    // The watch reference currently held, so a userId change (delegate
    // reuse) releases the old user before claiming the new one.
    property string _watched: ""

    // Session epoch: sign-out/account switch drops the manager's watched
    // set (one account's contacts must never be polled against the next
    // account's server), so every surviving dot re-registers on the bump.
    readonly property int _epoch:
        presenceService ? presenceService.sessionEpoch : 0
    on_EpochChanged: {
        _watched = ""
        _sync()
    }

    function _sync() {
        resolvePresence()
        if (_watched === userId)
            return;
        if (_watched !== "" && presenceService)
            presenceService.unwatch(_watched);
        _watched = "";
        if (userId !== "" && presenceService) {
            presenceService.watch(userId);
            _watched = userId;
        }
    }

    onUserIdChanged: _sync()
    Component.onCompleted: _sync()
    Component.onDestruction: {
        if (_watched !== "" && presenceService)
            presenceService.unwatch(_watched);
        _watched = "";
    }

    visible: presenceState !== ""
    width: dotSize
    height: dotSize
    radius: dotSize / 2
    // The outer disc is the RING that separates the dot from the avatar
    // underneath — it was drawn as a 2px border, which meant the state
    // colour and the ring shared one Rectangle and the state could only ever
    // be a solid fill.
    color: ring

    // The state disc. Same painted area as before (dotSize - 2*2), so
    // online and away are pixel-identical; offline becomes a RING instead of
    // a solid dot. Three presence states rendered as three solid dots of
    // near-equal weight meant "offline" attracted as much of the eye as
    // "online" — and told apart by hue alone, which a red/green-blind reader
    // cannot do at 6px. Hollow vs solid is a difference in FORM, and it
    // matches how Element X draws the same three states.
    Rectangle {
        anchors.fill: parent
        anchors.margins: 2
        radius: width / 2
        readonly property bool _offline:
            dot.presenceState !== "online"
            && dot.presenceState !== "unavailable"
        color: dot.presenceState === "online" ? AppTheme.presenceOnline
             : dot.presenceState === "unavailable" ? AppTheme.presenceAway
                                                   : "transparent"
        // Whole pixels: a fractional border renders soft at 1.0 DPR.
        border.width: _offline ? Math.max(1, Math.round(dot.dotSize * 0.2)) : 0
        border.color: AppTheme.presenceOffline
    }
}
