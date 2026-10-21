import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The docked "now playing" strip for the app-owned voice/audio clip
// (app.voicePlayback), in the manner of Telegram Desktop and Signal Desktop.
// The default presentation (app.settings.voicePlayerDocked).
//
// Its host lays it out in a band of its own (TimelinePane: under the room
// header and the call bars; Main.qml: along the bottom of Settings, or of a
// pane that has no room column), so it covers nothing: the timeline is
// shorter by its height instead. It shows for as long as a clip is loaded,
// in the clip's own room too, so the strip does not come and go (and the
// timeline does not resize) as the row scrolls.
//
// Left to right: the sender's avatar (or an audio file's cover), the title
// and "Voice message · Thread · Room" (both a link back to the message),
// the clock, the speed chip, play/pause, next (while a run of voice messages
// follows), pop out (the floating card) and stop. The scrubber runs along the
// bottom edge inside the strip, so its hit band never reaches the timeline.
Rectangle {
    id: root
    objectName: "voicePlayerBar"

    // The host decides; the strip draws nothing while false.
    property bool shown: false
    // Shown at the bottom of a screen rather than under a header: the
    // divider goes on top.
    property bool atBottom: false

    readonly property var voice: app.voicePlayback
    readonly property alias logic: logic

    visible: root.shown
    implicitHeight: root.shown ? row.implicitHeight + 6 + seekBand.height : 0
    color: AppTheme.cardElevated
    // A full-width band, so its edges read against the timeline and the
    // header in every theme (design-media P1).
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: root.atBottom ? parent.top : undefined
        anchors.bottom: root.atBottom ? undefined : parent.bottom
        height: 1
        color: AppTheme.border
    }

    VoicePlayerLogic { id: logic }

    // Narrow chat columns drop the least needed parts first.
    readonly property bool roomForClock: root.width >= 420
    // Between 300 and 420 px the position alone stays (C21-D13).
    readonly property bool roomForPosition: root.width >= 300
    readonly property bool roomForSpeed: root.width >= 360

    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Now playing")
    // State and title only: the position is the scrubber's value, never a
    // description that changes every second.
    Accessible.description: logic.accessibleSummary

    RowLayout {
        id: row
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: 4
        anchors.leftMargin: AppTheme.spacing12
        anchors.rightMargin: AppTheme.spacing8
        spacing: AppTheme.spacing8

        // The way back to the message: avatar, title and subtitle together,
        // with a link's hover underline.
        AbstractButton {
            id: sourceLink
            objectName: "voicePlayerBarSource"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            implicitHeight: sourceRow.implicitHeight
            focusPolicy: Qt.StrongFocus
            hoverEnabled: true
            Accessible.role: Accessible.Link
            Accessible.name: qsTr("Go to the message: %1").arg(logic.shownTitle)
            onClicked: logic.openSource()
            // Space is play/pause everywhere on the player, also here; only
            // Return and Enter follow the link (a focused link used to open
            // the message on Space).
            Keys.onSpacePressed: (event) => {
                if (!event.isAutoRepeat)
                    root.voice.togglePlayPause()
                event.accepted = true
            }
            Keys.onReturnPressed: logic.openSource()
            Keys.onEnterPressed: logic.openSource()
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            background: Rectangle {
                radius: AppTheme.radiusSm
                color: "transparent"
                border.width: sourceLink.visualFocus ? 2 : 0
                border.color: AppTheme.focusRing
            }
            contentItem: RowLayout {
                id: sourceRow
                spacing: AppTheme.spacing8
                Item {
                    Layout.alignment: Qt.AlignVCenter
                    implicitWidth: 30
                    implicitHeight: 30
                    Avatar {
                        anchors.fill: parent
                        visible: logic.shownArt.length === 0
                        size: 30
                        mxc: logic.shownAvatar
                        name: logic.shownSender
                        colorKey: logic.shownSenderId
                        onScreen: root.visible
                    }
                    Rectangle {
                        anchors.fill: parent
                        visible: logic.shownArt.length > 0
                        radius: AppTheme.radiusSm
                        color: AppTheme.embedSurface
                        clip: true
                        Image {
                            anchors.fill: parent
                            source: parent.visible ? logic.shownArt : ""
                            sourceSize.width: 64
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                        }
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 0
                    Label {
                        objectName: "voicePlayerBarTitle"
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                        text: logic.shownTitle
                        color: logic.readable(AppTheme.textPrimary, root.color)
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        font.weight: AppTheme.weightStrong
                        font.underline: sourceLink.hovered
                        elide: Text.ElideRight
                    }
                    Label {
                        objectName: "voicePlayerBarSubtitle"
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                        text: logic.statusText.length > 0 ? logic.statusText
                                                          : logic.shownSubtitle
                        color: logic.readable(
                            root.voice.fetchState === "failed"
                            ? AppTheme.danger : AppTheme.textSecondary,
                            root.color)
                        font.pixelSize: AppTheme.scaled(AppTheme.textMicro + 1)
                        elide: Text.ElideRight
                    }
                }
            }
        }

        Label {
            objectName: "voicePlayerBarClock"
            visible: root.roomForClock || root.roomForPosition
            text: root.roomForClock ? logic.clock
                                    : logic.formatPosition(root.voice.position)
            color: logic.readable(AppTheme.textSecondary, root.color)
            font.pixelSize: AppTheme.scaled(AppTheme.textMicro + 1)
            // The scrubber's value carries the position for assistive
            // technology; a label re-announcing every second does not.
            Accessible.ignored: true
        }
        VoiceSpeedChip {
            objectName: "voicePlayerBarSpeed"
            surface: root.color
            visible: root.roomForSpeed
        }
        // The level and mute, on the player that stays (C21-D06).
        MediaVolumeControl {
            objectName: "voicePlayerBarVolume"
            audio: root.voice.active ? root.voice.audioOutput : null
            sliderObjectName: "voicePlayerBarVolumeSlider"
            iconSize: 15
            implicitWidth: 24; implicitHeight: 24
            visible: root.roomForSpeed
        }
        IconButton {
            id: playPauseButton
            objectName: "voicePlayerBarPlayPause"
            fill: true
            implicitWidth: 30; implicitHeight: 30
            iconSize: 17
            focusPolicy: Qt.StrongFocus
            iconName: root.voice.fetchState === "fetching"
                      ? "schedule"
                      : (root.voice.playing ? "pause" : "play_arrow")
            enabled: root.voice.fetchState !== "fetching"
            readonly property string actionLabel: root.voice.playing
                ? qsTr("Pause playback") : qsTr("Resume playback")
            Accessible.name: actionLabel
            ToolTip.text: actionLabel
            ToolTip.visible: hovered
            ToolTip.delay: 600
            onClicked: root.voice.togglePlayPause()
        }
        IconButton {
            objectName: "voicePlayerBarNext"
            visible: root.voice.hasNext
            size: "md"
            focusPolicy: Qt.StrongFocus
            iconName: "chevron_right"
            readonly property string actionLabel: qsTr("Next voice message")
            Accessible.name: actionLabel
            ToolTip.text: actionLabel
            ToolTip.visible: hovered
            ToolTip.delay: 600
            onClicked: root.voice.playNext()
        }
        IconButton {
            objectName: "voicePlayerBarPopOut"
            size: "md"
            focusPolicy: Qt.StrongFocus
            iconName: "open_in_full"
            readonly property string actionLabel:
                qsTr("Pop out the player")
            Accessible.name: actionLabel
            ToolTip.text: actionLabel
            ToolTip.visible: hovered
            ToolTip.delay: 600
            onClicked: app.settings.voicePlayerDocked = false
        }
        IconButton {
            objectName: "voicePlayerBarStop"
            size: "md"
            focusPolicy: Qt.StrongFocus
            iconName: "close"
            readonly property string actionLabel: qsTr("Stop playback")
            Accessible.name: actionLabel
            ToolTip.text: actionLabel
            ToolTip.visible: hovered
            ToolTip.delay: 600
            onClicked: root.voice.stop()
        }
    }

    // The scrubber band, along the bottom edge and inside the strip.
    Item {
        id: seekBand
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.atBottom ? 0 : 1
        anchors.leftMargin: AppTheme.spacing12
        anchors.rightMargin: AppTheme.spacing12
        height: 14
        VoiceSeekBar {
            objectName: "voicePlayerBarSeek"
            anchors.fill: parent
        }
    }

    // The window's "focus the player" shortcut lands on play/pause
    // (Main.qml), so Space right after it plays or pauses.
    function focusPlayer() {
        playPauseButton.forceActiveFocus(Qt.ShortcutFocusReason)
    }

    // Space here toggles the clip, as on the card.
    Keys.onSpacePressed: (event) => {
        if (event.isAutoRepeat) { event.accepted = true; return }
        root.voice.togglePlayPause()
        event.accepted = true
    }
}
