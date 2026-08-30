import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// One SCREEN SHARE on the call stage.
//
// A SHARE IS A TILE, NOT A MODE. That sentence is the whole point of this
// file. The stage used to have exactly one notion of a share: `sharingPerson`
// returned the FIRST participant whose `screenSharing` was true and the layout
// switched itself to a spotlight on them. Consequences, both reported:
//   * a SECOND simultaneous sharer had no surface anywhere — not a tile, not a
//     strip entry, not a picker. They were structurally unreachable.
//   * "Back to grid" latched `layoutMode = "grid"` and nothing ever wrote it
//     back, so the share became unreachable for the rest of the call
//     ("now if share is closed no way to get it back").
// Both disappear once every live share is an ordinary tile in the grid: the
// grid is then a COMPLETE index of everything on offer, so dismissing a
// spotlight cannot lose anything. Discord works this way and that is why it
// never strands you.
//
// One person sharing WITH their camera on therefore occupies TWO tiles — this
// one and their CallParticipantTile — which is correct: they are two separate
// tracks and one surface can only render one of them.
//
// ONE SINK PER TRACK, AND THE LAST ATTACH OWNS IT. `SfuVideoRouter` holds a
// single screen sink per participant identity: a second attach on the same
// identity replaces the first, and a release names the SINK — so it gives up
// only what that sink still owns, and a superseded surface gives up nothing.
//
// That second clause was missing until 2026-08-27, and its absence is the
// reported "when i full screen it it stop shwoing video". The comment here
// used to claim the safety property came from "the grid and the spotlight are
// mutually exclusive Loaders" — which is true of their `active` and NOT of
// their object LIFETIME. Qt builds the newly activated Loader's content
// synchronously and destroys the deactivated one's with `deleteLater()`, so
// the two overlap by one event-loop turn, and the dying tile's key-named
// detach landed inside that window every single time.
//
// A share is still rendered in exactly ONE place at a time — the strip
// excludes the spotlighted share BY shareId, and full screen stands the stage
// surfaces down — but that is now an arrangement, not the guarantee. The
// guarantee is the ownership rule.
Item {
    id: root

    /// Stable identity of this share for one call. Remote: the LiveKit
    /// screen-share track sid. Local: "local:<n>". A share that stops and
    /// restarts is a NEW published track and so a new id — which is why a
    /// restarted share can never arrive still-dismissed.
    property string shareId: ""
    /// The SFU identity of whoever is sharing. This is what routes the video:
    /// the screen sink is keyed on the participant, not on the track key.
    property string ownerIdentity: ""
    property string ownerDisplayName: ""
    /// Watched only so the sink is RE-ATTACHED when it arrives. The SFU can
    /// announce a share before it says which media section the track landed
    /// on, and an attach made while the key was still empty never receives a
    /// frame. A LOCAL share exists before the SFU has named a track for it at
    /// all, so this is empty for a while by construction.
    property string trackKey: ""
    /// Our own share. Routes through the engine's self-view tee rather than
    /// through a received stream — there is no remote stream for this device.
    property bool local: false

    /// Compact form, for the strip beside the spotlight.
    property bool compact: false
    /// This tile IS the spotlight.
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
        // A share tile is always a panel: there is content in it, or there is
        // about to be. It never takes the bare-avatar shape.
        color: AppTheme.stormInset
        border.width: root.focused || root.activeFocus ? 2 : 1
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
            sourceComponent: Item {
                /// Nothing has arrived yet: the tile keeps its placeholder
                /// rather than showing a black hole while the first frame is
                /// in flight.
                readonly property bool hasFrame:
                    output.videoSink && output.videoSink.videoSize.width > 0

                VideoOutput {
                    id: output
                    anchors.fill: parent
                    // A shared screen is CONTENT: it is FITTED, never
                    // cropped. Cropping hides the edges of what the other
                    // person is showing, which is usually exactly where their
                    // toolbars and tabs are.
                    fillMode: VideoOutput.PreserveAspectFit
                }

                function attach() {
                    if (root.local)
                        app.groupCall.attachLocalScreenSink(output.videoSink);
                    else
                        app.groupCall.attachScreenSink(root.ownerIdentity,
                                                       output.videoSink);
                }
                function detach() {
                    // Names the SINK, never the key. The key-named detach
                    // this replaced is what made a spotlighted share blank:
                    // the spotlight's tile is built SYNCHRONOUSLY while the
                    // grid's is destroyed by deleteLater(), so the dying grid
                    // tile removed the key the spotlight had just taken, and
                    // nothing re-attached. That is "when i full screen it it
                    // stop shwoing video", every time rather than as a race.
                    app.groupCall.detachSink(output.videoSink);
                }
                Component.onCompleted: attach()
                Component.onDestruction: detach()

                // No periodic re-arm — see CallParticipantTile for why one was
                // written and removed. `onTrackKeyChanged` on the tile covers
                // the late-key case, which is the only one that needs it.
            }
        }

        // Placeholder while the first frame is in flight. NOT a claim that
        // the share is unviewable — the old stage drew permanent wording of
        // that kind over an empty rectangle and never rendered anything,
        // which is how "I did not see their screenshare" happened.
        ColumnLayout {
            anchors.centerIn: parent
            // A WIDTH, so the sentence below has something to elide against.
            // Centred with no width, the column took its own implicit one —
            // the whole unwrapped line — and a grid cell narrower than that
            // simply cut the wording off at both ends, because this tile
            // clips. Which cells are narrower than the line is a function of
            // how wide the platform draws it.
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
                // FILLING, not centre-aligned: an aligned cell is not
                // stretched, so the text would keep its implicit width and
                // overflow exactly as before. The label centres itself
                // inside the width it is given instead.
                Layout.fillWidth: true
                // A Label whose text can be empty in the state it is created
                // in belongs behind a Loader — this delegate is instantiated
                // per share, and a never-laid-out empty Text keeps
                // ItemObservesViewport forever.
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

        // Nameplate: bottom-left pill, the share glyph INSIDE it ahead of the
        // name, always visible. This is the affordance that says "this is a
        // screen, and whose".
        Rectangle {
            id: namePlate
            objectName: "callShareNameplate"
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: root.compact ? 6 : 8
            implicitWidth: Math.min(plate.implicitWidth + 12,
                                    surface.width - (root.compact ? 12 : 16))
            implicitHeight: plate.implicitHeight + 6
            radius: AppTheme.radiusPill
            // Its own dark field, painted over arbitrary video, so the name
            // stays legible on a bright screen share.
            color: Qt.rgba(0, 0, 0, 0.55)

            RowLayout {
                id: plate
                objectName: "callShareNameplateRow"
                // Anchored to the plate's EDGES, not merely centred in it —
                // see the long note in CallParticipantTile. `centerIn` gave
                // this row its full implicit width, so "%1's screen" ran out
                // past both ends of its own pill and was then cut off by the
                // tile's clip. The label can only elide against a width
                // somebody gave it.
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
                    Layout.fillWidth: true
                    text: root._label
                    // A fixed light ink rather than a theme token: this plate
                    // paints its own field over video, so the surrounding
                    // theme says nothing about what is legible on it.
                    color: "#FFFFFF"
                    font.pixelSize: root.compact ? 11 : 12
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }
            }
        }

        TapHandler {
            // Left button only: TapHandlers are non-exclusive across
            // subtrees, so grabbing every button would also swallow presses
            // meant for the stage beneath.
            acceptedButtons: Qt.LeftButton
            onTapped: root.activated()
        }

        HoverHandler {
            id: shareHover
            cursorShape: Qt.PointingHandCursor
        }
    }
    // ── The share's own volume ───────────────────────────────────────────
    //
    // SEPARATE FROM THE SHARER'S MICROPHONE, because they are separate
    // tracks and a viewer wants them apart: turn the game down without
    // silencing the person playing it. The engine keys its receive volume
    // per TRACK for exactly this — one participant publishing both a
    // microphone and a desktop used to collide on a single volume element,
    // so a change landed on whichever GStreamer found first.
    //
    // Offered only when the share actually carries sound. A slider that
    // cannot move anything is worse than no slider.
    // `shareHasAudio()` IS A PLAIN CALL WITH NO NOTIFY BEHIND IT, so this
    // binding cannot re-evaluate on its own. That matters because a sharer
    // can toggle share audio MID-SHARE from our own menu: without a
    // dependency the slider would never appear for a share that started
    // silent, and the right-click would stay dead for the rest of the call.
    // `participantsChanged` is emitted from the path that rebuilds
    // participants out of SFU track state, which is exactly when the answer
    // can change.
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

    // A right-click on the share opens it, mirroring the participant tile —
    // and gated on the share actually having sound, so it never swallows a
    // press the stage wanted when there is nothing to adjust. That collision
    // has shipped three times in this repo.
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
