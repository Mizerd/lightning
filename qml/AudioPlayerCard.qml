import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: root
    objectName: "audioPlayerCard"

    property string mediaKey: ""
    // The event this card plays. The player is keyed by it, so a rebuilt row
    // (room switch, scroll, thread panel) finds its live state again.
    property string eventId: ""
    property string roomId: ""
    property string senderName: ""
    property string senderId: ""
    property string senderAvatarMxc: ""
    property string threadRootId: ""
    property string filename: ""
    property string mimetype: ""
    property real fileSize: 0
    property real durationMs: 0
    property bool isVoice: false
    property var waveform: []
    property bool rowOnScreen: true
    property bool prefetchAllowed: true
    property bool canSave: false
    // The event carries a cover another client attached (`info.thumbnail_url`
    // or `thumbnail_file`). It is fetched through the media bridge, never from
    // the URL, and only stands in until the file's own embedded art is read.
    property bool thumbAvailable: false
    property bool inBubble: false

    signal saveRequested()
    // The cover was clicked: show `source` (MediaBridge's RAM-only image URL,
    // never the homeserver's) enlarged, titled `title`. The host owns the
    // viewer; the clip keeps playing.
    signal artworkViewRequested(string source, string title)
    signal openExternalRequested()

    readonly property var voice: app.voicePlayback
    readonly property bool isCurrent:
        root.eventId.length > 0 && root.voice.active
        && root.voice.eventId === root.eventId
    readonly property bool playing: root.isCurrent && root.voice.playing
    readonly property bool ready: root.isCurrent && root.voice.loaded
    readonly property string fetchState:
        root.isCurrent ? root.voice.fetchState : "idle"
    readonly property real livePosition:
        root.isCurrent ? root.voice.position : 0
    readonly property real liveDuration:
        root.isCurrent && root.voice.duration > 0 ? root.voice.duration
                                                  : root.durationMs
    readonly property bool seekable: root.ready && root.voice.seekable
    // What an earlier play of this file found: its tags and cover art, kept
    // in RAM by the controller (never written to disk). Re-read whenever the
    // controller learns something (clipInfoRevision).
    readonly property var clipInfo: {
        var revision = root.voice.clipInfoRevision
        return root.isVoice || root.mediaKey.length === 0
            ? ({}) : root.voice.clipInfo(root.mediaKey)
    }
    property string artworkSource: root.clipInfo.artworkSource || root.wireCoverSource
    property string wireCoverSource: ""
    property bool wireCoverFailed: false
    function refreshWireCover() {
        if (wireCoverSource.length > 0 || wireCoverFailed || !thumbAvailable
                || isVoice || mediaKey.length === 0 || !rowOnScreen
                || !app.mediaBridge || !app.mediaBridge.supported)
            return
        wireCoverSource = app.mediaBridge.mediaSource(mediaKey, "thumb") || ""
    }
    function noteCoverStatus(image, shown) {
        if (image.status === Image.Ready && root.wireCoverSource.length > 0
                && shown === root.wireCoverSource
                && !root.clipInfo.artworkSource)
            root.voice.rememberEventCover(root.mediaKey, root.wireCoverSource)

        if (image.status === Image.Error && shown.length > 0
                && shown === root.wireCoverSource) {
            root.wireCoverSource = ""
            root.wireCoverFailed = true
        }
    }
    function requestArtworkView() {
        if (root.artworkSource.length > 0)
            root.artworkViewRequested(root.artworkSource,
                                      root.filename || root.trackTitle)
    }
    onThumbAvailableChanged: refreshWireCover()
    readonly property string wireCoverCacheKey: "thumb:" + root.mediaKey
    Connections {
        target: root.thumbAvailable && app.mediaBridge ? app.mediaBridge : null
        function onMediaCached(cacheKey) {
            if (cacheKey === root.wireCoverCacheKey && !root.wireCoverFailed)
                root.wireCoverSource = app.mediaBridge.cachedSource(cacheKey)
        }
        function onMediaFetchFailed(cacheKey, category) {
            if (cacheKey === root.wireCoverCacheKey)
                root.wireCoverFailed = true
        }
        function onMediaRetryable(cacheKey) {
            if (cacheKey === root.wireCoverCacheKey) {
                root.wireCoverFailed = false
                root.refreshWireCover()
            }
        }
    }
    readonly property string trackTitle: root.clipInfo.title || ""
    readonly property string trackArtist: root.clipInfo.artist || ""
    readonly property real rate: root.isVoice
        ? app.settings.voicePlaybackRate
        : (root.isCurrent ? root.voice.rate : 1.0)

    function formatPosition(ms) {
        if (!ms || ms < 0) ms = 0
        return root.clockText(Math.floor(ms / 1000))
    }
    function formatDuration(ms) {
        if (!ms || ms < 0) ms = 0
        return root.clockText(Math.round(ms / 1000))
    }
    function readableInk(ink) {
        return root.inBubble ? ink : AppTheme.legibleInkOn(ink, root.color)
    }
    function clockText(totalSeconds) {
        return AppTheme.clockText(totalSeconds)
    }
    function meta(startAtMs) {
        var m = {
            "senderName": root.senderName,
            "senderId": root.senderId,
            "senderAvatarMxc": root.senderAvatarMxc,
            "filename": root.filename,
            "isVoice": root.isVoice,
            "durationMs": root.durationMs,
            "threadRootId": root.threadRootId,
            "waveform": root.waveform || []
        }
        if (startAtMs !== undefined && startAtMs >= 0)
            m.startAtMs = Math.round(startAtMs)
        return m
    }
    function togglePlay() {
        if (!app.mediaBridge.supported) {
            root.openExternalRequested()
            return
        }
        root.voice.toggle(root.eventId, root.roomId, root.mediaKey, root.meta())
    }
    function seekTo(ms) {
        if (root.isCurrent) {
            root.voice.seek(ms)
            return
        }
        if (!app.mediaBridge.supported || root.eventId.length === 0
            || root.mediaKey.length === 0)
            return
        root.voice.play(root.eventId, root.roomId, root.mediaKey,
                        root.meta(ms))
    }
    function setRate(rate) {
        if (root.isVoice)
            app.settings.voicePlaybackRate = rate
        else if (root.isCurrent)
            root.voice.setRate(rate)
    }
    function maybePrefetch() {
        if (rowOnScreen && prefetchAllowed && mediaKey.length > 0
            && app.mediaBridge.supported
            && app.settings.videoPrefetchEnabled)
            app.mediaBridge.prefetchPlayable(
                mediaKey,
                fileSize || app.settings.knownMediaSizeBytes(mediaKey) || 0)
    }
    onPrefetchAllowedChanged: if (prefetchAllowed) maybePrefetch()
    Component.onCompleted: {
        refreshWireCover()
        maybePrefetch()
        reportVisibility()
    }
    onMediaKeyChanged: {
        wireCoverSource = ""
        wireCoverFailed = false
        refreshWireCover()
        maybePrefetch()
    }
    onRowOnScreenChanged: {
        if (rowOnScreen) {
            refreshWireCover()
            maybePrefetch()
        }
        reportVisibility()
    }
    property Item _viewport: null
    function findViewport() {
        for (var p = root.parent; p; p = p.parent) {
            if (p.contentY !== undefined && p.flickableDirection !== undefined)
                return p
        }
        return null
    }
    function actuallyOnScreen() {
        if (!root.visible || !root.rowOnScreen || root.width <= 0)
            return false
        if (!root._viewport)
            root._viewport = root.findViewport()
        var view = root._viewport
        if (!view)
            return true
        var r = root.mapToItem(view, 0, 0, root.width, root.height)
        return r.x + r.width > 8 && r.x < view.width - 8
            && r.y + r.height > 8 && r.y < view.height - 8
    }
    function reportVisibility() {
        root.voice.reportRowVisible(root, root.eventId,
                                    root.isCurrent && root.actuallyOnScreen())
    }
    Timer {
        interval: 250
        repeat: true
        running: root.isCurrent && root.rowOnScreen
        onTriggered: root.reportVisibility()
    }
    onIsCurrentChanged: reportVisibility()
    function focusPlayButton() {
        playButton.forceActiveFocus(Qt.ShortcutFocusReason)
    }
    onVisibleChanged: reportVisibility()
    Component.onDestruction: root.voice.reportRowVisible(root, root.eventId,
                                                         false)

    implicitWidth: Math.min(
        360, root.hostContentWidth >= 0
             ? root.hostContentWidth
             : (bubble ? bubble.width : 360))
    readonly property bool artKnown:
        !root.isVoice && (root.artworkSource.length > 0
                          || (root.thumbAvailable && !root.wireCoverFailed))
    readonly property bool largeCover:
        root.artKnown && root.width >= 280
        && app.settings && app.settings.albumArtStyle !== "compact"
    readonly property bool hugeCover:
        root.largeCover && app.settings.albumArtStyle === "large"
    readonly property real tileSize:
        root.hugeCover ? Math.max(40, root.width - (root.inBubble ? 0 : 12))
                       : (root.largeCover ? Math.round(AppTheme.scaled(96)) : 40)
    implicitHeight: cardGrid.implicitHeight + 12
    color: root.inBubble ? "transparent" : AppTheme.embedSurface
    radius: AppTheme.radiusSm
    border.color: AppTheme.border
    border.width: root.inBubble ? 0 : 1
    property var bubble: null
    property real hostContentWidth: -1

    GridLayout {
        id: cardGrid
        readonly property bool huge: root.hugeCover
        readonly property bool big: root.largeCover && !root.hugeCover
        readonly property int ctlRow: huge || big ? 1 : 0
        readonly property int colShift: huge ? 1 : 0
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 6
        anchors.leftMargin: root.inBubble ? 0 : 6
        anchors.rightMargin: root.inBubble ? 0 : 6
        columns: 6
        columnSpacing: 8
        rowSpacing: 6

        Loader {
            objectName: "audioArtSlot"
            Layout.alignment: cardGrid.huge ? (Qt.AlignTop | Qt.AlignHCenter)
                                            : Qt.AlignTop
            Layout.row: 0
            Layout.column: 0
            Layout.rowSpan: cardGrid.big ? 2 : 1
            Layout.columnSpan: cardGrid.huge ? 6 : 1
            active: root.artKnown
            visible: active
            sourceComponent: Item {
                implicitWidth: root.tileSize
                implicitHeight: root.tileSize
                Rectangle {
                    id: coverArtBox
                    objectName: "audioCoverArtBox"
                    anchors.fill: parent
                    radius: AppTheme.radiusSm
                    color: coverArtImage.status === Image.Ready
                           ? "transparent" : AppTheme.cardElevated
                    clip: true
                    Image {
                        id: coverArtImage
                        objectName: "audioCoverArtwork"
                        readonly property string shown: root.artworkSource
                        anchors.fill: parent
                        // The provider bakes the tile's own rounded square, so
                        // no corner of the picture lies outside the frame.
                        source: shown.indexOf("image://lightning-media/") === 0
                                ? shown + "|shape:rsq:" + Math.max(1, Math.min(500,
                                      Math.round(coverArtBox.radius * 1000
                                                 / Math.max(1, root.tileSize))))
                                : shown
                        onStatusChanged: root.noteCoverStatus(this, shown)
                        readonly property int decodeEdge:
                            root.hugeCover ? 720 : (root.largeCover ? 320 : 96)
                        sourceSize.width: decodeEdge
                        sourceSize.height: decodeEdge
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: true
                        opacity: status === Image.Ready ? 1 : 0
                        Behavior on opacity {
                            enabled: !AppTheme.reducedMotion
                            NumberAnimation { duration: 220 }
                        }
                        Accessible.name: qsTr("Audio cover artwork")
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: parent.radius
                        color: "transparent"
                        border.width: 1
                        border.color: root.playing ? AppTheme.accent
                                                   : AppTheme.border
                        Behavior on border.color {
                            enabled: !AppTheme.reducedMotion
                            ColorAnimation { duration: 180 }
                        }
                    }
                }
                CoverArtButton {
                    objectName: "audioCoverButton"
                    anchors.fill: parent
                    enabled: root.artworkSource.length > 0
                    onActivated: root.requestArtworkView()
                }
            }
        }
        Item {
            Layout.alignment: cardGrid.big ? Qt.AlignVCenter : Qt.AlignTop
            Layout.row: cardGrid.ctlRow
            Layout.column: 1 - cardGrid.colShift
            Layout.fillWidth: cardGrid.big
            implicitWidth: playButton.implicitWidth
            implicitHeight: root.isVoice ? playButton.implicitHeight
                                         : (cardGrid.big ? 30 : 40)
            IconButton {
                id: playButton
                objectName: "audioPlayPauseButton"
                x: cardGrid.big ? 0 : (parent.width - width) / 2
                y: (parent.height - height) / 2
                fill: true
                focusPolicy: Qt.StrongFocus
                implicitWidth: 30; implicitHeight: 30
                iconName: root.fetchState === "fetching"
                          ? "schedule"
                          : (root.playing ? "pause" : "play_arrow")
                iconSize: 17
                enabled: root.fetchState !== "fetching"
                readonly property string what: root.isVoice
                    ? qsTr("voice message")
                    : (root.trackTitle || root.filename || qsTr("audio"))
                readonly property string actionLabel: root.playing
                    ? qsTr("Pause %1").arg(what) : qsTr("Play %1").arg(what)
                Accessible.name: actionLabel
                Accessible.description: root.isCurrent
                    ? (root.playing ? qsTr("Playing") : qsTr("Paused"))
                    : qsTr("%1 long").arg(root.formatDuration(root.liveDuration))
                property bool tipSuppressed: false
                onHoveredChanged: if (!hovered) tipSuppressed = false
                onClicked: {
                    tipSuppressed = true
                    root.togglePlay()
                }
                ToolTip {
                    objectName: "audioPlayPauseTip"
                    parent: playButton
                    visible: playButton.hovered && !playButton.tipSuppressed
                    delay: 600
                    text: playButton.actionLabel
                    x: (playButton.width - implicitWidth) / 2
                    y: -implicitHeight - 6
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            Layout.row: cardGrid.huge ? 1 : 0
            Layout.column: cardGrid.big ? 1 : 2 - cardGrid.colShift
            Layout.columnSpan: cardGrid.big ? 4 : 1
            Layout.fillHeight: cardGrid.big
            spacing: 2
            Label {
                objectName: "audioTitle"
                textFormat: Text.PlainText
                text: root.fetchState === "failed"
                      ? qsTr("This audio cannot be played")
                      : root.isVoice ? qsTr("Voice message")
                                     : (root.trackTitle || root.filename
                                        || qsTr("Audio"))
                color: root.fetchState === "failed"
                       ? AppTheme.danger : AppTheme.text
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                font.weight: AppTheme.weightStrong
                elide: Label.ElideMiddle
                Layout.fillWidth: true
            }

            Item {
                Layout.fillWidth: true
                implicitHeight: 18
                visible: root.fetchState !== "failed"

                VoiceSeekBar {
                    id: seekSlider
                    objectName: root.isVoice && root.waveform
                                && root.waveform.length > 0
                                ? "audioWaveRow" : "audioSeekSlider"
                    anchors.fill: parent
                    followsPlayer: root.isCurrent
                    idleDuration: root.durationMs
                    idleSeekable: app.mediaBridge.supported
                                  && root.eventId.length > 0
                                  && root.mediaKey.length > 0
                                  && root.liveDuration > 0
                    waveform: root.isVoice ? (root.waveform || []) : []
                    trackHeight: 4
                    onSeekRequested: (ms) => root.seekTo(ms)
                    Accessible.name: root.isVoice
                        ? qsTr("Voice message position")
                        : qsTr("Seek position")
                }
            }

            Label {
                objectName: "audioClock"
                text: {
                    var pos = root.formatPosition(root.livePosition)
                    var total = root.formatDuration(root.liveDuration)
                    var line = root.isCurrent ? pos + " / " + total : total
                    if (!root.isVoice && root.trackArtist.length > 0)
                        line += " • " + root.trackArtist
                    if (!root.isVoice && root.fileSize > 0) {
                        var kb = root.fileSize / 1024
                        line += " • " + (kb < 1024 ? kb.toLocaleString(Qt.locale(AppLocale.name), "f", 0) + " KB"
                                        : Number(kb / 1024).toLocaleString(Qt.locale(AppLocale.name), "f", 1) + " MB")
                    }
                    return line
                }
                Layout.fillWidth: true
                Accessible.ignored: root.isCurrent
                elide: Label.ElideRight
                textFormat: Text.PlainText
                color: root.readableInk(AppTheme.textMuted)
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
            }
        }

        AbstractButton {
            id: audioSpeedButton
            objectName: "audioSpeedButton"
            readonly property var rates: [0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0]
            readonly property real rate: root.rate
            Layout.alignment: cardGrid.big ? Qt.AlignVCenter : Qt.AlignTop
            Layout.row: cardGrid.ctlRow
            Layout.column: cardGrid.big ? 2 : 3 - cardGrid.colShift
            implicitWidth: 34; implicitHeight: 24
            enabled: root.isVoice || root.ready
            focusPolicy: Qt.TabFocus
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Playback speed %1x").arg(rate)
            ToolTip.text: qsTr("Playback speed")
            ToolTip.visible: hovered
            ToolTip.delay: 600
            onClicked: speedMenu.popup()
            Keys.onReturnPressed: (event) => {
                event.accepted = true
                if (enabled) clicked()
            }
            Keys.onEnterPressed: (event) => {
                event.accepted = true
                if (enabled) clicked()
            }
            AppMenu {
                id: speedMenu
                objectName: "audioSpeedMenu"
                menuWidth: 140
                Repeater {
                    model: audioSpeedButton.rates
                    AppMenuItem {
                        required property real modelData
                        text: modelData + "×"
                        iconName: Math.abs(modelData - audioSpeedButton.rate)
                                  < 0.001 ? "check" : ""
                        onTriggered: root.setRate(modelData)
                    }
                }
            }
            background: Rectangle {
                radius: AppTheme.radiusSm
                color: audioSpeedButton.hovered ? AppTheme.hover : "transparent"
                border.width: audioSpeedButton.visualFocus ? 2 : 0
                border.color: AppTheme.focusRing
            }
            contentItem: Label {
                text: audioSpeedButton.rate + "×"
                color: root.readableInk(
                    Math.abs(audioSpeedButton.rate - 1.0) < 0.001
                    ? AppTheme.textMuted : AppTheme.accent)
                opacity: audioSpeedButton.enabled ? 1 : 0.5
                font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                font.weight: AppTheme.weightBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }
        MediaVolumeControl {
            objectName: "audioMuteButton"
            Layout.alignment: cardGrid.big ? Qt.AlignVCenter : Qt.AlignTop
            Layout.row: cardGrid.ctlRow
            Layout.column: cardGrid.big ? 3 : 4 - cardGrid.colShift
            audio: root.isCurrent ? root.voice.audioOutput : null
            sliderObjectName: "audioVolumeSlider"
            iconSize: 15
            implicitWidth: 24; implicitHeight: 24
            opacity: root.ready ? 1 : 0.45
        }
        IconButton {
            objectName: "audioSaveButton"
            Layout.alignment: cardGrid.big ? Qt.AlignVCenter : Qt.AlignTop
            Layout.row: cardGrid.ctlRow
            Layout.column: cardGrid.big ? 4 : 5 - cardGrid.colShift
            iconName: "download"
            iconSize: 15
            implicitWidth: 24; implicitHeight: 24
            visible: root.canSave
            readonly property string actionLabel:
                qsTr("Download %1").arg(root.filename || qsTr("audio"))
            Accessible.name: actionLabel
            ToolTip.text: actionLabel
            ToolTip.visible: hovered
            ToolTip.delay: 600
            onClicked: root.saveRequested()
        }
    }
}
