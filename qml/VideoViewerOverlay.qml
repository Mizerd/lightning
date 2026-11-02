import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

Popup {
    id: root
    objectName: "videoViewerOverlay"

    property var player: null
    property var cardOutput: null
    property var cardAudio: null
    property real declaredDurationMs: 0
    property string title: ""
    property string senderName: ""
    property var timestamp: null
    property bool canSave: false
    property string mediaKey: ""
    property string posterSource: ""
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
        if (player && cardOutput)
            player.videoOutput = cardOutput
        player = null
        cardOutput = null
        cardAudio = null
    }

    background: Rectangle {
        color: Qt.alpha(AppTheme.scrimSurface, 1.0)
    }

    contentItem: FocusScope {
        focus: true
        Accessible.role: Accessible.Dialog
        Accessible.name: root.title.length > 0
                         ? qsTr("Video: %1").arg(root.title) : qsTr("Video")

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
            anchors.topMargin: AppTheme.spacing12 + header.height
                               + AppTheme.spacing8
            anchors.bottomMargin: AppTheme.spacing24 + overlayBar.height
            fillMode: VideoOutput.PreserveAspectFit
        }
        SoftwareVideoNotice {
            objectName: "videoViewerSoftwareNotice"
            anchors.fill: expandedOutput
            shown: !!root.player && !root.ended
        }

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
            padding: AppTheme.spacing12
            leftPadding: AppTheme.spacing16
            rightPadding: AppTheme.spacing20
            onClicked: root.replayRequested()
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
                Label {
                    id: saveNotice
                    objectName: "videoViewerSaveNotice"
                    property bool ok: true
                    Layout.fillWidth: true
                    visible: text.length > 0
                    textFormat: Text.PlainText
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
                VideoControlBar.ScrimIconButton {
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
                VideoControlBar.ScrimIconButton {
                    objectName: "videoViewerCloseButton"
                    iconName: "close"
                    iconSize: 20
                    implicitWidth: 36; implicitHeight: 36
                    iconColorOverride: AppTheme.scrimInk
                    Accessible.name: qsTr("Close video")
                    ToolTip.text: Accessible.name
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    onClicked: root.close()
                }
            }
        }

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
