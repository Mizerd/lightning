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
    border.color: ring
    border.width: 2
    color: presenceState === "online" ? AppTheme.presenceOnline
         : presenceState === "unavailable" ? AppTheme.presenceAway
         : AppTheme.presenceOffline
}
