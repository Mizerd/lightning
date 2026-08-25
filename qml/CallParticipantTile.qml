import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

// One participant on the call stage — Discord's layout, Lightning's tokens.
//
// Two shapes, and which one is drawn is NOT this component's decision:
//
//   * `bare` — a circular avatar on the stage's own canvas, no panel, no
//     border, no fill, with the name centred beneath it. This is what the
//     maintainer asked for ("bubbles of participats and their avatar").
//     HONESTY NOTE so nobody later "corrects" this against a screenshot:
//     CURRENT desktop Discord does not do this. Every participant there is a
//     rounded-rect tile with a circular avatar inside it, camera or not; the
//     free-standing circles are Discord's mobile call and its older desktop
//     DM call. The brief asked for circles-until-video, which is the older
//     shape, and it is a good one — but it is not "what Discord does today".
//   * a TILE — a 16:9 rounded rectangle with a nameplate pill, which is what
//     every participant becomes the moment ANYONE in the call turns on a
//     camera or starts a share, including the people who have neither.
//
// HONESTY RULE, and the whole reason `micKnown`/`cameraKnown` exist: the SFU
// reports a track's muted state only for tracks it knows about. Before a
// participant publishes, or for a device that never will, the state is
// genuinely UNKNOWN — and a boolean cannot say that. So a badge renders only
// when something authoritative said so, and unknown renders NOTHING rather
// than a confident, wrong "not muted".
//
// Delegate discipline: this is instantiated per participant, so every Label
// whose text can legitimately be empty lives behind a Loader. A never
// laid-out empty Text keeps ItemObservesViewport forever and makes Qt walk
// the whole instantiated tree on every scroll frame — the single most
// expensive QML mistake recorded in this repo.
Item {
    id: root

    property string userId: ""
    property string displayName: ""
    property string avatarMxc: ""

    property bool micKnown: false
    property bool micMuted: false
    property bool cameraKnown: false
    property bool cameraOn: false
    /// The SFU participant identity this tile shows. Routes video, and it
    /// is the only identifier that works for BOTH membership formats — the
    /// sticky form's identity is a hash, so it cannot be rebuilt from a user
    /// and device id.
    property string identity: ""
    property bool screenSharing: false
    property bool handRaised: false
    /// "" (unknown) | "poor" | "good" | "excellent". UNKNOWN DRAWS NOTHING —
    /// the SFU may never report quality for a participant, and an invented
    /// "good" is a claim nobody made.
    property string connectionQuality: ""

    /// Which of this participant's video tracks this surface shows:
    /// "camera" or "screen". A person can send BOTH at once, and one surface
    /// can only render one of them — the stage gives a share its own TILE
    /// (see CallShareTile) rather than replacing the person with it.
    property string mediaKind: "camera"
    /// Routing keys from the participant row, watched only so the sink is
    /// RE-ATTACHED when they change. The SFU can announce a participant
    /// before it says which media section their tracks landed on, and an
    /// attach made while the key was still empty never receives a frame.
    property string cameraTrackKey: ""
    property string screenTrackKey: ""
    readonly property string activeTrackKey: root.mediaKind === "screen" ? root.screenTrackKey : root.cameraTrackKey
    // Re-attach when the routing key finally arrives. Attaching once at
    // creation is not enough: the key can still be empty then, and an attach
    // under an empty key never receives a frame. Declared HERE rather than in
    // a Connections inside the Loader — the change signal of a property whose
    // name starts with an underscore is not reachable by an `on…Changed`
    // handler name, and a handler that never runs looks exactly like one that
    // does nothing.
    onActiveTrackKeyChanged: if (videoLoader.item)
        videoLoader.item.attach()

    /// Voice-activity ring, driven by the SFU's speaker updates.
    property bool speaking: false
    /// Amplitude, 0.0-1.0, from LiveKit's `SpeakerInfo.level`.
    ///
    /// THIS is "volume shows up as a circle arround user". Discord itself
    /// cannot do it — its voice gateway's speaking payload is a bitmask with
    /// no amplitude field at all — but LiveKit publishes a level and
    /// Lightning was throwing it away before CallParticipantModel existed.
    ///
    /// An SFU that publishes only `active` gives speaking=true, level=0.0,
    /// and the ring degrades to its fixed minimum. That is deliberate: no
    /// level is fabricated from the boolean, because a made-up amplitude
    /// would animate a number nobody measured.
    property real speakingLevel: 0.0
    /// This participant is the local device.
    property bool local: false
    /// Manually spotlighted.
    property bool focused: false
    /// Compact form for the strip beside a screen share.
    property bool compact: false
    /// Draw no card: just the avatar, its speaking ring and the name, on
    /// whatever the stage's canvas is. See the shape note at the top.
    ///
    /// Selection and keyboard focus still draw a card, because those are
    /// states the user caused and must be able to see.
    property bool bare: false
    readonly property bool _drawsCard:
        !root.bare || videoLoader.visible || root.focused || root.activeFocus

    signal activated()

    implicitWidth: compact ? 148 : 240
    implicitHeight: compact ? 96 : 168

    // ── The speaking ring ────────────────────────────────────────────────
    //
    // The ring is OUTSIDE the avatar and the avatar does not move: the ring
    // Rectangle is a CHILD of a fixed-size holder, so however far it breathes
    // it contributes nothing to this item's implicit size and reflows
    // nothing. Scaling the avatar instead would move every neighbour on every
    // syllable, which is the reasoning already written into
    // CallSpeakerBubbles.qml and it is correct.
    //
    // Attack fast, release slow, or a ring that follows amplitude strobes:
    // speech amplitude crosses zero between syllables.
    readonly property real ringTarget:
        root.speaking ? 3 + 6 * Math.max(0, Math.min(1, root.speakingLevel)) : 0
    property real ringGap: 0
    onRingTargetChanged: {
        ringMotion.duration = root.ringTarget > root.ringGap ? 60 : 220
        root.ringGap = root.ringTarget
    }
    // The binding's FIRST evaluation does not arrive as a change, so a tile
    // created while its owner is already talking would sit at gap 0 until
    // they paused.
    Component.onCompleted: root.ringGap = root.ringTarget
    Behavior on ringGap {
        NumberAnimation {
            id: ringMotion
            duration: 60
            easing.type: Easing.OutCubic
        }
    }

    readonly property int _avatarSize: {
        // Fit the avatar to the tile rather than to a fixed ladder, so the
        // grid stays sane from a 2-up 1:1 layout to a 12-up group.
        var box = Math.min(width, height - (compact ? 18 : 26))
        // A bare tile has no card to sit inside, so the avatar IS the tile
        // and takes the room a panel's padding would have used.
        var size = Math.round(box * (root.bare && !root.compact ? 0.74 : 0.52))
        return Math.max(compact ? 28 : 40,
                        Math.min(size, root.bare && !root.compact ? 148 : 96))
    }

    // "You" for the local device, but the AVATAR and colour key still come
    // from the real account — reported as: "I came in as You, should show my
    // avatar and display name". The label says who the tile is; it is not a
    // reason to draw a blank circle.
    readonly property string _label: root.local
                                     ? qsTr("You")
                                     : (root.displayName.length > 0
                                        ? root.displayName : root.userId)
    /// What the avatar draws initials from when there is no image: the real
    /// name, never the word "You" (which would render "Y" for everyone).
    readonly property string _avatarName: root.displayName.length > 0
                                          ? root.displayName
                                          : root.userId

    // ── Per-person playback volume ───────────────────────────────────────
    //
    // "if a user A sets user B volume to 70% it stays the same in next call
    // or other room." Discord's model exactly: the person, not the call.
    //
    // WHY IT IS NOT OFFERED ON THE LOCAL TILE. This is a PLAYBACK volume —
    // how loud this device renders that person. Nobody hears their own
    // published audio, so a slider on your own tile would move a number with
    // no audible effect and read as broken. What the local device controls is
    // the GAIN on what it SENDS, which lives with the other device settings
    // (CallDeviceSettings) because it is a property of this computer's
    // microphone, not of a call.
    //
    // WHERE THE VALUE COMES FROM. `SfuCallController` — both directions,
    // and never QSettings from here. The controller owns the mapping from the
    // SFU identity this tile holds to the Matrix user id the store is keyed
    // by, which is the whole reason the setting survives a rejoin: an
    // identity is per DEVICE (and, in the sticky membership format, an opaque
    // hash), so keying a stored preference by it would forget the choice the
    // moment the person came back.
    //
    // READ ON OPEN, not bound. A binding would need a tick to observe a C++
    // call, and there is nothing to observe: no remote party can change this
    // value, so the only writer while the popup is up is the slider itself.
    // Re-reading on every open is what makes it show 70 rather than always
    // 100 — the defect this replaces was a control with no read path at all.
    readonly property bool _volumeOffered:
        !root.local && root.identity.length > 0
        && typeof app !== "undefined" && app && app.groupCall

    /// This person's stored volume, or the 100% neutral point when nothing
    /// is known.
    ///
    /// `participantVolume()` and not the model's `volumePercent` role,
    /// deliberately: the invokable answers from the STORE, so it is right
    /// from the first frame a tile exists, whereas the role is only correct
    /// once the controller has seeded the row. Those two are the same number
    /// in the steady state and different exactly at the moment a call opens —
    /// which is when a person is most likely to reach for this.
    function currentVolumePercent() {
        if (!root._volumeOffered)
            return 100;
        var value = app.groupCall.participantVolume(root.identity);
        return (value === undefined || value === null) ? 100 : value;
    }

    /// Bumped by every write. Any binding that CALLS `currentVolumePercent()`
    /// must read this, because Qt cannot observe a C++ function call as a
    /// dependency — a binding without it evaluates once and then describes a
    /// value that has since changed. The repo has shipped that mistake often
    /// enough to have a name for the fix.
    property int volumeRevision: 0

    /// Apply a level. ONE writer, the controller — it drives the engine AND
    /// records the preference, so a surface cannot persist a value the audio
    /// path never received, or the reverse.
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
    // Carries the same facts the badges show, so a screen-reader user learns
    // what a sighted one does — and is told nothing when the state is
    // unknown. The speaking LEVEL is deliberately absent: an amplitude is
    // decoration, and announcing a number would be noise.
    Accessible.description: {
        var parts = []
        if (root.micKnown && root.micMuted)
            parts.push(qsTr("Microphone muted"))
        if (root.screenSharing)
            parts.push(qsTr("Sharing their screen"))
        if (root.handRaised)
            parts.push(qsTr("Hand raised"))
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
        // The context key reaches the volume control without a pointer. The
        // hover button is genuinely unreachable by keyboard — it is revealed
        // by hover — so without this the setting would be pointer-only, which
        // for a preference that persists is a worse gap than a missing hover
        // affordance.
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

        // Live video, when there is any.
        //
        // Behind a Loader so a voice-only tile builds no VideoOutput at all
        // — this is a per-participant delegate, and a grid of idle video
        // surfaces costs real GPU memory for nothing.
        //
        // `cameraOn` is only ever true when something authoritative said so
        // (see the honesty rule above), so an unknown camera shows the
        // avatar rather than a black rectangle.
        Loader {
            id: videoLoader
            anchors.fill: parent
            // A screen share is video as much as a camera is. Gating on
            // `cameraOn` alone meant a shared screen never rendered at all —
            // reported as "I did not see their screenshare".
            //
            // `local` is excluded because the engine publishes our own media
            // rather than receiving it: there is no remote stream for this
            // device — but the engine TEES both captures into a self-view
            // branch, so our own screen share AND our own camera do have
            // local video. Without the camera branch a local tile could only
            // ever show an avatar while the capture light was on, which read
            // as "the camera doesn't work".
            //
            // A local CAMERA tile follows our own cameraOn rather than the
            // SFU's report about us, which is the same fact arriving later.
            active: root.identity.length > 0
                    && (root.mediaKind === "screen"
                        ? root.screenSharing
                        : (root.local
                            ? app.groupCall.cameraOn
                            : (root.cameraKnown && root.cameraOn)))
            visible: active && item && item.hasFrame
            sourceComponent: Item {
                /// Nothing has arrived yet: the tile keeps showing the
                /// avatar instead of a black hole while the first frame is
                /// in flight.
                readonly property bool hasFrame:
                    output.videoSink && output.videoSink.videoSize.width > 0

                VideoOutput {
                    id: output
                    anchors.fill: parent
                    // A shared screen is CONTENT: cropping it hides the
                    // edges of what the other person is showing, which is
                    // usually where their toolbars and tabs are. A camera
                    // frame crops to fill because a letterboxed face in a
                    // grid cell looks broken.
                    fillMode: root.mediaKind === "screen"
                              ? VideoOutput.PreserveAspectFit
                              : VideoOutput.PreserveAspectCrop
                }

                // Attach on creation, RELEASE on destruction — and the
                // release names this SINK, never a key.
                //
                // The four key-named detaches this replaced were the whole of
                // "camera no longer works". Qt destroys a replaced surface
                // with deleteLater() while it creates the replacement
                // synchronously, so on every grid↔spotlight swap and on every
                // QQuickRepeater regenerate (which is how a Repeater answers
                // beginMoveRows — a participant reorder) the order is: new
                // tile attaches, THEN old tile detaches. A key-named detach
                // removed whatever was there, so the dying tile unhooked the
                // living one, and since attach() only runs on creation and on
                // an activeTrackKey change, NOTHING put it back for the rest
                // of the call.
                //
                // Before this round that was masked: the stage bound a JS
                // array rebuilt on every update, so every tile was destroyed
                // and re-created several times a second and re-attached
                // itself. The model that removed that churn is what exposed
                // this.
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
                    // ONE verb, naming the sink. It cannot name the wrong
                    // key, and once another surface has claimed what this one
                    // held there is nothing here left to give up — so a late
                    // destruction takes nothing with it.
                    //
                    // The branch this replaced could ALSO name the wrong key
                    // honestly: `local` and `mediaKind` are tile properties,
                    // and the key itself is derived from a track sid that
                    // arrives late — so a tile could compute a different key
                    // at destruction than it did at creation.
                    app.groupCall.detachSink(output.videoSink);
                }
                Component.onCompleted: attach()
                Component.onDestruction: detach()

                // NO periodic re-arm here, and that is a decision rather than
                // an omission.
                //
                // A `Connections { onParticipantsChanged: attach() }` was
                // written, analysed and REMOVED. It looks like free
                // self-healing — attachSink is an idempotent hash write — and
                // it would restore explicitly what the old
                // constantly-resetting surface used to provide by accident.
                // But it fires on a DYING tile too: between a layout swap and
                // the deferred delete that ends the old tile, both tiles are
                // alive and connected, so one participant update in that
                // window has the dying tile re-CLAIM the key from its
                // successor — and then its destruction releases it as the
                // rightful owner, leaving the live surface blank. That is the
                // exact defect this round exists to remove, reintroduced by
                // its own safety net.
                //
                // The late-key case it was meant to cover is already handled
                // where it belongs: `onActiveTrackKeyChanged` on the tile
                // re-attaches when the SFU finally names the track.
            }
        }

        // The avatar and its ring.
        Item {
            id: avatarBlock
            // Hidden, not destroyed, while video is live: the camera can go
            // off at any moment and rebuilding the avatar block then would
            // flash an empty tile.
            visible: !videoLoader.visible
            anchors.centerIn: parent
            anchors.verticalCenterOffset: root.compact ? -6 : -8
            width: root._avatarSize
            height: root._avatarSize

            Rectangle {
                anchors.centerIn: parent
                // The ONLY thing amplitude moves. `ringGap` is animated, so
                // this width follows it smoothly without any layout being
                // involved: the holder's size is fixed and this Rectangle is
                // a free child of it.
                width: parent.width + 2 * root.ringGap
                height: parent.height + 2 * root.ringGap
                radius: width / 2
                color: "transparent"
                border.width: root.bare ? 3 : 2
                border.color: AppTheme.success
                // Louder also reads as brighter, but never fully transparent
                // while speaking — an SFU that reports no level must still
                // show a ring.
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
                // The real name, not the "You" label: initials of "You"
                // would be a Y on the local tile and nothing recognisable.
                name: root._avatarName
                colorKey: root.userId
                size: root._avatarSize
            }
        }

        // ── Nameplate ────────────────────────────────────────────────────
        //
        // On a TILE it is a pill in the bottom-left with the mute glyph
        // INSIDE it, ahead of the name — the badge belongs to the name, and
        // a mute state a reader has to hover to discover is the single
        // most-complained-about thing about Discord's current call tile.
        // Nothing here is hover-gated.
        //
        // On a BARE avatar there is no pill: a filled pill under a
        // free-standing circle reads as a tile that failed to draw.
        //
        // Behind a Loader: the label is empty until a profile resolves,
        // which is the state this delegate is created in.
        Loader {
            active: root._label.length > 0 && !root.bare
            visible: active
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: root.compact ? 6 : 8
            anchors.rightMargin: root.compact ? 6 : 8
            sourceComponent: Rectangle {
                implicitWidth: Math.min(plate.implicitWidth + 12,
                                        surface.width - (root.compact ? 12 : 16))
                implicitHeight: plate.implicitHeight + 6
                radius: AppTheme.radiusPill
                // A translucent dark plate over whatever the tile is showing,
                // so the name stays legible on a bright screen share as well
                // as on the tile's own surface.
                color: Qt.rgba(0, 0, 0, 0.55)

                RowLayout {
                    id: plate
                    anchors.centerIn: parent
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
                        Layout.fillWidth: true
                        text: root._label
                        // Deliberately a fixed light ink, not a theme text
                        // token: this plate paints its own dark field over
                        // arbitrary video, so the surrounding theme says
                        // nothing about what is legible on it.
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
                text: root._label
                color: AppTheme.stormText
                font.pixelSize: root.compact ? 11 : 13
                font.weight: Font.Medium
                elide: Text.ElideRight
                maximumLineCount: 1
                horizontalAlignment: Text.AlignHCenter
            }
        }

        // State badges, top-right. Each in its own Loader so an inactive
        // badge costs nothing and contributes no empty Text. ALWAYS visible
        // — never hover-gated.
        RowLayout {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: root.compact ? 6 : 8
            spacing: 4

            Loader {
                // Only a REPORTED poor link earns a badge. "good" and
                // "excellent" are the ordinary case and drawing a badge for
                // them is decoration; "" means the SFU never said, and a
                // badge there would be a claim nobody made.
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
                // Only an authoritative "camera is off" earns a badge, and
                // only on a tile: a bare avatar IS the "no camera" state, so
                // the badge would be saying what the shape already says.
                active: root.cameraKnown && !root.cameraOn && !root.bare
                visible: active
                sourceComponent: CallTileBadge {
                    iconName: "videocam_off"
                    tone: "muted"
                }
            }
        }

        // A muted mic still has to be visible on a BARE avatar, where there
        // is no nameplate to carry it: a small badge on the circle itself,
        // exactly where the bubble strip puts it.
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
            // Left button only: TapHandlers are non-exclusive across
            // subtrees, so grabbing every button here would also swallow
            // presses meant for the stage beneath.
            acceptedButtons: Qt.LeftButton
            onTapped: root.activated()
        }

        // The volume gesture. RIGHT button, and it must be its own handler
        // with its own accepted button: TapHandlers are non-exclusive across
        // subtrees, so a single handler taking every button here would also
        // swallow right presses meant for the stage, and this repo has shipped
        // that collision three separate times.
        TapHandler {
            enabled: root._volumeOffered
            acceptedButtons: Qt.RightButton
            onTapped: root.openVolumeControl()
        }

        // ── The visible way in ───────────────────────────────────────────
        //
        // A right-click-only control is a control most people never find, so
        // the same action gets a button. HOVER-REVEALED and in the TOP-LEFT,
        // which is the one corner of this tile that is free: the state badges
        // own the top-right and the nameplate owns the bottom-left, and a
        // control that covers either would hide a fact to offer a preference.
        //
        // Behind a Loader, and the Loader is inactive when the button is not
        // wanted, so a grid of tiles nobody is pointing at builds no buttons.
        Loader {
            id: volumeAffordance
            active: root._volumeOffered
                    && (tileHover.hovered || volumePopup.visible
                        || root.activeFocus)
            visible: active
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: root.compact ? 4 : 6
            sourceComponent: IconButton {
                objectName: "callParticipantVolumeButton"
                size: root.compact ? "sm" : "md"
                storm: true
                // `volume_off` at zero is the honest glyph: a person turned
                // all the way down is muted FOR THIS DEVICE, and drawing a
                // speaker with waves would say the opposite. Both names are in
                // Icon.qml's map — the bundled Material Symbols font is a
                // SUBSET, and an unmapped name renders as tofu.
                iconName: {
                    var _ = root.volumeRevision;
                    return root.currentVolumePercent() > 0 ? "volume_up"
                                                           : "volume_off";
                }
                Accessible.name: qsTr("Volume for %1").arg(root._label)
                ToolTip.text: Accessible.name
                ToolTip.visible: hovered && !volumePopup.visible
                ToolTip.delay: 600
                onClicked: root.openVolumeControl()
            }
        }

        HoverHandler { id: tileHover }
    }

    // ── The volume popup ─────────────────────────────────────────────────
    //
    // A child of the TILE, deliberately, and this is the one place the
    // message-action-bar precedent does NOT apply. That crash came from a
    // per-row Loader's loaded Rectangle reparenting itself to
    // `Overlay.overlay` while the Loader stayed its destruction owner. A
    // Popup is not that: it owns its own overlay lifetime, exactly as the
    // timeline's details Dialog does, so a participant leaving mid-adjust
    // takes the popup down with the tile instead of leaving a dangling item
    // on the overlay.
    Popup {
        id: volumePopup
        objectName: "callParticipantVolumePopup"
        // Centred over the tile it belongs to, clamped into the window by
        // Popup's own margins so a tile at the edge of the grid does not put
        // its slider off screen.
        x: Math.round((root.width - width) / 2)
        y: Math.round((root.height - height) / 2)
        width: 268
        margins: 8
        padding: AppTheme.spacing12
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        // Read here rather than in a binding: see currentVolumePercent(). An
        // `onOpened` read also means a popup reopened after the controller
        // clamped or reset a value shows what the controller actually has,
        // not what this surface last sent.
        onOpened: volumeSlider.value = root.currentVolumePercent()

        background: Rectangle {
            radius: AppTheme.radiusMd
            color: AppTheme.stormPanel
            border.width: 1
            border.color: AppTheme.stormBorder

            // A POPUP DOES NOT CONSUME A PRESS THAT LANDS ON IT.
            //
            // `QQuickPopup::mousePressEvent` sets `accepted = blockInput()`,
            // and `blockInput()` returns FALSE when the item IS the popup
            // item — so delivery keeps walking down to whatever is behind the
            // overlay. `modal: true` would not help: it blocks presses
            // OUTSIDE a popup only, and this repo has already shipped one fix
            // resting on the opposite premise, which was inert.
            //
            // Behind this popup sit the tile's two TapHandlers. Without this
            // sink, a left press on the popup's padding reaches
            // `root.activated()` and re-spotlights the stage while the user is
            // reading a slider, and a right press reaches the volume handler
            // and re-opens the popup under itself. The Slider consumes its own
            // presses, which is exactly what makes the hole easy to miss:
            // dragging works, and only the surrounding chrome misbehaves.
            //
            // The sink belongs in `background:` — the bottom-most
            // hit-testable item, below `contentItem`, so it catches what the
            // content did not want and steals nothing the content did. NOT
            // fixed with `z`.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
                // Hover too: without it the tile beneath keeps reporting
                // hover through the popup.
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
                    Layout.fillWidth: true
                    // No Loader: this label is never empty. `_label` falls
                    // back to the user id, and the popup cannot be opened on
                    // a tile with neither, because `_volumeOffered` requires
                    // an identity.
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
                // 200, not 100: above the neutral point is real
                // amplification, which is the whole reason a per-person
                // control is worth having — a quiet participant is the case
                // it exists for.
                to: 200
                stepSize: 1
                snapMode: Slider.SnapAlways
                Accessible.name: qsTr("Volume for %1").arg(root._label)

                // `onMoved`, NEVER `onValueChanged`. `onMoved` fires only for
                // a USER gesture; `onValueChanged` also fires for the
                // programmatic read in `onOpened`, which would write the
                // value straight back to the controller on every open — a
                // pointless store write per open, and one that would defeat
                // the store's own "the default is not recorded" rule by
                // re-recording whatever was read.
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

                    // The neutral point, drawn ON the track. 100 is not the
                    // middle of a preference, it is the ONE value that
                    // changes nothing, and a slider whose default sits
                    // unmarked half way along reads as a range with no home.
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
                    // White, not boltInk: the thumb rides the fill BOUNDARY,
                    // so a dark disc reads as disabled past half range. Same
                    // reasoning as the Settings sliders.
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
                    wrapMode: Text.WordWrap
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textMeta
                    // ALWAYS shown, not revealed once the user is already
                    // past 100. A consequence disclosed only after the fact
                    // is not a disclosure. Stated flatly: what it does, and
                    // what it can cost.
                    text: qsTr("Above 100% amplifies and can clip.")
                }

                // Only when there is something to reset. A permanently
                // present Reset next to a control already at its default is
                // furniture.
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
                            // Set explicitly: assigning `value` is not a
                            // user gesture, so `onMoved` does not fire and
                            // the reset would otherwise change the picture
                            // and nothing else.
                            root.applyVolumePercent(100);
                        }
                    }
                }
            }
        }
    }
}
