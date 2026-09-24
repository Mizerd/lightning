import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// One participant on the call stage. The stage decides which shape is drawn:
//
//   * `bare`: a circular avatar on the stage canvas with the name beneath,
//     no panel (voice-only calls);
//   * a tile: a 16:9 rounded rectangle with a nameplate pill, used for
//     every participant once anyone has a camera or share on.
//
// `micKnown`/`cameraKnown` exist because the SFU reports muted state only for
// tracks it knows about. Unknown renders nothing rather than a confident,
// wrong "not muted".
//
// Per-participant delegate: every Label whose text can be empty lives behind
// a Loader. An empty, never laid-out Text keeps ItemObservesViewport and
// makes Qt walk the whole tree on every scroll frame.
Item {
    id: root

    /// True under Qt Quick's software renderer, which cannot draw video even
    /// though frames arrive and the sink reports a size. The tile then stays in
    /// its placeholder state and CallStage explains why once. `app` is looked
    /// up defensively because tests load this component standalone.
    readonly property bool softwareRendererHidesVideo:
        typeof app !== "undefined" && app && app.softwareRenderer === true

    property string userId: ""
    property string displayName: ""
    property string avatarMxc: ""

    property bool micKnown: false
    property bool micMuted: false
    property bool cameraKnown: false
    property bool cameraOn: false
    /// The SFU participant identity this tile shows; routes video. The only id
    /// that works for both membership formats (the sticky form's is a hash).
    property string identity: ""
    property bool screenSharing: false
    property bool handRaised: false
    /// A transient reaction emoji, usually "". The model clears it when the
    /// reaction expires. A remote value, bounded to one grapheme cluster in
    /// rust/src/rtc.rs and rendered as plain text only.
    property string reactionEmoji: ""
    /// "" (unknown) | "poor" | "good" | "excellent". Unknown draws nothing.
    property string connectionQuality: ""

    /// Which video track this surface shows: "camera" or "screen". A share gets
    /// its own tile (see CallShareTile) rather than replacing the person.
    property string mediaKind: "camera"
    /// Routing keys, watched so the sink is re-attached when they arrive: the
    /// SFU may announce a participant before its tracks have keys.
    property string cameraTrackKey: ""
    property string screenTrackKey: ""
    readonly property string activeTrackKey: root.mediaKind === "screen" ? root.screenTrackKey : root.cameraTrackKey
    // Re-attach when the routing key arrives; an attach under an empty key
    // never receives a frame. Declared here rather than in a Connections inside
    // the Loader, because an `on…Changed` handler can't reach a property whose
    // name starts with an underscore.
    onActiveTrackKeyChanged: if (videoLoader.item)
        videoLoader.item.attach()

    /// Voice-activity ring, driven by the SFU's speaker updates.
    property bool speaking: false
    /// Amplitude 0.0-1.0, from LiveKit's `SpeakerInfo.level`. An SFU that only
    /// reports `active` gives level 0.0 and the ring stays at its minimum; no
    /// level is fabricated.
    property real speakingLevel: 0.0
    /// This participant is the local device.
    property bool local: false
    /// Manually spotlighted.
    property bool focused: false
    /// Compact form for the strip beside a screen share.
    property bool compact: false
    /// Draw no card: just the avatar, ring and name on the stage canvas.
    /// Selection and keyboard focus still draw a card.
    property bool bare: false
    readonly property bool _drawsCard:
        !root.bare || videoLoader.visible || root.focused || root.activeFocus

    signal activated()

    implicitWidth: compact ? 148 : 240
    implicitHeight: compact ? 96 : 168

    // ── The speaking ring ──
    // The ring is a child of a fixed-size holder, so it never affects layout
    // (scaling the avatar would move every neighbour; see
    // CallSpeakerBubbles.qml). Fast attack, slow release, or it strobes between
    // syllables.
    readonly property real ringTarget:
        root.speaking ? 3 + 6 * Math.max(0, Math.min(1, root.speakingLevel)) : 0
    property real ringGap: 0
    onRingTargetChanged: {
        ringMotion.duration = root.ringTarget > root.ringGap ? 60 : 220
        root.ringGap = root.ringTarget
    }
    // The first evaluation isn't a change, so seed the gap for a tile created
    // while its owner is already talking.
    Component.onCompleted: root.ringGap = root.ringTarget
    Behavior on ringGap {
        NumberAnimation {
            id: ringMotion
            duration: 60
            easing.type: Easing.OutCubic
        }
    }

    readonly property int _avatarSize: {
        // Fit the avatar to the tile, from 2-up to 12-up grids.
        var box = Math.min(width, height - (compact ? 18 : 26))
        // A bare tile has no card, so the avatar takes the padding's room too.
        var size = Math.round(box * (root.bare && !root.compact ? 0.74 : 0.52))
        return Math.max(compact ? 28 : 40,
                        Math.min(size, root.bare && !root.compact ? 148 : 96))
    }

    // "You" for the local device; the avatar and colour key still come from the
    // real account.
    readonly property string _label: root.local
                                     ? qsTr("You")
                                     : (root.displayName.length > 0
                                        ? root.displayName : root.userId)
    /// Initials source: the real name, never "You".
    readonly property string _avatarName: root.displayName.length > 0
                                          ? root.displayName
                                          : root.userId

    // ── Per-person playback volume ──
    // Persisted per person (not per call). Not offered on the local tile: you
    // don't hear your own audio; send gain lives in CallDeviceSettings.
    //
    // SfuCallController owns reads and writes, mapping the SFU identity (per
    // device, sometimes an opaque hash) to the Matrix user id the store is
    // keyed by, so the setting survives a rejoin. Read on open rather than
    // bound: only the slider writes it while the popup is up.
    readonly property bool _volumeOffered:
        !root.local && root.identity.length > 0
        && typeof app !== "undefined" && app && app.groupCall

    /// Stored volume, or 100% when unknown. Uses participantVolume(), which
    /// answers from the store, rather than the model's volumePercent role,
    /// which is only right once the controller has seeded the row.
    function currentVolumePercent() {
        if (!root._volumeOffered)
            return 100;
        var value = app.groupCall.participantVolume(root.identity);
        return (value === undefined || value === null) ? 100 : value;
    }

    /// Bumped on every write. Bindings that call currentVolumePercent() must
    /// read it, since Qt can't observe a C++ call as a dependency.
    property int volumeRevision: 0

    /// Apply a level. The controller is the one writer: it drives the engine
    /// and records the preference together.
    function applyVolumePercent(percent) {
        if (!root._volumeOffered)
            return;
        app.groupCall.setParticipantVolume(root.identity, Math.round(percent));
        root.volumeRevision = root.volumeRevision + 1;
    }

    function openVolumeControl() {
        if (!root._volumeOffered)
            return;
        volumePopup.open();
    }

    Accessible.role: Accessible.Button
    Accessible.name: root._label.length > 0 ? root._label : qsTr("Participant")
    // Carries the same facts as the badges, and nothing when state is unknown.
    // The speaking level is decoration and is left out.
    Accessible.description: {
        var parts = []
        if (root.micKnown && root.micMuted)
            parts.push(qsTr("Microphone muted"))
        if (root.screenSharing)
            parts.push(qsTr("Sharing their screen"))
        if (root.handRaised)
            parts.push(qsTr("Hand raised"))
        if (root.reactionEmoji.length > 0)
            parts.push(qsTr("Reacted with %1").arg(root.reactionEmoji))
        if (root.speaking)
            parts.push(qsTr("Speaking"))
        if (root.connectionQuality === "poor")
            parts.push(qsTr("Poor connection"))
        return parts.join(", ")
    }
    Accessible.focusable: true
    Accessible.onPressAction: root.activated()

    activeFocusOnTab: true
    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                || event.key === Qt.Key_Space) {
            root.activated()
            event.accepted = true
        }
        // The context key opens the volume control; the hover button isn't
        // keyboard-reachable.
        if (event.key === Qt.Key_Menu && root._volumeOffered) {
            root.openVolumeControl()
            event.accepted = true
        }
    }

    Rectangle {
        id: surface
        anchors.fill: parent
        radius: AppTheme.radiusTile
        color: !root._drawsCard
               ? "transparent"
               : (root.focused ? AppTheme.selected : AppTheme.cardElevated)
        border.width: !root._drawsCard
                      ? 0 : (root.focused || root.activeFocus ? 2 : 1)
        border.color: root.activeFocus
                      ? AppTheme.focusRing
                      : (root.focused ? AppTheme.accentBorder : AppTheme.borderSubtle)

        // Live video, when there is any. Behind a Loader so voice-only tiles
        // build no VideoOutput. An unknown camera shows the avatar, not black.
        Loader {
            id: videoLoader
            anchors.fill: parent
            // Screen shares are video too. The local device receives no remote
            // stream, but the engine tees both captures into a self-view, so
            // local camera and screen tiles have video. A local camera tile
            // follows our own cameraOn rather than the SFU's later report about
            // us.
            active: root.identity.length > 0
                    && (root.mediaKind === "screen"
                        ? root.screenSharing
                        : (root.local
                            ? app.groupCall.cameraOn
                            : (root.cameraKnown && root.cameraOn)))
            visible: active && item && item.hasFrame
                     && !root.softwareRendererHidesVideo
            sourceComponent: Item {
                /// Keeps the avatar showing until the first frame arrives.
                readonly property bool hasFrame:
                    output.videoSink && output.videoSink.videoSize.width > 0

                VideoOutput {
                    id: output
                    anchors.fill: parent
                    // Shares fit (cropping hides their edges); cameras crop to
                    // fill.
                    fillMode: root.mediaKind === "screen"
                              ? VideoOutput.PreserveAspectFit
                              : VideoOutput.PreserveAspectCrop
                }

                // Attach on creation, release on destruction, and the release
                // names this sink, never a key. Qt creates a replacement
                // surface before deleteLater() destroys the old one (layout
                // swaps, Repeater regeneration on row moves), so a key-named
                // detach from the dying tile would unhook the live one for the
                // rest of the call.
                function attach() {
                    if (root.mediaKind === "screen") {
                        if (root.local)
                            app.groupCall.attachLocalScreenSink(output.videoSink);
                        else
                            app.groupCall.attachScreenSink(root.identity,
                                                           output.videoSink);
                    } else if (root.local) {
                        app.groupCall.attachLocalCameraSink(output.videoSink);
                    } else {
                        app.groupCall.attachVideoSink(root.identity,
                                                      output.videoSink);
                    }
                }
                function detach() {
                    // One verb, naming the sink: it can't name the wrong key,
                    // and once another surface has claimed the key there's
                    // nothing left to give up. (The key itself can change
                    // between creation and destruction.)
                    app.groupCall.detachSink(output.videoSink);
                }
                Component.onCompleted: attach()
                Component.onDestruction: detach()

                // No periodic re-arm (e.g. on participantsChanged): it would
                // also fire on a dying tile during a layout swap, letting it
                // reclaim the key from its successor and then release it,
                // blanking the live surface. Late keys are handled by
                // onActiveTrackKeyChanged.
            }
        }

        // The avatar and its ring.
        Item {
            id: avatarBlock
            // Hidden, not destroyed, while video is live, so the camera going
            // off doesn't flash an empty tile.
            visible: !videoLoader.visible
            anchors.centerIn: parent
            anchors.verticalCenterOffset: root.compact ? -6 : -8
            width: root._avatarSize
            height: root._avatarSize

            Rectangle {
                anchors.centerIn: parent
                // The only thing amplitude moves; a free child of the
                // fixed-size holder, so no layout is involved.
                width: parent.width + 2 * root.ringGap
                height: parent.height + 2 * root.ringGap
                radius: width / 2
                color: "transparent"
                border.width: root.bare ? 3 : 2
                border.color: AppTheme.success
                // Louder reads brighter, but never invisible while speaking (an
                // SFU may report no level).
                opacity: root.speaking
                         ? 0.55 + 0.45 * Math.max(0, Math.min(1, root.speakingLevel))
                         : 0
                visible: opacity > 0
                Behavior on opacity {
                    NumberAnimation { duration: 110 }
                }
            }

            Avatar {
                anchors.fill: parent
                mxc: root.avatarMxc
                // The real name, not "You".
                name: root._avatarName
                colorKey: root.userId
                size: root._avatarSize
            }
        }

        // ── Nameplate ── On a tile: a pill in the bottom-left with the mute
        // glyph inside it, ahead of the name, never hover-gated. A bare avatar
        // gets no pill. Behind a Loader because the label is empty until a
        // profile resolves.
        Loader {
            active: root._label.length > 0 && !root.bare
            visible: active
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: root.compact ? 6 : 8
            anchors.rightMargin: root.compact ? 6 : 8
            sourceComponent: Rectangle {
                id: namePlate
                objectName: "callTileNameplate"
                implicitWidth: Math.min(plate.implicitWidth + 12,
                                        surface.width - (root.compact ? 12 : 16))
                implicitHeight: plate.implicitHeight + 6
                radius: AppTheme.radiusPill
                // Translucent dark plate, legible over bright shares too.
                color: Qt.rgba(0, 0, 0, 0.55)

                RowLayout {
                    id: plate
                    objectName: "callTileNameplateRow"
                    // Anchored left and right so the width cap above applies to
                    // the row; centerIn alone let it take its full implicit
                    // width, so the name never elided. Not a loop: a Text's
                    // implicit width doesn't change when it elides.
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 6
                    anchors.rightMargin: 6
                    spacing: 4
                    Loader {
                        active: root.micKnown && root.micMuted
                        visible: active
                        Layout.alignment: Qt.AlignVCenter
                        sourceComponent: Icon {
                            name: "mic_off"
                            size: root.compact ? 12 : 14
                            color: AppTheme.dangerInk
                        }
                    }
                    Text {
                        // Remote or externally chosen text: never markup.
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: root._label
                        // A fixed light ink: the plate paints its own dark
                        // field over arbitrary video, so theme tokens don't
                        // apply.
                        color: "#FFFFFF"
                        font.pixelSize: root.compact ? 11 : 12
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                        maximumLineCount: 1
                    }
                }
            }
        }

        // The bare form's name: centred under the circle, no plate.
        Loader {
            active: root._label.length > 0 && root.bare && !videoLoader.visible
            visible: active
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: avatarBlock.bottom
            anchors.topMargin: 8
            width: parent.width - 12
            sourceComponent: Text {
                // Remote or externally chosen text: never markup.
                textFormat: Text.PlainText
                text: root._label
                color: AppTheme.stormText
                font.pixelSize: root.compact ? 11 : 13
                font.weight: Font.Medium
                elide: Text.ElideRight
                maximumLineCount: 1
                horizontalAlignment: Text.AlignHCenter
            }
        }

        // State badges, top-right, each in its own Loader so inactive badges
        // cost nothing. Never hover-gated.
        RowLayout {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: root.compact ? 6 : 8
            spacing: 4

            Loader {
                // Only a reported poor link earns a badge; "" means the SFU
                // never said.
                active: root.connectionQuality === "poor"
                visible: active
                sourceComponent: CallTileBadge {
                    iconName: "warning"
                    tone: "danger"
                }
            }
            Loader {
                active: root.handRaised
                visible: active
                sourceComponent: CallTileBadge {
                    iconName: "front_hand"
                    tone: "accent"
                }
            }
            Loader {
                active: root.screenSharing
                visible: active
                sourceComponent: CallTileBadge {
                    iconName: "screen_share"
                    tone: "accent"
                }
            }
            Loader {
                // Only an authoritative "camera off", and only on a tile: a
                // bare avatar already means no camera.
                active: root.cameraKnown && !root.cameraOn && !root.bare
                visible: active
                sourceComponent: CallTileBadge {
                    iconName: "videocam_off"
                    tone: "muted"
                }
            }
        }

        // The transient reaction, top-left. The volume button shares this
        // corner and keeps it (a control shouldn't move under a cursor reaching
        // for it), so the pill is offset by the button's actual width. In a
        // Loader because "" is the usual state (see the note at the top).
        Loader {
            active: root.reactionEmoji.length > 0
            visible: active
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.topMargin: root.compact ? 6 : 8
            anchors.leftMargin: (root.compact ? 6 : 8)
                                + (volumeAffordance.visible
                                   ? volumeAffordance.width + 4 : 0)
            sourceComponent: Rectangle {
                id: reactionPill
                objectName: "callTileReaction"
                implicitWidth: reactionGlyph.implicitWidth
                               + (root.compact ? 10 : 14)
                implicitHeight: reactionGlyph.implicitHeight
                                + (root.compact ? 4 : 6)
                radius: height / 2
                // Its own dark field, like the nameplate, since it sits over
                // video.
                color: Qt.rgba(0, 0, 0, 0.55)
                border.width: 1
                border.color: Qt.rgba(1, 1, 1, 0.18)

                Text {
                    id: reactionGlyph
                    anchors.centerIn: parent
                    // A remote string: plain text only. The emoji family is
                    // resolved in C++ (QML has no `font.families`, and Qt's
                    // automatic fallback picks a monochrome face on some
                    // versions).
                    textFormat: Text.PlainText
                    text: root.reactionEmoji
                    color: "#FFFFFF"
                    font.pixelSize: root.compact ? 15 : 20
                    font.family: (typeof app !== "undefined" && app
                                  && app.emojiFontFamily) || ""
                }

                // A short entrance; no exit animation, since the item
                // disappears with its data. Targeted by id (`target: parent`
                // would animate the Loader). With reduced motion it never runs.
                scale: 1.0
                opacity: 1.0
                ParallelAnimation {
                    running: !AppTheme.reducedMotion
                    NumberAnimation {
                        target: reactionPill
                        property: "scale"
                        from: 0.7
                        to: 1.0
                        duration: 140
                        easing.type: Easing.OutBack
                    }
                    NumberAnimation {
                        target: reactionPill
                        property: "opacity"
                        from: 0.0
                        to: 1.0
                        duration: 120
                    }
                }
            }
        }

        // Mute badge on a bare avatar, which has no nameplate, placed as in the
        // bubble strip.
        Loader {
            active: root.bare && root.micKnown && root.micMuted
                    && !videoLoader.visible
            visible: active
            anchors.right: avatarBlock.right
            anchors.bottom: avatarBlock.bottom
            sourceComponent: Rectangle {
                width: 22
                height: 22
                radius: 11
                color: AppTheme.stormCanvas
                border.width: 1
                border.color: AppTheme.stormBorder
                Icon {
                    anchors.centerIn: parent
                    name: "mic_off"
                    size: 14
                    color: AppTheme.danger
                }
            }
        }

        TapHandler {
            // Left button only: TapHandlers are non-exclusive across subtrees,
            // so taking every button would swallow presses meant for the stage.
            acceptedButtons: Qt.LeftButton
            onTapped: root.activated()
        }

        // Right-click opens the volume control; a separate handler for the same
        // reason as above.
        TapHandler {
            enabled: root._volumeOffered
            acceptedButtons: Qt.RightButton
            onTapped: root.openVolumeControl()
        }

        // ── The visible way in ── A hover-revealed button for the volume
        // control, top-left (badges own the top-right, the nameplate the
        // bottom-left). It keeps the corner over the reaction pill. Inactive
        // when not wanted, so idle grids build no buttons.
        Loader {
            id: volumeAffordance
            active: root._volumeOffered
                    && (tileHover.hovered || volumePopup.visible
                        || root.activeFocus)
            visible: active
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: root.compact ? 4 : 6
            // CallControlButton with a rounded-square ground, so it reads as a
            // control over bright video rather than a stray glyph.
            sourceComponent: CallControlButton {
                objectName: "callParticipantVolumeButton"
                diameter: root.compact ? 24 : 28
                glyphSize: root.compact ? 15 : 17
                cornerRadius: AppTheme.radiusMd
                // `volume_off` at zero: the person is muted for this device.
                // Both names are in Icon.qml's map (the bundled font is a
                // subset).
                iconName: {
                    var _ = root.volumeRevision;
                    return root.currentVolumePercent() > 0 ? "volume_up"
                                                           : "volume_off";
                }
                // CallControlButton uses the tooltip as its accessible name.
                tooltip: qsTr("Volume for %1").arg(root._label)
                onClicked: root.openVolumeControl()
            }
        }

        HoverHandler { id: tileHover }
    }

    // ── The volume popup ──
    // A child of the tile: a Popup owns its own overlay lifetime, so a
    // participant leaving mid-adjust takes the popup down with the tile.
    Popup {
        id: volumePopup
        objectName: "callParticipantVolumePopup"
        // Centred over its tile; Popup's margins keep it on screen.
        x: Math.round((root.width - width) / 2)
        y: Math.round((root.height - height) / 2)
        width: 268
        margins: 8
        padding: AppTheme.spacing12
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        // Read on open rather than bound (see currentVolumePercent()), so it
        // shows what the controller actually holds.
        onOpened: volumeSlider.value = root.currentVolumePercent()

        background: Rectangle {
            radius: AppTheme.radiusMd
            color: AppTheme.stormPanel
            border.width: 1
            border.color: AppTheme.stormBorder

            // A Popup doesn't consume presses on itself (blockInput() is false
            // for its own item) and `modal` only blocks presses outside it.
            // Without this sink, presses on the padding reach the tile's
            // TapHandlers beneath (re-spotlighting, or re-opening the popup).
            // In `background:`, below contentItem, so it only catches what the
            // content doesn't take.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
                // Hover too, or the tile beneath keeps reporting hover.
                hoverEnabled: true
            }
        }

        contentItem: ColumnLayout {
            spacing: AppTheme.spacing8

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                Icon {
                    name: volumeSlider.value > 0 ? "volume_up" : "volume_off"
                    size: 18
                    color: AppTheme.stormTextSecondary
                }
                Text {
                    // Remote or externally chosen text: never markup.
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    // No Loader: never empty (`_label` falls back to the user
                    // id, and `_volumeOffered` requires an identity).
                    text: root._label
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightMedium
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }
                Text {
                    objectName: "callParticipantVolumeReadout"
                    text: Math.round(volumeSlider.value) + "%"
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightMedium
                }
            }

            Slider {
                id: volumeSlider
                objectName: "callParticipantVolumeSlider"
                Layout.fillWidth: true
                from: 0
                // Up to 200: amplifying a quiet participant is the main use.
                to: 200
                stepSize: 1
                snapMode: Slider.SnapAlways
                Accessible.name: qsTr("Volume for %1").arg(root._label)

                // `onMoved`, never `onValueChanged`: the latter also fires for
                // the programmatic read in onOpened and would write the value
                // back on every open, recording the default the store
                // deliberately omits.
                onMoved: root.applyVolumePercent(value)

                background: Rectangle {
                    x: volumeSlider.leftPadding
                    y: volumeSlider.topPadding
                       + volumeSlider.availableHeight / 2 - 2
                    width: volumeSlider.availableWidth
                    height: 4
                    radius: AppTheme.radiusPill
                    color: AppTheme.stormInset

                    Rectangle {
                        width: volumeSlider.visualPosition * parent.width
                        height: parent.height
                        radius: AppTheme.radiusPill
                        color: AppTheme.bolt
                    }

                    // Marks the neutral point (100%) on the track.
                    Rectangle {
                        objectName: "callParticipantVolumeNeutralMark"
                        x: Math.round(parent.width / 2) - 1
                        y: -3
                        width: 2
                        height: parent.height + 6
                        radius: 1
                        color: AppTheme.stormTextMuted
                    }
                }
                handle: Rectangle {
                    x: volumeSlider.leftPadding
                       + volumeSlider.visualPosition
                         * (volumeSlider.availableWidth - width)
                    y: volumeSlider.topPadding
                       + volumeSlider.availableHeight / 2 - height / 2
                    width: 16
                    height: 16
                    radius: 8
                    // White, not boltInk: a dark thumb on the fill boundary
                    // reads as disabled. Same as the Settings sliders.
                    color: "#FFFFFF"
                    border.width: volumeSlider.visualFocus ? 2 : 0
                    border.color: AppTheme.bolt
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8

                Text {
                    Layout.fillWidth: true
                    // Preferred width 1 while filling: a wrapping Text reports
                    // its unwrapped width as preferred, and the RowLayout would
                    // squeeze the Reset button proportionally.
                    Layout.preferredWidth: 1
                    wrapMode: Text.WordWrap
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textMeta
                    // Always shown, so the consequence is disclosed before the
                    // user goes past 100.
                    text: qsTr("Above 100% amplifies and can clip. 200% applies the maximum the audio stage can reach.")
                }

                // Only when there is something to reset.
                Loader {
                    active: Math.round(volumeSlider.value) !== 100
                    visible: active
                    sourceComponent: AppButton {
                        objectName: "callParticipantVolumeReset"
                        storm: true
                        kind: "ghost"
                        text: qsTr("Reset")
                        onClicked: {
                            volumeSlider.value = 100;
                            // Assigning `value` isn't a user gesture, so
                            // onMoved won't fire; apply explicitly.
                            root.applyVolumePercent(100);
                        }
                    }
                }
            }
        }
    }
}
