import QtQuick
import QtQuick.Controls
import MatrixClient

// The shared presence indicator. Anchor it to an avatar's bottom-right, set
// `userId` and the `ring` colour of the surface behind it; watch/unwatch with
// PresenceManager, state colours and hiding live here. Unknown presence renders
// nothing: an unanswered lookup, a backend without presence and a server that
// disabled it are all the same absence, never a fabricated offline.
// `unavailable` is not consulted either; that disclosure needs a sentence and
// lives on the member profile's membership chip. The dot owns `statusText`, the
// one presence sentence, shown on hover where a surface opts in
// (`hoverStatus`).
Rectangle {
    id: dot

    // Resolved through one guarded property (as Avatar.qml's `bridge`): a
    // delegate created inside a property-change handler can see its first
    // unqualified `app` lookup resolve undefined. Must stay the first `app`
    // reference in this file; re-resolved from _sync(). Unresolved means no
    // watch and no render.
    property var presenceService:
        (typeof app !== "undefined" && app) ? app.presence : null

    function resolvePresence() {
        if (!presenceService && typeof app !== "undefined" && app)
            presenceService = app.presence
    }

    // The user shown. The dot holds one watch reference while it exists, so
    // only on-screen users are polled.
    property string userId: ""
    // The colour of the surface under the dot, drawn as its ring.
    property color ring: AppTheme.surface
    property int dotSize: 10

    // Re-evaluated on any presence change (revision); stateFor is a pure read.
    readonly property string presenceState:
        userId !== "" && presenceService && presenceService.revision >= 0
            ? presenceService.stateFor(userId) : ""

    // The presence sentence, the only one in the app. "" when unknown, which
    // renders nothing.
    readonly property string statusText: {
        if (userId === "" || !presenceService)
            return ""
        var rev = presenceService.revision   // re-evaluation dependency
        var info = presenceService.infoFor(userId)
        if (!info || info.state === undefined)
            return ""
        // The peer's status_msg follows the state: remote text, bounded and
        // control-stripped in Rust, rendered plain.
        var status = (info.statusMsg && info.statusMsg.length > 0)
                     ? " \u00b7 " + info.statusMsg : ""
        if (info.state === "online")
            return qsTr("Online") + status
        if (info.state === "unavailable")
            return qsTr("Away") + status
        if (info.state !== "offline")
            return ""
        var ago = info.lastActiveAgoMs
        if (ago === undefined || ago < 0)
            return qsTr("Offline") + status
        var mins = Math.floor(ago / 60000)
        if (mins < 1)
            return qsTr("Offline \u2014 active just now") + status
        if (mins < 60)
            return qsTr("Offline \u2014 active %1 min ago").arg(mins) + status
        var hours = Math.floor(mins / 60)
        if (hours < 24)
            return qsTr("Offline \u2014 active %1 h ago").arg(hours) + status
        return qsTr("Offline \u2014 active %1 d ago").arg(Math.floor(hours / 24)) + status
    }

    // Opt-in: a HoverHandler on every dot in a long list costs every row.
    property bool hoverStatus: false

    // The watch reference held, so a userId change releases the old user first.
    property string _watched: ""

    // Session epoch: sign-out/account switch drops the watched set, so
    // surviving dots re-register.
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
    // The outer disc is the ring separating the dot from the avatar.
    color: ring

    // The state disc. Offline is a hollow ring, online and away are solid: a
    // difference in form, readable without distinguishing hues (as Element X
    // does).
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

    // The tooltip gives readers who cannot tell the hues apart the same fact.
    HoverHandler {
        id: statusHover
        enabled: dot.hoverStatus && dot.visible
    }
    ToolTip.text: dot.statusText
    ToolTip.visible: dot.hoverStatus && statusHover.hovered
                     && dot.statusText.length > 0
    ToolTip.delay: 300
    // In the accessibility tree only where the dot is the sole carrier of
    // status.
    Accessible.role: Accessible.StaticText
    Accessible.ignored: !dot.hoverStatus || dot.statusText.length === 0
    Accessible.name: dot.statusText
}
