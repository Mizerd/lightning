import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The timeline's call row — "Alice started a call", with a Join button while
// the call is still up.
//
// WHAT THIS REPLACES. A call used to arrive as a room-STATE row (msgtype
// "state", state_kind "m.call", body the literal words "call event"), which
// put it inside RoomActivityDelegate's collapsed group. One call therefore
// rendered as "1 room update" expanding to "call event" — the reported
// "room event look bleak". A call is room HISTORY, not a room setting: it
// gets its own row, it is never hidden by the room-activity preference, and
// it breaks a state-activity run rather than joining one (TimelineModel).
//
// SAFETY RULES THIS ROW OBEYS.
//   * Every field is TYPED and presentation-safe. The sentence is built in
//     TimelineModel from a closed set plus the actor's resolved display
//     name; no free text a remote sender wrote reaches this row, because
//     the row carries a control the reader is invited to click. That is the
//     same rule that made the tombstone banner use Lightning's own wording.
//   * THE BUTTON IS THE ONLY JOIN TARGET. Discord had a period where the
//     whole call row was clickable and people joined calls by accident;
//     this component deliberately declares NO TapHandler and NO MouseArea
//     on the card, so the only way in is the button.
//   * ONE join path. The gate is `app.rtc.joinBlockReason()` and the action
//     is `app.groupCall.join()` — the same two calls RoomCallBanner makes,
//     never a second mechanism. The banner's closed-set reason tokens are
//     the only thing consulted; a raw server string is never shown.
Item {
    id: root

    /// The room this call belongs to (a real room id, never a composite
    /// thread timeline id).
    property string roomId: ""
    /// The caller. `actorName` is the RESOLVED display name; the user id is
    /// carried only for the avatar's colour key.
    property string actorUserId: ""
    property string actorName: ""
    property string actorAvatarMxc: ""
    /// The finished, translated sentence from TimelineModel.
    property string sentence: ""
    /// The caller's stated VIDEO intent. False means "not known to be
    /// video", never "audio only".
    property bool video: false
    /// How many people declined. A count — never who.
    property int declinedCount: 0
    /// The event's own timestamp.
    property var timestamp: undefined
    /// Passed down so an off-screen row does not fetch an avatar.
    property bool onScreen: true

    // ── Is this call still up? ───────────────────────────────────────────
    //
    // EVERY binding below calls into C++, and Qt cannot track a function
    // call as a dependency — a binding without `refreshTick` evaluates once
    // and never again, which for `sessionLive` means the Join button would
    // be frozen at whatever the room looked like when the row was built.
    // Same discipline as RoomCallBanner; bump the counter the bindings READ,
    // never assign over a binding.
    property int refreshTick: 0
    function refresh() {
        refreshTick = refreshTick + 1;
    }
    // Guarded because this delegate is loaded by fixtures that supply no
    // call controllers at all; there it degrades to "no live session", which
    // is the honest answer when nothing can be asked.
    readonly property bool rtcReachable:
        typeof app !== "undefined" && app && app.rtc && root.roomId.length > 0
    readonly property bool groupCallReachable:
        typeof app !== "undefined" && app && app.groupCall
    readonly property int participantCount: {
        var _ = root.refreshTick;
        return root.rtcReachable ? app.rtc.participantCount(root.roomId) : 0;
    }
    /// The room's MatrixRTC session is still up, so joining is meaningful.
    /// This is the ONLY thing that puts a Join button on a call row: a call
    /// that ended is history and offers nothing.
    readonly property bool sessionLive: participantCount > 0
    readonly property string blockReason: {
        var _ = root.refreshTick;
        return root.rtcReachable ? app.rtc.joinBlockReason(root.roomId)
                                 : "unsupported";
    }
    /// This device is already in the call, so the call controls are up and
    /// this row has nothing left to offer. ownDEVICE, not ownUser: the same
    /// account on another device is a real other participant.
    readonly property bool alreadyInThisCall: {
        var _ = root.refreshTick;
        if (root.groupCallReachable && app.groupCall.active
            && app.groupCall.roomId === root.roomId)
            return true;
        return root.rtcReachable && app.rtc.ownDeviceInSession(root.roomId);
    }
    readonly property bool canJoin:
        sessionLive && blockReason.length === 0 && !alreadyInThisCall
        && groupCallReachable

    // Re-read on a real session change only. RtcController emits this when
    // something actually changed, so this does not churn on every poke —
    // and this row deliberately does NOT call app.rtc.refresh() itself: the
    // pane's RoomCallBanner already owns that for the open room, and one
    // refresh per call row would be N requests for one answer.
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
        // Joining from this very row must make the button stand down. The
        // controller's `active` is NOTIFY stateChanged (there is no
        // activeChanged signal — a Connections handler named for one would
        // simply never run, and Qt only warns).
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
        // A live call is the accent surface the banner uses; an ended call
        // is a calm annotation. Both are CARDS — the point of the round is
        // that a call stops looking like a line of grey activity text.
        color: root.sessionLive ? AppTheme.accentSoft : AppTheme.cardElevated
        border.width: 1
        border.color: root.sessionLive ? AppTheme.accentBorder
                                       : AppTheme.borderSubtle

        RowLayout {
            id: content
            anchors.fill: parent
            anchors.margins: AppTheme.spacing12
            spacing: AppTheme.spacing12

            // The phone/camera glyph, on its own disc so the row reads as a
            // call at a glance rather than as another avatar line. Only
            // names present in Icon.qml's map may be used — the bundled
            // Material Symbols font is a SUBSET and an unmapped name renders
            // as tofu.
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

                // BOTH labels sit behind Loaders, and this is not
                // decoration. Every QQuickText is BORN carrying
                // ItemObservesViewport and only setText() with a non-empty
                // string clears it, so a Label whose text can be "" in the
                // state it is created in makes Qt walk the whole
                // instantiated timeline on every contentY change — §16's
                // single most expensive QML mistake. A fixture row with no
                // sentence, and any row whose timestamp is absent, are
                // exactly that state.
                Loader {
                    Layout.fillWidth: true
                    active: root.sentence.length > 0
                    visible: active
                    sourceComponent: Label {
                        objectName: "callEventSentence"
                        text: root.sentence
                        // PlainText, mandatory. The sentence embeds a
                        // member-chosen display name; Qt's AutoText default
                        // would promote a name beginning with markup to
                        // StyledText, and an <img src="https://…"> name
                        // would then fire an unconsented remote beacon from
                        // every viewer.
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
            }

            // The Join button, and ONLY while the call is still up. A
            // Loader, not a `visible` toggle: an ended call must not carry a
            // laid-out control at all, and this is also what the contract
            // test looks for.
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
                    // The SAME call RoomCallBanner's Join makes. Not a second
                    // join path: one gate (app.rtc.joinBlockReason) and one
                    // action (app.groupCall.join), so the two surfaces cannot
                    // drift into disagreeing about whether a call is joinable.
                    onClicked: app.groupCall.join(root.roomId, false)
                    Accessible.name: qsTr("Join the call")
                }
            }
        }
    }

    // The secondary line: the time, and the decline count when there is one.
    // Branched explicitly rather than with %n — without a loaded translation
    // a %n source string renders its "(s)" literally (§16).
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

    // The whole row announces itself as one thing to a screen reader; the
    // button keeps its own name so "Join" is reachable by itself.
    Accessible.role: Accessible.StaticText
    Accessible.name: root.metaText.length > 0
                     ? qsTr("%1 %2").arg(root.sentence).arg(root.metaText)
                     : root.sentence
}
