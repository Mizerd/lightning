import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// Expanded in-app video view: a modal overlay (like ImageViewerOverlay), not an
// OS fullscreen window, so account/room context and focus stay with the shell.
// It borrows the inline card's MediaPlayer, so playback continues without
// doubling audio; closing hands the surface back and restores focus. Escape
// closes.
//
// Chrome, shown together and hidden together during undisturbed playback:
// a header (file name, sender · date, Download, Close) and the full control
// bar with readable type. At the end the view stays open on the poster with
// a Replay instead of snapping shut.
Popup {
    id: root
    objectName: "videoViewerOverlay"

    property var player: null
    property var cardOutput: null
    // The card's AudioOutput, for the control bar's mute/volume.
    property var cardAudio: null
    // The event's declared length, for the bar until the player knows its own.
    property real declaredDurationMs: 0
    // Header: plain text from the event (never markup).
    property string title: ""
    property string senderName: ""
    property var timestamp: null
    property bool canSave: false
    // The video's media key: a save result is shown only for this video.
    property string mediaKey: ""
    // Drawn over the stage once the video has ended.
    property string posterSource: ""
    // The card's player reached its end; Replay is offered.
    property bool ended: false
    signal saveRequested()
    signal replayRequested()
    // Pulse-backend SIGABRT protection (pwcrash): with the `qtAudio` context
    // property, play only once Qt's PulseAudio connection is ready. A card or
    // bar loaded alone (tests) has no qtAudio and plays directly.
    function gatedPlay(p) {
        if (typeof qtAudio !== "undefined" && qtAudio) qtAudio.play(p)
        else p.play()
    }
    function cancelGatedPlay(p) {
        if (typeof qtAudio !== "undefined" && qtAudio) qtAudio.cancelPlay(p)
    }

    // Header and control bar: visible while paused, ended or engaged.
    readonly property bool chromeShown:
        !player || ended
        || player.playbackState !== MediaPlayer.PlayingState
        || overlayHover.hovered || overlayBar.activeFocus
        || overlayBar.menuOpen || header.activeFocus
        || saveNotice.text.length > 0

    function openFor(mediaPlayer, inlineOutput) {
        player = mediaPlayer
        cardOutput = inlineOutput
        cardAudio = mediaPlayer.audioOutput
        open()
        mediaPlayer.videoOutput = expandedOutput
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape

    // "20 Oct 2026 14:05", in the app's 12/24-hour preference.
    function dateText(ts) {
        var clock = typeof app !== "undefined" && app && app.settings
                    && app.settings.clockTimeFormat
                    ? app.settings.clockTimeFormat : "hh:mm"
        return Qt.locale(AppLocale.name).toString(ts, "d MMM yyyy") + " "
               + Qt.formatTime(ts, clock)
    }

    Connections {
        target: typeof app !== "undefined" && app ? app.mediaBridge : null
        enabled: root.opened
        function onSaveFinished(ok, message, mediaKey) {
            if (mediaKey !== root.mediaKey)
                return
            saveNotice.ok = ok
            saveNotice.text = message
            saveNoticeTimer.restart()
        }
    }

    onClosed: {
        saveNotice.text = ""
        // Hand the frames back to the card; playback state is untouched.
        if (player && cardOutput)
            player.videoOutput = cardOutput
        player = null
        cardOutput = null
        cardAudio = null
    }

    background: Rectangle {
        // Dark on every theme, like ImageViewerOverlay (the shared scrim
        // token), and opaque: at the scrim's own 85% the app's chrome showed
        // through around a letterboxed video and competed with it.
        color: Qt.alpha(AppTheme.scrimSurface, 1.0)
    }

    contentItem: FocusScope {
        focus: true

        // The control bar's key map (the inline card uses the same one):
        // Space toggles, arrows seek ±5 s, M mutes, Up/Down adjust and
        // remember the volume, F leaves the expanded view. After the end,
        // Space replays. Escape closes via the Popup policy.
        Keys.onPressed: (event) => {
            if (!root.player) return
            if (root.ended && (event.key === Qt.Key_Space
                               || event.key === Qt.Key_K)) {
                root.replayRequested()
                event.accepted = true
                return
            }
            if (overlayBar.handleKey(event))
                event.accepted = true
        }

        VideoOutput {
            id: expandedOutput
            anchors.fill: parent
            anchors.margins: AppTheme.spacing24
            // Clear of the header and the bar, so neither covers the picture
            // (white header text over a bright frame was unreadable).
            anchors.topMargin: AppTheme.spacing12 + header.height
                               + AppTheme.spacing8
            anchors.bottomMargin: AppTheme.spacing24 + overlayBar.height
            fillMode: VideoOutput.PreserveAspectFit
        }
        // The expanded view of a video the software renderer cannot draw.
        SoftwareVideoNotice {
            objectName: "videoViewerSoftwareNotice"
            anchors.fill: expandedOutput
            shown: !!root.player && !root.ended
        }

        // The end: the poster (the frame may already be gone) and a Replay.
        Image {
            objectName: "videoViewerPoster"
            anchors.fill: expandedOutput
            visible: root.ended && status === Image.Ready
            source: root.ended ? root.posterSource : ""
            fillMode: Image.PreserveAspectFit
            asynchronous: true
        }
        AbstractButton {
            id: replayButton
            objectName: "videoReplayButton"
            anchors.centerIn: expandedOutput
            visible: root.ended
            focusPolicy: Qt.TabFocus
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Replay video")
            // A pill: the round glyph's side gets less padding than the
            // label's, so the pair reads as centred.
            padding: AppTheme.spacing12
            leftPadding: AppTheme.spacing16
            rightPadding: AppTheme.spacing20
            onClicked: root.replayRequested()
            // The chrome surface, not the lighter badge scrim: the label sits
            // over whatever the last frame or poster is, bright or not.
            background: Rectangle {
                radius: height / 2
                color: AppTheme.scrimSurface
                border.width: replayButton.visualFocus ? 2 : 1
                border.color: replayButton.visualFocus ? AppTheme.focusRing
                              : replayButton.hovered ? AppTheme.scrimInkMuted
                              : AppTheme.scrimBorder
            }
            contentItem: Row {
                spacing: AppTheme.spacing8
                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: "refresh"
                    size: 28
                    color: AppTheme.scrimInk
                }
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Replay")
                    color: AppTheme.scrimInk
                    font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                    font.weight: AppTheme.weightStrong
                    Accessible.ignored: true
                }
            }
        }
        TapHandler {
            id: overlayTap
            // Taps toggle instantly (exclusive tap signals would delay the
            // single tap); a double tap toggles twice and exits. Not over the
            // control bar: this handler is its sibling and covers its gaps and
            // label, where a near-miss double click would close the overlay
            // (the same band check the image viewer uses).
            // The header is chrome too: a near-miss on Download or Close
            // must not toggle playback or close the view.
            function onTheBar(y) {
                return (overlayBar.visible && y >= overlayBar.y)
                       || (header.visible && y <= header.y + header.height)
            }
            onTapped: (eventPoint) => {
                if (onTheBar(eventPoint.position.y))
                    return
                if (!root.player) return
                if (root.ended) {
                    root.replayRequested()
                    return
                }
                if (root.player.playbackState === MediaPlayer.PlayingState) {
                    root.cancelGatedPlay(root.player)
                    root.player.pause()
                } else {
                    root.gatedPlay(root.player)
                }
            }
            onDoubleTapped: (eventPoint) => {
                if (onTheBar(eventPoint.position.y))
                    return
                root.close()
            }
        }
        HoverHandler { id: overlayHover }

        // Header: what is playing, and the way out. The stage starts below
        // it, so it never sits on the picture.
        FocusScope {
            id: header
            objectName: "videoViewerHeader"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: AppTheme.spacing12
            height: Math.max(headerText.implicitHeight, 36)
            visible: opacity > 0
            opacity: root.chromeShown ? 1.0 : 0.0
            Behavior on opacity {
                enabled: !AppTheme.reducedMotion
                NumberAnimation { duration: 160 }
            }
            ColumnLayout {
                id: headerText
                anchors.left: parent.left
                anchors.right: headerButtons.left
                anchors.rightMargin: AppTheme.spacing8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0
                Label {
                    objectName: "videoViewerTitle"
                    // Remote text: never markup.
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: root.title.length > 0 ? root.title : qsTr("Video")
                    color: AppTheme.scrimInk
                    font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                    font.weight: AppTheme.weightStrong
                    elide: Label.ElideMiddle
                }
                Label {
                    objectName: "videoViewerSubtitle"
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    visible: text.length > 0
                    // Guarded on the sender, as the image viewer's: "· 1 Jan
                    // 1970" is worse than nothing.
                    text: root.senderName.length > 0
                          ? (root.timestamp
                             ? qsTr("%1 · %2").arg(root.senderName)
                                   .arg(root.dateText(root.timestamp))
                             : root.senderName)
                          : ""
                    color: AppTheme.scrimInkMuted
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    elide: Label.ElideRight
                }
                // Download's answer, in the view where it was asked: the
                // timeline's download notice sits behind this overlay.
                Label {
                    id: saveNotice
                    objectName: "videoViewerSaveNotice"
                    property bool ok: true
                    Layout.fillWidth: true
                    visible: text.length > 0
                    textFormat: Text.PlainText
                    // Scrim inks: the theme's success/danger are tuned for the
                    // theme surface, not for this always-dark one.
                    color: ok ? AppTheme.scrimSuccessInk : AppTheme.scrimDangerInk
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    font.weight: AppTheme.weightStrong
                    elide: Label.ElideRight
                    Timer {
                        id: saveNoticeTimer
                        interval: 5000
                        onTriggered: saveNotice.text = ""
                    }
                }
            }
            Row {
                id: headerButtons
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: AppTheme.spacing8
                IconButton {
                    objectName: "videoViewerDownloadButton"
                    visible: root.canSave
                    iconName: "download"
                    iconSize: 20
                    implicitWidth: 36; implicitHeight: 36
                    iconColorOverride: AppTheme.scrimInk
                    Accessible.name: qsTr("Download video")
                    ToolTip.text: qsTr("Download")
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    onClicked: root.saveRequested()
                }
                IconButton {
                    objectName: "videoViewerCloseButton"
                    iconName: "close"
                    iconSize: 20
                    implicitWidth: 36; implicitHeight: 36
                    iconColorOverride: AppTheme.scrimInk
                    Accessible.name: qsTr("Close video")
                    onClicked: root.close()
                }
            }
        }

        // The full control set; the expanded view is the escape hatch from the
        // card's narrow bar.
        VideoControlBar {
            id: overlayBar
            objectName: "videoOverlayControlBar"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: AppTheme.spacing24
            anchors.rightMargin: AppTheme.spacing24
            anchors.bottomMargin: AppTheme.spacing8
            player: root.player
            audio: root.cardAudio
            declaredDurationMs: root.declaredDurationMs
            showExpand: true
            showClose: false
            large: true
            expandIcon: "close_fullscreen"
            onExpandRequested: root.close()
            readonly property bool shown: root.chromeShown
            visible: opacity > 0
            opacity: shown ? 1.0 : 0.0
            Behavior on opacity {
                enabled: !AppTheme.reducedMotion
                NumberAnimation { duration: 160 }
            }
        }
    }
}
