import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The timeline's call row ("Alice started a call"), with a Join button while
// the call is still up. A call is room history, not a setting: it gets its
// own row, is never hidden by the room-activity preference, and breaks a
// state-activity run (TimelineModel).
//
// Safety rules:
//   * Every field is typed and presentation-safe; the sentence is built in
//     TimelineModel from a closed set plus the resolved display name. No
//     sender-written free text reaches a row that carries a control.
//   * The button is the only join target: no TapHandler or MouseArea on the
//     card, so calls can't be joined by accident.
//   * One join path: gated by `app.rtc.joinBlockReason()`, acted on by
//     `app.groupCall.join()`, as in RoomCallBanner. Only the closed-set
//     reason tokens are shown, never a raw server string, and a blocked live
//     call says why.
Item {
    id: root

    /// The call's room (a real room id, never a composite thread timeline id).
    property string roomId: ""
    /// The caller. `actorName` is the resolved display name; the user id is
    /// only the avatar's colour key.
    property string actorUserId: ""
    property string actorName: ""
    property string actorAvatarMxc: ""
    /// The finished, translated sentence from TimelineModel.
    property string sentence: ""
    /// The caller's stated video intent. False means "not known to be video",
    /// not "audio only".
    property bool video: false
    /// How many people declined. A count, never who.
    property int declinedCount: 0
    /// The event's own timestamp.
    property var timestamp: undefined
    /// Passed down so an off-screen row doesn't fetch an avatar.
    property bool onScreen: true

    // ── Is this call still up? ──
    // These bindings call into C++, which Qt can't track, so they read
    // `refreshTick` (bumped, never assigned over), as in RoomCallBanner.
    property int refreshTick: 0
    function refresh() {
        refreshTick = refreshTick + 1;
    }
    // Guarded for fixtures that provide no call controllers; they degrade to
    // "no live session".
    readonly property bool rtcReachable:
        typeof app !== "undefined" && app && app.rtc && root.roomId.length > 0
    readonly property bool groupCallReachable:
        typeof app !== "undefined" && app && app.groupCall
    readonly property int participantCount: {
        var _ = root.refreshTick;
        return root.rtcReachable ? app.rtc.participantCount(root.roomId) : 0;
    }
    /// The room's MatrixRTC session is up. The only thing that puts a Join
    /// button on a call row.
    readonly property bool sessionLive: participantCount > 0
    readonly property string blockReason: {
        var _ = root.refreshTick;
        return root.rtcReachable ? app.rtc.joinBlockReason(root.roomId)
                                 : "unsupported";
    }
    /// This device is already in the call. Only the local call controller can
    /// answer this: a room-state membership naming this device can outlive a
    /// crash (until it expires), and the same user on another device is a real
    /// other participant.
    readonly property bool alreadyInThisCall:
        root.groupCallReachable && app.groupCall.active
        && app.groupCall.roomId === root.roomId
    /// Whether this is the newest call row. `sessionLive` is per room, so
    /// without this every call row in the room would offer Join. MatrixRTC has
    /// one session per room; only the newest row keeps the affordance. Defaults
    /// to true for hosts that don't set it (standalone fixtures).
    property bool isLatestCallRow: true
    readonly property bool supersededByNewerCall: !isLatestCallRow
    readonly property bool canJoin:
        sessionLive && blockReason.length === 0 && !alreadyInThisCall
        && groupCallReachable && !supersededByNewerCall

    /// Human wording for `blockReason`, the same closed token set
    /// (RtcController::joinBlockReason) that RoomCallBanner.blockText and
    /// IncomingCallPrompt.joinBlockText map. A raw server string is never
    /// shown.
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
        case "media_encryption_unavailable":
            return qsTr("This room is encrypted, and encrypted calls "
                        + "aren't available in this build");
        default:
            return qsTr("Joining isn't available");
        }
    }
    /// Shown where the Join button would be: a live call this device isn't in
    /// and can't join. An ended call has nothing to explain.
    readonly property string joinBlockedText:
        (root.sessionLive && !root.alreadyInThisCall && root.groupCallReachable
         && !root.supersededByNewerCall
         && root.blockReason.length > 0) ? root.blockText : ""

    // Re-read on a real session change only. This row doesn't call
    // app.rtc.refresh() itself: the open room's RoomCallBanner owns that, and
    // one refresh per row would be N requests for one answer.
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
        // Joining from this row must stand the button down. `active` is
        // NOTIFY stateChanged (there is no activeChanged signal).
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
        // A live call uses the banner's accent surface; an ended call a calm
        // card.
        color: root.sessionLive ? AppTheme.accentSoft : AppTheme.cardElevated
        border.width: 1
        border.color: root.sessionLive ? AppTheme.accentBorder
                                       : AppTheme.borderSubtle

        RowLayout {
            id: content
            anchors.fill: parent
            anchors.margins: AppTheme.spacing12
            spacing: AppTheme.spacing12

            // The call glyph on its own disc. Only names in Icon.qml's map
            // render (the bundled font is a subset).
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

                // Why there is no Join button. Behind a Loader like the labels
                // above: usually "".
                Loader {
                    Layout.fillWidth: true
                    active: root.joinBlockedText.length > 0
                    visible: active
                    sourceComponent: Label {
                        objectName: "callEventBlockReason"
                        text: root.joinBlockedText
                        // Lightning's own closed set, but still PlainText:
                        // nothing in a row carrying a control is ever markup.
                        textFormat: Text.PlainText
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        elide: Label.ElideRight
                        Accessible.name: text
                    }
                }
            }

            // Join, only while the call is up. A Loader rather than `visible`,
            // so an ended call carries no control (the contract test checks
            // this).
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
                    // The same gate and action as RoomCallBanner's Join, so the
                    // two can't disagree about whether a call is joinable.
                    onClicked: app.groupCall.join(root.roomId, false)
                    Accessible.name: qsTr("Join the call")
                }
            }
        }
    }

    // Secondary line: time and, if any, the decline count. Explicit branches
    // rather than %n, which renders "(s)" literally without a translation.
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

    // One screen-reader name for the row; the button keeps its own so "Join"
    // is reachable.
    Accessible.role: Accessible.StaticText
    Accessible.name: root.metaText.length > 0
                     ? qsTr("%1 %2").arg(root.sentence).arg(root.metaText)
                     : root.sentence
}
