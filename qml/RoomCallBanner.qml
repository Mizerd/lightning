import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// "N people in call": the room's live MatrixRTC session, visible as a calm
// banner with a facepile. Produces no timeline rows (membership churn is not
// timeline content). When joining is blocked, the button is disabled and the
// reason is shown: app.rtc.joinBlockReason() returns a closed-set token, mapped
// to wording here, never a raw server string.
Rectangle {
    id: root

    /// The room this banner describes.
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

    /// Wording for `blockReason`, from RtcController::joinBlockReason's closed
    /// set.
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
            return qsTr("Couldn't check whether calling is available");
        case "session_closed":
            return qsTr("This call has ended");
        case "no_media_transport":
            return qsTr("Joining calls isn't supported yet in this build");
        case "no_permission":
            // The room's power levels forbid writing call membership; known
            // from the room snapshot before the click.
            return qsTr("You can't start or join calls in this room");
        case "media_encryption_unavailable":
            // The room is encrypted and call media E2EE is not active, so the
            // SFU could read the media.
            return qsTr("This room is encrypted, and encrypted calls "
                        + "aren't available in this build");
        default:
            return qsTr("Joining isn't available");
        }
    }

    /// Facepile faces, one per person (not per device), with room-resolved
    /// profiles: real avatars where known, initials otherwise.
    readonly property var faces: {
        var _ = root.refreshTick;
        return roomId.length > 0 ? app.rtc.participantFaces(roomId, 4) : [];
    }

    /// The local call controller is the only authority on whether this device is
    /// in the call; it also covers the gap between Join and the membership
    /// landing. ownDeviceInSession is not usable: a device id survives a
    /// restart, so a stale membership would hide Join from a user who was just
    /// dropped. ownUserPresent is not enough either: the same account on another
    /// device is a real other participant.
    readonly property bool locallyInCall: app.groupCall.active && app.groupCall.roomId === root.roomId

    objectName: "roomCallBanner"
    visible: hasCall && !locallyInCall
    // Collapse when there is no call.
    implicitHeight: visible ? content.implicitHeight + AppTheme.spacing12 * 2 : 0
    height: implicitHeight
    color: AppTheme.accentSoft
    radius: AppTheme.radiusMd
    border.width: 1
    border.color: AppTheme.accentBorder

    // Re-read when this room's session changes; the controller only emits on
    // real changes.
    Connections {
        target: app.rtc
        function onSessionChanged(changedRoomId) {
            if (changedRoomId === root.roomId) {
                // Re-evaluate the bindings that call plain functions.
                root.refresh();
            }
        }
        function onAvailabilityChanged() {
            root.refresh();
        }
    }

    // Bumped to re-evaluate bindings that call C++ functions; a counter the
    // bindings read, never an assignment over them.
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

        // Facepile of real participants; empty renders nothing.
        RowLayout {
            spacing: -8
            Layout.alignment: Qt.AlignVCenter
            Repeater {
                model: root.faces
                delegate: Item {
                    required property var modelData
                    implicitWidth: 26
                    implicitHeight: 26
                    // A ring in the banner's colour keeps overlapping faces
                    // separable.
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

        // A Loader: this label is empty when created, and a never-laid-out
        // empty Text stays a viewport observer.
        Loader {
            Layout.fillWidth: true
            // 1px preferred, filling: a RowLayout shrinks children in
            // proportion to their preferred widths, which would squeeze Join.
            // The count elides; the button cannot.
            Layout.preferredWidth: 1
            active: root.participantCount > 0
            visible: active
            sourceComponent: Text {
                text: {
                    var _ = root.refreshTick;
                    var n = root.participantCount;
                    if (n <= 0)
                        return "";
                    // Branched rather than %n, which renders "(s)" literally
                    // without a translation.
                    if (root.ownUserPresent && n === 1)
                        return qsTr("You are in a call");
                    return n === 1 ? qsTr("1 person in call") : qsTr("%1 people in call").arg(n);
                }
                color: AppTheme.textPrimary
                font.pixelSize: 13
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
            font.pixelSize: 12
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
            // The full sentence stays reachable when elided.
            ToolTip.visible: hoverHandler.hovered && text.length > 0
            ToolTip.text: root.blockText
            HoverHandler {
                id: hoverHandler
            }
        }

        // The join affordance, gated on blockReason, ownDeviceHere and
        // locallyInCall. CallEventDelegate.qml uses the same
        // app.rtc.joinBlockReason() and app.groupCall.join(), so both surfaces
        // agree; change them together.
        AppButton {
            objectName: "roomCallJoinButton"
            text: qsTr("Join")
            // Never shrink the button; the texts absorb the squeeze.
            Layout.minimumWidth: implicitWidth
            visible: root.blockReason.length === 0
            enabled: visible
            onClicked: app.groupCall.join(root.roomId, false)
        }
    }
}
