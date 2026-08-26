import QtQuick
import QtQuick.Controls
import MatrixClient

// "There is a call in this room", drawn on a room-list row.
//
// ONE component for BOTH layouts. The rule — when is the glyph shown, what
// does it mean, and what may it cost — is identical in Classic and Channels,
// and a second copy is how the two lists end up disagreeing about whether a
// call is live.
//
// WHAT IT MAY NOT DO: ask. `RtcController::refresh()` re-reads one room's
// session, and `read_membership_events` falls back to a full `/state` request
// whenever the store holds no live membership — which is the normal state of
// every idle room. A row that refreshed itself would therefore issue one
// `/state` per room in the list, on every rebuild. So this reads only what the
// controller ALREADY knows and re-reads it when the controller says that room
// changed. The sync loop's global `m.call.member` handler is what fills that
// in: a call starting, ending or changing anywhere pokes its room.
//
// The honest consequence, and it is deliberate: a call that was already
// running before this client synced, in a room nothing has poked since, shows
// nothing. `participantCount`'s own contract already says so — "0 means no
// call (or none observed yet)" — and a wrong icon is worse than a late one.
Item {
    id: root

    property string roomId: ""
    /// The glyph's ink. The caller owns it because a selected row, a hovered
    /// row and a muted row all read differently in the two layouts.
    property color color: AppTheme.textSecondary
    property int glyphSize: 14

    // Guarded: this component is loaded by fixtures that supply no call
    // controller at all, where it degrades to "no live session" — the honest
    // answer when nothing can be asked.
    readonly property bool rtcReachable:
        typeof app !== "undefined" && app && app.rtc && root.roomId.length > 0

    // Bump the counter the binding READS; never assign over the binding.
    // Same discipline as RoomCallBanner and CallEventDelegate.
    property int refreshTick: 0

    readonly property bool live: {
        var _ = root.refreshTick;
        return root.rtcReachable && app.rtc.hasLiveSession(root.roomId);
    }
    /// This device is in that call. Drawn in the accent so a row you are
    /// CONNECTED to is not the same picture as a row somebody else is talking
    /// in — those are different facts and the second one is a suggestion.
    readonly property bool joinedHere: {
        var _ = root.refreshTick;
        if (typeof app !== "undefined" && app && app.groupCall
            && app.groupCall.active && app.groupCall.roomId === root.roomId)
            return true;
        return root.rtcReachable && app.rtc.ownDeviceInSession(root.roomId);
    }

    implicitWidth: root.live ? root.glyphSize : 0
    implicitHeight: root.live ? root.glyphSize : 0
    visible: root.live

    Connections {
        enabled: root.rtcReachable
        target: (typeof app !== "undefined" && app) ? app.rtc : null
        function onSessionChanged(changedRoomId) {
            if (changedRoomId === root.roomId)
                root.refreshTick = root.refreshTick + 1;
        }
        function onAvailabilityChanged() {
            root.refreshTick = root.refreshTick + 1;
        }
    }
    // Our own join/leave never pokes the room from the sync loop's point of
    // view in time to matter, and "am I in this call" is the half of the
    // answer this device knows for certain.
    Connections {
        enabled: typeof app !== "undefined" && app && app.groupCall
        target: (typeof app !== "undefined" && app) ? app.groupCall : null
        function onActiveChanged() {
            root.refreshTick = root.refreshTick + 1;
        }
        function onRoomIdChanged() {
            root.refreshTick = root.refreshTick + 1;
        }
    }

    // Behind a Loader: an Icon is a Text, and a never-laid-out empty Text
    // keeps ItemObservesViewport forever — in a per-row delegate that is the
    // single most expensive QML mistake known in this tree (d1ddc2f).
    Loader {
        anchors.fill: parent
        active: root.live
        sourceComponent: Icon {
            name: "call"
            size: root.glyphSize
            color: root.joinedHere ? AppTheme.accent : root.color
        }
    }

    Accessible.role: Accessible.StaticText
    Accessible.name: root.joinedHere ? qsTr("You are in a call in this room")
                                     : qsTr("There is a call in this room")
}
