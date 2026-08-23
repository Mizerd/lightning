import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// "N people in call" — the room's live MatrixRTC session, made visible.
//
// This is what lets a Lightning user SEE a call an Element user started:
// the membership state events are observed, parsed and counted, and the
// result is a calm banner with a facepile. It deliberately produces no
// timeline rows — call membership churn as timeline spam is exactly what
// PART 18 asks us not to do.
//
// The Join affordance is the honest part. Lightning can observe a session
// but cannot yet publish membership (doing so without a media transport
// would tell every client in the room to open an SFU connection that can
// never complete), so the button is DISABLED and carries the specific
// reason. `app.rtc.joinBlockReason()` returns a closed-set token, mapped to
// wording here — never a raw server string.
Rectangle {
    id: root

    /// The room this banner describes.
    property string roomId: ""

    // EVERY binding that calls into RtcController must read `refreshTick`.
    // Qt cannot track a C++ function call as a dependency, so a binding
    // without the tick evaluates once and never again — which for
    // `participantCount` means `visible` stays false and the banner never
    // appears at all.
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

    /// Human wording for `blockReason`. The tokens are a closed set from
    /// RtcController::joinBlockReason — a raw server string is never shown.
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
        default:
            return qsTr("Joining isn't available");
        }
    }

    /// Facepile faces, deduped per PERSON: the same account on two devices
    /// is two participants but one face, and repeating an avatar with no
    /// explanation reads as a bug. Each entry carries the room-resolved
    /// profile, so real avatars are drawn where known and initials only
    /// where they are not.
    readonly property var faces: {
        var _ = root.refreshTick;
        return roomId.length > 0 ? app.rtc.participantFaces(roomId, 4) : [];
    }

    /// This device is in this room's call, so the call CONTROLS are up and
    /// this banner has nothing left to offer. `ownUserPresent` is not enough:
    /// the same account on another device is a genuine other participant, and
    /// the banner should still offer to join from here.
    readonly property bool ownDeviceHere: {
        var _ = root.refreshTick;
        return roomId.length > 0 && app.rtc.ownDeviceInSession(roomId);
    }
    /// ...and the same when the local call controller is on this room, which
    /// covers the window between pressing Join and the membership landing.
    readonly property bool locallyInCall: app.groupCall.active && app.groupCall.roomId === root.roomId

    objectName: "roomCallBanner"
    visible: hasCall && !ownDeviceHere && !locallyInCall
    // Height collapses when there is no call so the room layout does not
    // reserve space for a banner nobody can see.
    implicitHeight: visible ? content.implicitHeight + AppTheme.spacing12 * 2 : 0
    height: implicitHeight
    color: AppTheme.accentSoft
    radius: AppTheme.radiusMd
    border.width: 1
    border.color: AppTheme.accentBorder

    // Re-read whenever this room's session changes. The controller emits
    // only on a REAL change, so this does not churn on every poke.
    Connections {
        target: app.rtc
        function onSessionChanged(changedRoomId) {
            if (changedRoomId === root.roomId) {
                // Re-evaluate the count/facepile bindings, which are plain
                // function calls and therefore have no dependency Qt could
                // track on its own.
                root.refresh();
            }
        }
        function onAvailabilityChanged() {
            root.refresh();
        }
    }

    // Bindings above call C++ functions, which Qt cannot observe for
    // change, so a nudge property is bumped to re-evaluate them. This is
    // the same pattern the media-cache handlers use: bump a counter the
    // binding READS, never assign over the binding itself.
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

        // Facepile. A Repeater of avatars; each face is a real participant,
        // so an empty list simply renders nothing.
        RowLayout {
            spacing: -8
            Layout.alignment: Qt.AlignVCenter
            Repeater {
                model: root.faces
                delegate: Item {
                    required property var modelData
                    implicitWidth: 26
                    implicitHeight: 26
                    // Ring in the banner's own colour so overlapping faces
                    // stay separable without a border on the avatar itself.
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

        // Behind a Loader: this label is EMPTY in the state it is created
        // in (no call yet), and a never-laid-out empty Text keeps
        // ItemObservesViewport for the life of the item — §16's single most
        // expensive QML mistake. Harmless where this banner sits today (a
        // sibling of the timeline Flickable, not inside its content item),
        // but a later re-parent would make it costly and silent.
        Loader {
            Layout.fillWidth: true
            active: root.participantCount > 0
            visible: active
            sourceComponent: Text {
                text: {
                    var _ = root.refreshTick;
                    var n = root.participantCount;
                    if (n <= 0)
                        return "";
                    // Branched explicitly rather than %n: without a loaded
                    // translation a %n source string renders its "(s)"
                    // literally.
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

        // The reason a call cannot be joined, rendered INLINE rather than
        // as a tooltip. A disabled AbstractButton receives no hover events
        // in Qt Quick, so `ToolTip.visible: hovered` on a disabled control
        // never fires — and `joinBlock()` has no "None" branch today, so
        // the button is always disabled and the explanation would never
        // have been reachable at all.
        Text {
            Layout.maximumWidth: 220
            visible: text.length > 0
            text: root.blockText
            color: AppTheme.textMuted
            font.pixelSize: 12
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
            // The full sentence is always reachable, even when elided.
            ToolTip.visible: hoverHandler.hovered && text.length > 0
            ToolTip.text: root.blockText
            HoverHandler {
                id: hoverHandler
            }
        }

        AppButton {
            objectName: "roomCallJoinButton"
            text: qsTr("Join")
            visible: root.blockReason.length === 0
            enabled: visible
            // This handler was an EMPTY BLOCK with a comment saying no join
            // path existed yet, long after one did. The button rendered,
            // looked enabled, and did nothing — reported as "when I try to
            // join a call started from Element it just doesn't join".
            onClicked: app.groupCall.join(root.roomId, false)
        }
    }
}
