import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// One screen share on the call stage. A share is a tile, not a mode: every
// live share is an ordinary grid tile, so the grid is a complete index and
// dismissing the spotlight can't strand a share. A person sharing with their
// camera on occupies two tiles (this and their CallParticipantTile), one per
// track.
//
// SfuVideoRouter holds one screen sink per participant identity: the last
// attach owns it, and a release names the sink, so a superseded surface gives
// up nothing. That matters because Qt builds a newly activated Loader's
// content synchronously but destroys the old one with deleteLater(), so
// surfaces overlap for one event-loop turn during layout changes.
Item {
    id: root

    /// True under Qt Quick's software renderer, which can't draw video even
    /// though frames arrive. The tile then keeps its placeholder and CallStage
    /// explains why. `app` is looked up defensively for standalone tests.
    readonly property bool softwareRendererHidesVideo:
        typeof app !== "undefined" && app && app.softwareRenderer === true

    /// Stable id of this share for one call: the LiveKit screen-share track
    /// sid, or "local:<n>" for ours. A restarted share is a new track and a new
    /// id, so it can never arrive already dismissed.
    property string shareId: ""
    /// The sharer's SFU identity; the screen sink is keyed on it.
    property string ownerIdentity: ""
    property string ownerDisplayName: ""
    /// Watched so the sink is re-attached when it arrives: an attach under an
    /// empty key never receives a frame. A local share exists before the SFU
    /// names its track.
    property string trackKey: ""
    /// Our own share, routed through the engine's self-view tee.
    property bool local: false

    /// Compact form, for the strip beside the spotlight.
    property bool compact: false
    /// This tile is the spotlight.
    property bool focused: false

    signal activated()

    implicitWidth: compact ? 148 : 240
    implicitHeight: compact ? 96 : 168

    readonly property string _label: root.local
        ? qsTr("Your screen")
        : (root.ownerDisplayName.length > 0
           ? qsTr("%1's screen").arg(root.ownerDisplayName)
           : qsTr("Shared screen"))

    onTrackKeyChanged: if (videoLoader.item)
        videoLoader.item.attach()

    Accessible.role: Accessible.Button
    Accessible.name: root._label
    Accessible.description: qsTr("Screen share")
    Accessible.focusable: true
    Accessible.onPressAction: root.activated()

    activeFocusOnTab: true
    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                || event.key === Qt.Key_Space) {
            root.activated()
            event.accepted = true
        }
    }

    Rectangle {
        id: surface
        anchors.fill: parent
        radius: AppTheme.radiusTile
        // Always a panel, never the bare-avatar shape.
        color: AppTheme.stormInset
        // While the picture shows, its own frame (below) is the only edge. The
        // focus ring still draws on the tile.
        readonly property bool pictureFramed:
            videoLoader.visible && videoLoader.item
            && videoLoader.item.pictureFramed === true
        border.width: root.activeFocus ? 2 : (pictureFramed ? 0 : (root.focused ? 2 : 1))
        border.color: root.activeFocus
                      ? AppTheme.focusRing
                      : (root.focused ? AppTheme.accentBorder
                                      : AppTheme.borderSubtle)
        clip: true

        Loader {
            id: videoLoader
            anchors.fill: parent
            anchors.margins: 1
            active: root.ownerIdentity.length > 0
            visible: active && item && item.hasFrame
                     && !root.softwareRendererHidesVideo
            sourceComponent: Item {
                /// Keeps the placeholder until the first frame arrives.
                readonly property bool hasFrame:
                    output.videoSink && output.videoSink.videoSize.width > 0
                readonly property bool pictureFramed: videoFrame.visible

                VideoOutput {
                    id: output
                    anchors.fill: parent
                    // Shares are fitted, never cropped: the edges usually hold
                    // the sharer's toolbars and tabs.
                    fillMode: VideoOutput.PreserveAspectFit
                }
                // A frame around the painted picture rather than the tile, so
                // the fitted video doesn't look loose on a large surface.
                // Computed from the frame size: VideoOutput's contentRect
                // didn't describe it.
                Rectangle {
                    id: videoFrame
                    objectName: "callShareVideoFrame"
                    readonly property real videoW:
                        output.videoSink ? output.videoSink.videoSize.width : 0
                    readonly property real videoH:
                        output.videoSink ? output.videoSink.videoSize.height : 0
                    readonly property real fit:
                        videoW > 0 && videoH > 0
                        ? Math.min(output.width / videoW, output.height / videoH)
                        : 0
                    readonly property real paintedW: Math.round(videoW * fit)
                    readonly property real paintedH: Math.round(videoH * fit)
                    x: output.x + Math.round((output.width - paintedW) / 2)
                    y: output.y + Math.round((output.height - paintedH) / 2)
                    width: paintedW
                    height: paintedH
                    visible: fit > 0
                    color: "transparent"
                    radius: AppTheme.radiusSm
                    border.width: 2
                    // Carries the spotlight accent.
                    border.color: root.focused ? AppTheme.accentBorder
                                               : Qt.rgba(1, 1, 1, 0.34)
                }

                function attach() {
                    if (root.local)
                        app.groupCall.attachLocalScreenSink(output.videoSink);
                    else
                        app.groupCall.attachScreenSink(root.ownerIdentity,
                                                       output.videoSink);
                }
                function detach() {
                    // Names the sink, never the key: a dying tile's key-named
                    // detach would unhook the surface that just replaced it.
                    app.groupCall.detachSink(output.videoSink);
                }
                Component.onCompleted: attach()
                Component.onDestruction: detach()

                // No periodic re-arm (see CallParticipantTile);
                // onTrackKeyChanged covers late keys.
            }
        }

        // Placeholder while the first frame is in flight; not a claim the share
        // is unviewable.
        ColumnLayout {
            anchors.centerIn: parent
            // A width so the text below can elide; otherwise the column takes
            // the line's full width and the clipping tile cuts it off.
            width: parent.width - AppTheme.spacing16
            spacing: 6
            visible: !videoLoader.visible
            Icon {
                Layout.alignment: Qt.AlignHCenter
                name: "screen_share"
                size: root.compact ? 20 : 30
                color: AppTheme.stormTextSecondary
            }
            Loader {
                // Filling, not centre-aligned, so the text has a width to elide
                // against; the label centres itself.
                Layout.fillWidth: true
                // Behind a Loader: an empty Text keeps ItemObservesViewport.
                active: !root.compact && root._label.length > 0
                visible: active
                sourceComponent: Text {
                    text: qsTr("Waiting for the picture…")
                    color: AppTheme.stormTextSecondary
                    font.pixelSize: 12
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                }
            }
        }

        // Nameplate: bottom-left pill with the share glyph ahead of the name.
        // On a large surface it covers the picture, so it fades out 3 s after
        // the tile appears or the pointer last moved; small grid tiles keep it
        // as their label.
        readonly property bool plateAutoHides: root.width >= 480
        property int plateIdleTicks: 0
        property bool plateIdle: false
        Timer {
            id: plateIdleTimer
            objectName: "callSharePlateIdleTimer"
            interval: 500
            repeat: true
            running: surface.plateAutoHides && !surface.plateIdle
            onTriggered: {
                surface.plateIdleTicks += 1
                if (surface.plateIdleTicks >= 6) // 3 s
                    surface.plateIdle = true
            }
        }
        onPlateAutoHidesChanged: {
            surface.plateIdleTicks = 0
            surface.plateIdle = false
        }
        property real plateLastX: -1
        property real plateLastY: -1
        HoverHandler {
            id: plateHover
            enabled: surface.plateAutoHides
            onPointChanged: {
                const p = plateHover.point.position
                if (Math.abs(p.x - surface.plateLastX) < 1
                    && Math.abs(p.y - surface.plateLastY) < 1)
                    return
                surface.plateLastX = p.x
                surface.plateLastY = p.y
                surface.plateIdleTicks = 0
                surface.plateIdle = false
            }
        }
        Rectangle {
            id: namePlate
            objectName: "callShareNameplate"
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: root.compact ? 6 : 8
            opacity: surface.plateAutoHides && surface.plateIdle ? 0 : 1
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: 220 } }
            implicitWidth: Math.min(plate.implicitWidth + 12,
                                    surface.width - (root.compact ? 12 : 16))
            implicitHeight: plate.implicitHeight + 6
            radius: AppTheme.radiusPill
            // Its own dark field, legible over bright video.
            color: Qt.rgba(0, 0, 0, 0.55)

            RowLayout {
                id: plate
                objectName: "callShareNameplateRow"
                // Anchored to the plate's edges so the label can elide (see
                // CallParticipantTile).
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 4
                Icon {
                    Layout.alignment: Qt.AlignVCenter
                    name: "screen_share"
                    size: root.compact ? 12 : 14
                    color: AppTheme.success
                }
                Text {
                    // Remote or externally chosen text: never markup.
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: root._label
                    // A fixed light ink: the plate paints its own field over
                    // video.
                    color: "#FFFFFF"
                    font.pixelSize: root.compact ? 11 : 12
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }
            }
        }

        TapHandler {
            // Left button only: TapHandlers are non-exclusive across subtrees,
            // so taking every button would swallow presses meant for the stage.
            acceptedButtons: Qt.LeftButton
            onTapped: root.activated()
        }

        HoverHandler {
            id: shareHover
            cursorShape: Qt.PointingHandCursor
        }
    }
    // ── The share's own volume ──
    // Separate from the sharer's microphone (separate tracks; the engine keys
    // receive volume per track). Offered only when the share carries sound.
    // shareHasAudio() is a plain call with no notify, so participantsChanged
    // (emitted when participants are rebuilt from SFU track state) drives a
    // revision, since share audio can be toggled mid-share.
    property int shareAudioRevision: 0
    Connections {
        target: app.groupCall
        function onParticipantsChanged() {
            root.shareAudioRevision = root.shareAudioRevision + 1
        }
    }
    readonly property bool shareAudioOffered: {
        var _ = root.shareAudioRevision
        return root.shareId.length > 0 && app.groupCall
            && app.groupCall.shareHasAudio(root.shareId)
    }

    property int shareVolumeRevision: 0
    function currentShareVolume() {
        var _ = root.shareVolumeRevision
        if (!root.shareAudioOffered)
            return 100
        var v = app.groupCall.shareVolume(root.shareId)
        return (v === undefined || v === null) ? 100 : v
    }
    function applyShareVolume(percent) {
        if (!root.shareAudioOffered)
            return
        app.groupCall.setShareVolume(root.shareId, Math.round(percent))
        root.shareVolumeRevision = root.shareVolumeRevision + 1
    }
    function openShareVolume() {
        if (root.shareAudioOffered)
            shareVolumePopup.open()
    }

    // Right-click opens it, as on the participant tile, and only when the share
    // has sound, so it never swallows a press the stage wanted.
    TapHandler {
        enabled: root.shareAudioOffered
        acceptedButtons: Qt.RightButton
        onTapped: root.openShareVolume()
    }

    Popup {
        id: shareVolumePopup
        objectName: "callShareVolumePopup"
        x: Math.round((root.width - width) / 2)
        y: Math.round((root.height - height) / 2)
        width: 268
        margins: 8
        padding: AppTheme.spacing12
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        onOpened: shareVolumeSlider.value = root.currentShareVolume()

        background: Rectangle {
            radius: AppTheme.radiusMd
            color: AppTheme.stormPanel
            border.width: 1
            border.color: AppTheme.stormBorder
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
                hoverEnabled: true
            }
        }

        contentItem: ColumnLayout {
            spacing: AppTheme.spacing8
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                Icon {
                    name: shareVolumeSlider.value > 0 ? "volume_up"
                                                      : "volume_off"
                    size: 18
                    color: AppTheme.stormTextSecondary
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Screen share")
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightMedium
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }
                Text {
                    objectName: "callShareVolumeReadout"
                    text: Math.round(shareVolumeSlider.value) + "%"
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightMedium
                }
            }
            Slider {
                id: shareVolumeSlider
                objectName: "callShareVolumeSlider"
                Layout.fillWidth: true
                from: 0
                to: 200
                stepSize: 1
                snapMode: Slider.SnapAlways
                Accessible.name: qsTr("Screen share volume")
                onMoved: root.applyShareVolume(value)
                background: Rectangle {
                    x: shareVolumeSlider.leftPadding
                    y: shareVolumeSlider.topPadding
                       + shareVolumeSlider.availableHeight / 2 - 2
                    width: shareVolumeSlider.availableWidth
                    height: 4
                    radius: AppTheme.radiusPill
                    color: AppTheme.stormInset
                    Rectangle {
                        width: shareVolumeSlider.visualPosition * parent.width
                        height: parent.height
                        radius: AppTheme.radiusPill
                        color: AppTheme.bolt
                    }
                    Rectangle {
                        x: Math.round(parent.width / 2) - 1
                        y: -3
                        width: 2
                        height: parent.height + 6
                        radius: 1
                        color: AppTheme.stormTextMuted
                    }
                }
                handle: Rectangle {
                    x: shareVolumeSlider.leftPadding
                       + shareVolumeSlider.visualPosition
                         * (shareVolumeSlider.availableWidth - width)
                    y: shareVolumeSlider.topPadding
                       + shareVolumeSlider.availableHeight / 2 - height / 2
                    width: 16
                    height: 16
                    radius: 8
                    color: AppTheme.bolt
                    border.width: 1
                    border.color: AppTheme.stormBorder
                }
            }
        }
    }

}
