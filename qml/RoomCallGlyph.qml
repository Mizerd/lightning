import QtQuick
import QtQuick.Controls
import MatrixClient

Item {
    id: root

    property string roomId: ""
    property color color: AppTheme.textSecondary
    property int glyphSize: 14

    readonly property bool rtcReachable:
        typeof app !== "undefined" && app && app.rtc && root.roomId.length > 0

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
