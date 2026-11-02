import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: root

    property string roomId: ""

    // Every binding that calls into RtcController must read `refreshTick`: a
    // C++ function call is not a tracked dependency, and without it `visible`
    // would never become true.
    readonly property int participantCount: {
        var _ = root.refreshTick;
        return roomId.length > 0 ? app.rtc.participantCount(roomId) : 0;
    }
    readonly property bool hasCall: participantCount > 0
    readonly property bool ownUserPresent: {
        var _ = root.refreshTick;
        return roomId.length > 0 && app.rtc.ownUserInSession(roomId);
    }
    readonly property string blockReason: {
        var _ = root.refreshTick;
        return roomId.length > 0 ? app.rtc.joinBlockReason(roomId) : "unsupported";
    }

    readonly property string blockText: {
        switch (root.blockReason) {
        case "":
            return "";
        case "unsupported":
            return qsTr("This build can't join Matrix calls");
        case "undiscovered":
            return qsTr("Checking whether calling is available…");
        case "no_transport":
            return qsTr("No MatrixRTC service on this homeserver");
        case "discovery_failed":
            return qsTr("Couldn't reach this homeserver's call service, "
                        + "so calling isn't available");
        case "session_closed":
            return qsTr("This call has ended");
        case "no_media_transport":
            return qsTr("Joining calls isn't supported yet in this build");
        case "no_permission":
            return qsTr("You can't start or join calls in this room");
        case "network_route":
            return qsTr("Calls aren't available while this account uses a proxy");
        case "media_encryption_unavailable":
            return qsTr("This room is encrypted, and encrypted calls "
                        + "aren't available in this build");
        default:
            return qsTr("Joining isn't available");
        }
    }

    readonly property var faces: {
        var _ = root.refreshTick;
        return roomId.length > 0 ? app.rtc.participantFaces(roomId, 4) : [];
    }

    readonly property bool locallyInCall: app.groupCall.active && app.groupCall.roomId === root.roomId

    objectName: "roomCallBanner"
    visible: hasCall && !locallyInCall
    implicitHeight: visible ? content.implicitHeight + AppTheme.spacing12 * 2 : 0
    height: implicitHeight
    color: AppTheme.callBanner
    radius: AppTheme.radiusMd
    border.width: 1
    border.color: AppTheme.accentBorder

    Connections {
        target: app.rtc
        function onSessionChanged(changedRoomId) {
            if (changedRoomId === root.roomId) {
                root.refresh();
            }
        }
        function onAvailabilityChanged() {
            root.refresh();
        }
    }

    property int refreshTick: 0
    function refresh() {
        refreshTick = refreshTick + 1;
    }

    onRoomIdChanged: {
        if (roomId.length > 0)
            app.rtc.refresh(roomId);
    }

    RowLayout {
        id: content
        anchors.fill: parent
        anchors.margins: AppTheme.spacing12
        spacing: AppTheme.spacing12

        Icon {
            name: "call"
            size: 18
            color: AppTheme.accent
            Layout.alignment: Qt.AlignVCenter
        }

        RowLayout {
            spacing: -8
            Layout.alignment: Qt.AlignVCenter
            Repeater {
                model: root.faces
                delegate: Item {
                    required property var modelData
                    implicitWidth: 26
                    implicitHeight: 26
                    Rectangle {
                        anchors.fill: parent
                        radius: width / 2
                        color: AppTheme.accentSoft
                    }
                    Avatar {
                        anchors.centerIn: parent
                        size: 22
                        mxc: parent.modelData ? parent.modelData.avatarMxc : ""
                        name: parent.modelData ? (parent.modelData.displayName.length > 0 ? parent.modelData.displayName : parent.modelData.userId) : ""
                        colorKey: parent.modelData ? parent.modelData.userId : ""
                    }
                }
            }
        }

        Loader {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            active: root.participantCount > 0
            visible: active
            sourceComponent: Text {
                text: {
                    var _ = root.refreshTick;
                    var n = root.participantCount;
                    if (n <= 0)
                        return "";
                    if (root.ownUserPresent && n === 1)
                        return qsTr("You are in a call");
                    return n === 1 ? qsTr("1 person in call") : qsTr("%1 people in call").arg(n);
                }
                color: AppTheme.callBannerText
                font.pixelSize: AppTheme.scaled(13)
                font.weight: Font.Medium
                elide: Text.ElideRight
            }
        }

        // The block reason is inline, not a tooltip: a disabled button receives
        // no hover events.
        Text {
            Layout.maximumWidth: 220
            visible: text.length > 0
            text: root.blockText
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.scaled(12)
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
            ToolTip.visible: hoverHandler.hovered && text.length > 0
            ToolTip.text: root.blockText
            HoverHandler {
                id: hoverHandler
            }
        }

        AppButton {
            objectName: "roomCallRetryButton"
            text: qsTr("Try again")
            size: "sm"
            Layout.minimumWidth: implicitWidth
            visible: root.blockReason === "discovery_failed"
            onClicked: app.rtc.discover(root.roomId)
        }

        AppButton {
            objectName: "roomCallJoinButton"
            text: qsTr("Join")
            Layout.minimumWidth: implicitWidth
            visible: root.blockReason.length === 0
            enabled: visible
            onClicked: app.groupCall.join(root.roomId, false)
        }
    }
}
