import QtQuick
import QtQuick.Controls
import MatrixClient

// "There is a call in this room" on a room-list row, one component for both
// layouts. It never asks: RtcController::refresh() can fall back to a full
// /state for an idle room, so a self-refreshing row would issue one per room on
// every rebuild. It reads what the controller already knows and re-reads when
// that room changes; the sync loop's m.call.member handler pokes rooms as calls
// start and end. A call already running before this client synced, in a room
// nothing has poked since, shows nothing (participantCount: "0 means no call or
// none observed yet").
Item {
    id: root

    property string roomId: ""
    /// The glyph's ink, owned by the caller (selected, hovered and muted rows
    /// differ).
    property color color: AppTheme.textSecondary
    property int glyphSize: 14

    // Guarded: fixtures supply no call controller, which reads as no session.
    readonly property bool rtcReachable:
        typeof app !== "undefined" && app && app.rtc && root.roomId.length > 0

    // A counter the binding reads; never assign over the binding.
    property int refreshTick: 0

    readonly property bool live: {
        var _ = root.refreshTick;
        return root.rtcReachable && app.rtc.hasLiveSession(root.roomId);
    }
    /// This device is in that call, drawn in the accent. Asks the call
    /// controller, not room state: ownDeviceInSession() is also true for a stale
    /// membership left by an unclean exit, since the device id survives a
    /// restart.
    readonly property bool joinedHere:
        typeof app !== "undefined" && app && app.groupCall
        && app.groupCall.active && app.groupCall.roomId === root.roomId

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
    // Our own join/leave does not poke the room in time; this device knows that
    // half for certain.
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

    // Behind a Loader: an Icon is a Text, and an empty never-laid-out Text
    // stays a viewport observer.
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
