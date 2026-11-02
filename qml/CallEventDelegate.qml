import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root

    property string roomId: ""
    property string actorUserId: ""
    property string actorName: ""
    property string actorAvatarMxc: ""
    property string sentence: ""
    property bool video: false
    property int declinedCount: 0
    property var timestamp: undefined
    property bool onScreen: true

    property int refreshTick: 0
    function refresh() {
        refreshTick = refreshTick + 1;
    }
    readonly property bool rtcReachable:
        typeof app !== "undefined" && app && app.rtc && root.roomId.length > 0
    readonly property bool groupCallReachable:
        typeof app !== "undefined" && app && app.groupCall
    readonly property int participantCount: {
        var _ = root.refreshTick;
        return root.rtcReachable ? app.rtc.participantCount(root.roomId) : 0;
    }
    readonly property bool sessionLive: participantCount > 0
    readonly property string blockReason: {
        var _ = root.refreshTick;
        return root.rtcReachable ? app.rtc.joinBlockReason(root.roomId)
                                 : "unsupported";
    }
    readonly property bool alreadyInThisCall:
        root.groupCallReachable && app.groupCall.active
        && app.groupCall.roomId === root.roomId
    property bool isLatestCallRow: true
    readonly property bool supersededByNewerCall: !isLatestCallRow
    readonly property bool canJoin:
        sessionLive && blockReason.length === 0 && !alreadyInThisCall
        && groupCallReachable && !supersededByNewerCall

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
            return qsTr("You can't join calls in this room");
        case "network_route":
            return qsTr("Calls aren't available while this account uses a proxy");
        case "media_encryption_unavailable":
            return qsTr("This room is encrypted, and encrypted calls "
                        + "aren't available in this build");
        default:
            return qsTr("Joining isn't available");
        }
    }
    readonly property string joinBlockedText:
        (root.sessionLive && !root.alreadyInThisCall && root.groupCallReachable
         && !root.supersededByNewerCall
         && root.blockReason.length > 0) ? root.blockText : ""

    Connections {
        enabled: root.rtcReachable
        target: (typeof app !== "undefined" && app) ? app.rtc : null
        function onSessionChanged(changedRoomId) {
            if (changedRoomId === root.roomId)
                root.refresh();
        }
        function onAvailabilityChanged() {
            root.refresh();
        }
    }
    Connections {
        enabled: root.groupCallReachable
        target: (typeof app !== "undefined" && app) ? app.groupCall : null
        function onStateChanged() {
            root.refresh();
        }
    }

    implicitHeight: card.implicitHeight

    Rectangle {
        id: card
        objectName: "callEventCard"
        width: parent.width
        implicitHeight: content.implicitHeight + AppTheme.spacing12 * 2
        height: implicitHeight
        radius: AppTheme.radiusMd
        color: root.sessionLive ? AppTheme.accentSoft : AppTheme.cardElevated
        border.width: 1
        border.color: root.sessionLive ? AppTheme.accentBorder
                                       : AppTheme.borderSubtle

        RowLayout {
            id: content
            anchors.fill: parent
            anchors.margins: AppTheme.spacing12
            spacing: AppTheme.spacing12

            Rectangle {
                Layout.alignment: Qt.AlignVCenter
                implicitWidth: 32
                implicitHeight: 32
                radius: width / 2
                color: root.sessionLive ? AppTheme.accentBorder : AppTheme.hover
                Icon {
                    objectName: "callEventGlyph"
                    anchors.centerIn: parent
                    name: root.video ? "videocam" : "call"
                    size: 18
                    color: root.sessionLive ? AppTheme.accent
                                            : AppTheme.textMuted
                }
            }

            Avatar {
                objectName: "callEventActorAvatar"
                Layout.alignment: Qt.AlignVCenter
                size: 24
                mxc: root.actorAvatarMxc
                name: root.actorName.length > 0 ? root.actorName
                                                : root.actorUserId
                colorKey: root.actorUserId
                onScreen: root.onScreen
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                spacing: 2

                // Both labels sit behind Loaders: a Text created with "" keeps
                // ItemObservesViewport and makes Qt walk the timeline on every
                // scroll. A missing sentence or timestamp is exactly that
                // state.
                Loader {
                    Layout.fillWidth: true
                    active: root.sentence.length > 0
                    visible: active
                    sourceComponent: Label {
                        objectName: "callEventSentence"
                        text: root.sentence
                        // PlainText is mandatory: the sentence contains a
                        // member-chosen display name, and AutoText could render
                        // an <img> name as a remote beacon for every viewer.
                        textFormat: Text.PlainText
                        color: AppTheme.textPrimary
                        font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                        font.weight: AppTheme.weightMedium
                        elide: Label.ElideRight
                        Accessible.name: text
                    }
                }

                Loader {
                    Layout.fillWidth: true
                    active: root.metaText.length > 0
                    visible: active
                    sourceComponent: Label {
                        objectName: "callEventMeta"
                        text: root.metaText
                        textFormat: Text.PlainText
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        elide: Label.ElideRight
                        Accessible.name: text
                    }
                }

                Loader {
                    Layout.fillWidth: true
                    active: root.joinBlockedText.length > 0
                    visible: active
                    sourceComponent: Label {
                        objectName: "callEventBlockReason"
                        text: root.joinBlockedText
                        textFormat: Text.PlainText
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        elide: Label.ElideRight
                        Accessible.name: text
                    }
                }
            }

            Loader {
                objectName: "callEventJoinLoader"
                Layout.alignment: Qt.AlignVCenter
                active: root.canJoin
                visible: active
                sourceComponent: AppButton {
                    objectName: "callEventJoinButton"
                    text: qsTr("Join")
                    kind: "primary"
                    size: "sm"
                    onClicked: app.groupCall.join(root.roomId, false)
                    Accessible.name: qsTr("Join the call")
                }
            }
        }
    }

    readonly property string timeText:
        root.timestamp === undefined || root.timestamp === null
        ? "" : Qt.formatDateTime(root.timestamp, app.settings.clockTimeFormat)
    readonly property string declinedText:
        root.declinedCount <= 0 ? ""
        : (root.declinedCount === 1 ? qsTr("1 person declined")
                                    : qsTr("%1 people declined")
                                        .arg(root.declinedCount))
    readonly property string metaText: {
        if (root.declinedText.length === 0)
            return root.timeText;
        if (root.timeText.length === 0)
            return root.declinedText;
        return qsTr("%1 · %2").arg(root.timeText).arg(root.declinedText);
    }

    Accessible.role: Accessible.StaticText
    Accessible.name: root.metaText.length > 0
                     ? qsTr("%1 %2").arg(root.sentence).arg(root.metaText)
                     : root.sentence
}
