import QtQuick
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
// ONE SINK PER TRACK. `SfuVideoRouter` holds a single screen sink per
// participant identity: a second attach on the same identity replaces the
// first, and the first surface's destruction then detaches the survivor, so
// one of the two goes permanently blank. A share must therefore be rendered in
// exactly ONE place at a time — the grid and the spotlight are mutually
// exclusive Loaders, and the spotlight's strip excludes the spotlighted
// share BY shareId.
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
                    if (root.local)
                        app.groupCall.detachLocalScreenSink();
                    else
                        app.groupCall.detachScreenSink(root.ownerIdentity);
                }
                Component.onCompleted: attach()
                Component.onDestruction: detach()
            }
        }

        // Placeholder while the first frame is in flight. NOT a claim that
        // the share is unviewable — the old stage drew permanent wording of
        // that kind over an empty rectangle and never rendered anything,
        // which is how "I did not see their screenshare" happened.
        ColumnLayout {
            anchors.centerIn: parent
            spacing: 6
            visible: !videoLoader.visible
            Icon {
                Layout.alignment: Qt.AlignHCenter
                name: "screen_share"
                size: root.compact ? 20 : 30
                color: AppTheme.stormTextSecondary
            }
            Loader {
                Layout.alignment: Qt.AlignHCenter
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
                }
            }
        }

        // Nameplate: bottom-left pill, the share glyph INSIDE it ahead of the
        // name, always visible. This is the affordance that says "this is a
        // screen, and whose".
        Rectangle {
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
                anchors.centerIn: parent
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
}
