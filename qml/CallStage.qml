import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The call surface — Discord's DM arrangement, Lightning's tokens.
//
// WHERE IT LIVES. This is a PANEL AT THE TOP of the conversation column, with
// the message list still visible and scrolling independently beneath it. It
// used to REPLACE the timeline entirely; the maintainer asked for the other
// thing ("calls get put at the top of the screen"), which is also what Discord
// does in a DM. The host (TimelinePane) owns the split and the divider; this
// component only asks to be collapsed.
//
// WHAT IT DRAWS.
//   * Voice only — circular avatars on the canvas, name centred beneath, a
//     speaking ring driven by AMPLITUDE.
//   * The moment anyone turns on a camera or starts a share, every
//     participant becomes a rounded-rect tile, including the people with
//     neither.
//   * A SHARE IS A TILE, NOT A MODE. Every live share is a cell in the grid
//     alongside the people. One person sharing with their camera on is two
//     cells; two sharers are two cells.
//
// THE LATCH THIS REPLACES. The old stage had `layoutMode`, and
// `effectiveLayout` returned it verbatim whenever it was not "auto". The only
// writer of anything else was "Back to grid" writing "grid" — and NOTHING ever
// wrote back, so one press made the share unreachable for the rest of the
// call. That is exactly "now if share is closed no way to get it back". There
// is no `layoutMode` here and no local layout state at all: what the stage
// shows is derived from `app.groupCall.stageState`, dismissal applies to the
// SPOTLIGHT and never to the share's existence, and the grid remains a
// complete index of everything on offer.
//
// THE INVARIANT, pinned by CallUiContractTest: while any share is live there
// is always at least one on-screen control that puts it back on the spotlight
// — the share's own tile in the grid, and the explicit "Show screen share"
// button in the header.
Rectangle {
    id: root

    objectName: "callStage"
    color: AppTheme.stormCanvas

    /// The participant list was asked for from the dock. The host (the
    /// timeline pane) decides where it opens — the stage has no side panel of
    /// its own to put it in.
    signal participantsRequested()

    /// The host owns the panel's HEIGHT, so collapsing is a request, not a
    /// local write: the stage cannot resize itself out of a SplitView it does
    /// not own.
    property bool collapsed: false
    signal collapseToggled()

    // ── The data layer. Models, bound directly. ──────────────────────────
    //
    // Never copied into a JS array: an array reassigned is a MODEL RESET, and
    // the speaker feed updates continuously while anybody talks, so the old
    // `participants()` + `refreshTick` shape destroyed every tile — and with
    // it every VideoOutput and its attach()/detach() pair — on every syllable.
    // An amplitude-driven ring is impossible on top of that, because the item
    // that would animate does not survive the update that drives it.
    readonly property var participantModel: app.groupCall.participantModel
    readonly property var shareModel: app.groupCall.shareModel
    readonly property var stageState: app.groupCall.stageState

    readonly property int peopleCount:
        root.participantModel ? root.participantModel.count : 0
    readonly property int shareCount:
        root.shareModel ? root.shareModel.count : 0

    // How many cameras are live. There is no aggregate property for this on
    // the model and a plain JS scan is NOT a binding — it cannot re-run when a
    // row changes. So the model's own change signals drive an explicit
    // recount, which reads the model authoritatively rather than trying to
    // track a delta.
    //
    // Cost, honestly: `dataChanged` also fires for speaker levels, so this
    // walks the participant list roughly as often as anyone speaks. A call has
    // tens of participants, and the walk is cheaper than one video frame. An
    // aggregate property on CallParticipantModel would remove it entirely and
    // is noted as a follow-up.
    property int camerasOn: 0
    function recountCameras() {
        var n = 0;
        if (root.participantModel) {
            for (var i = 0; i < root.participantModel.count; ++i) {
                var row = root.participantModel.get(i);
                if (row && row.cameraOn === true)
                    ++n;
            }
        }
        // Assigning the same value emits nothing, so a steady call stays quiet.
        root.camerasOn = n;
    }
    Connections {
        target: root.participantModel
        function onDataChanged() { root.recountCameras() }
        function onCountChanged() { root.recountCameras() }
    }
    Component.onCompleted: root.recountCameras()

    /// True when nothing in this call is sending video at all — the ordinary
    /// voice call, drawn as circular avatars on the canvas rather than a grid
    /// of empty panels.
    readonly property bool voiceOnly:
        root.shareCount === 0 && root.camerasOn === 0

    // ── What the spotlight is showing ────────────────────────────────────
    //
    // `indexOfShare`/`indexOfIdentity` are plain calls Qt cannot observe, so
    // each binding also READS the model's count: a row can only appear or
    // disappear with a membership change, and `spotlightShareId` /
    // `pinnedIdentity` notify on their own. These two answer "is there
    // anything to show?" — the actual RENDERING goes through a Repeater whose
    // delegate matches by id, so a track key that fills in late still reaches
    // the surface.
    readonly property int spotlightShareRow: {
        var _ = root.shareCount;
        if (!root.shareModel || !root.stageState
                || root.stageState.spotlightShareId.length === 0)
            return -1;
        return root.shareModel.indexOfShare(root.stageState.spotlightShareId);
    }
    readonly property int pinnedRow: {
        var _ = root.peopleCount;
        if (!root.participantModel || !root.stageState
                || root.stageState.pinnedIdentity.length === 0)
            return -1;
        return root.participantModel.indexOfIdentity(
                    root.stageState.pinnedIdentity);
    }
    readonly property bool spotlightHasSurface:
        root.spotlightShareRow >= 0 || root.pinnedRow >= 0

    /// Grid or spotlight. Derived — there is deliberately NO local override
    /// property here, because a local override with no writer back is exactly
    /// the latch this surface is replacing.
    readonly property string effectiveLayout: {
        if (!root.stageState)
            return "grid";
        if (root.stageState.layoutPreference === "grid")
            return "grid";
        if (root.stageState.layoutPreference === "spotlight")
            return "spotlight";
        // A screen share is the reason everyone is looking, so it takes the
        // stage automatically; likewise a manual pin.
        return (root.stageState.spotlightShareId.length > 0
                || root.stageState.pinnedIdentity.length > 0)
                ? "spotlight" : "grid";
    }

    /// THE way back. "Back to grid" DISMISSES the spotlighted share and drops
    /// the pin; it must never write a layout preference, which is what made
    /// the old exit one-way. Dismissal falls through to the next live share on
    /// its own, and the dismissed one is still a tile in the grid.
    function leaveSpotlight() {
        if (!root.stageState)
            return;
        if (root.stageState.spotlightShareId.length > 0)
            root.stageState.dismissShare(root.stageState.spotlightShareId);
        root.stageState.clearPin();
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: root.collapsed ? AppTheme.spacing8 : AppTheme.spacing12
        spacing: AppTheme.spacing8

        // ── Header: who, where, and the layout affordances ───────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            Icon {
                name: "call"
                size: 18
                color: AppTheme.accent
            }
            Text {
                Layout.fillWidth: !root.collapsed
                text: {
                    var n = root.peopleCount;
                    if (n <= 0)
                        return qsTr("Connecting…");
                    return n === 1 ? qsTr("1 person in call")
                                   : qsTr("%1 people in call").arg(n);
                }
                color: AppTheme.stormText
                font.pixelSize: 14
                font.weight: Font.Medium
                elide: Text.ElideRight
            }
            // Reconnecting and degraded states are SHOWN, never left as a
            // frozen picture — a call that looks fine while it is not is
            // the failure users report as "it just stopped".
            Loader {
                active: app.groupCall.state === SfuCallController.Reconnecting
                visible: active
                sourceComponent: Text {
                    text: qsTr("Reconnecting…")
                    color: AppTheme.warning
                    font.pixelSize: 12
                }
            }
            Loader {
                active: app.groupCall.mediaEncrypted
                visible: active
                sourceComponent: Icon {
                    name: "lock"
                    size: 14
                    color: AppTheme.success
                }
            }

            // Collapsed, the panel is a one-line strip: who is here, who is
            // talking, and the controls. The bubble row lives ONLY here —
            // expanded, the stage itself already draws every participant, and
            // a second row of the same faces above it was a redundant strip
            // that cost a whole line of the message list.
            Loader {
                Layout.fillWidth: root.collapsed
                Layout.preferredHeight: active ? implicitHeight : 0
                active: root.collapsed
                visible: active
                sourceComponent: CallSpeakerBubbles {}
            }

            // Compact controls while collapsed: the expanded dock is gone, so
            // without these a collapsed call would have no controls at all —
            // CallHeaderBar's header instance stands down whenever the stage
            // is on screen. Same component, same definition, third placement.
            Loader {
                active: root.collapsed
                visible: active
                Layout.preferredHeight: active ? implicitHeight : 0
                Layout.alignment: Qt.AlignVCenter
                sourceComponent: CallHeaderBar {
                    objectName: "callStageCollapsedDock"
                    placement: "dock"
                    compact: true
                    onParticipantsRequested: root.participantsRequested()
                }
            }

            // ── The way back to a dismissed share ────────────────────────
            //
            // Bound to `restorableShareAvailable`, which is true exactly when
            // a LIVE share is dismissed. This is the explicit half of the
            // invariant; the implicit half is that the share is still a tile
            // in the grid and clicking it restores it too.
            Loader {
                active: !root.collapsed && root.stageState
                        && root.stageState.restorableShareAvailable
                visible: active
                sourceComponent: AppButton {
                    objectName: "callRestoreShareButton"
                    size: "sm"
                    kind: "primary"
                    storm: true
                    text: root.stageState.dismissedShareCount > 1
                          ? qsTr("Show screen shares (%1)")
                            .arg(root.stageState.dismissedShareCount)
                          : qsTr("Show screen share")
                    onClicked: root.stageState.restoreAllShares()
                }
            }

            // Collapse / expand. Not a layout mode: it is the panel's own
            // size, and the HOST owns that.
            CallControlButton {
                objectName: "callCollapseButton"
                iconName: root.collapsed ? "open_in_full" : "close_fullscreen"
                diameter: 30
                glyphSize: 16
                tooltip: root.collapsed ? qsTr("Expand the call")
                                        : qsTr("Collapse the call")
                onClicked: root.collapseToggled()
            }
        }

        // ── The stage ────────────────────────────────────────────────────
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !root.collapsed

            // GRID — every surface the same size. Shares first, then people.
            Loader {
                anchors.fill: parent
                active: !root.collapsed && root.effectiveLayout === "grid"
                visible: active
                sourceComponent: CallTileGrid {
                    objectName: "callGrid"
                    shareModel: root.shareModel
                    participantModel: root.participantModel
                    voiceOnly: root.voiceOnly
                    // Clicking a share tile RESTORES it — that is the second,
                    // implicit way back, and it is why a dismissed share can
                    // never become unreachable.
                    onShareActivated: shareId => {
                        if (root.stageState)
                            root.stageState.restoreShare(shareId);
                    }
                    onParticipantActivated: identity => {
                        if (root.stageState)
                            root.stageState.pin(identity);
                    }
                }
            }

            // SPOTLIGHT — one large surface with everyone else in a strip
            // beneath, which is the arrangement that makes shared content
            // readable.
            Loader {
                anchors.fill: parent
                active: !root.collapsed && root.effectiveLayout === "spotlight"
                visible: active
                sourceComponent: ColumnLayout {
                    spacing: AppTheme.spacing8

                    Rectangle {
                        objectName: "callSpotlight"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: AppTheme.radiusTile
                        color: AppTheme.stormInset
                        border.width: 1
                        border.color: AppTheme.stormBorder
                        clip: true

                        // The spotlighted SHARE. A Repeater over the real
                        // model rather than a `get(row)` snapshot: the track
                        // key fills in late, and a snapshot taken before it
                        // arrives never attaches a sink.
                        Repeater {
                            model: root.shareModel
                            delegate: Loader {
                                id: spotShare
                                required property string shareId
                                required property string ownerIdentity
                                required property string ownerDisplayName
                                required property string trackKey
                                required property bool local

                                anchors.fill: parent
                                anchors.margins: 1
                                active: root.stageState
                                        && spotShare.shareId
                                           === root.stageState.spotlightShareId
                                visible: active
                                sourceComponent: CallShareTile {
                                    shareId: spotShare.shareId
                                    ownerIdentity: spotShare.ownerIdentity
                                    ownerDisplayName: spotShare.ownerDisplayName
                                    trackKey: spotShare.trackKey
                                    local: spotShare.local
                                    focused: true
                                    onActivated: root.leaveSpotlight()
                                }
                            }
                        }

                        // The pinned PERSON, when no share is spotlighted.
                        Repeater {
                            model: root.participantModel
                            delegate: Loader {
                                id: spotPerson
                                required property string identity
                                required property string userId
                                required property string displayName
                                required property string avatarMxc
                                required property bool local
                                required property bool micKnown
                                required property bool micMuted
                                required property bool cameraKnown
                                required property bool cameraOn
                                required property string cameraTrackKey
                                required property bool screenSharing
                                required property bool speaking
                                required property real speakingLevel
                                required property bool handRaised
                                required property string connectionQuality

                                anchors.fill: parent
                                anchors.margins: 1
                                active: root.stageState
                                        && root.spotlightShareRow < 0
                                        && spotPerson.identity
                                           === root.stageState.pinnedIdentity
                                visible: active
                                sourceComponent: CallParticipantTile {
                                    identity: spotPerson.identity
                                    userId: spotPerson.userId
                                    displayName: spotPerson.displayName
                                    avatarMxc: spotPerson.avatarMxc
                                    local: spotPerson.local
                                    micKnown: spotPerson.micKnown
                                    micMuted: spotPerson.micMuted
                                    cameraKnown: spotPerson.cameraKnown
                                    cameraOn: spotPerson.cameraOn
                                    cameraTrackKey: spotPerson.cameraTrackKey
                                    screenSharing: spotPerson.screenSharing
                                    mediaKind: "camera"
                                    speaking: spotPerson.speaking
                                    speakingLevel: spotPerson.speakingLevel
                                    handRaised: spotPerson.handRaised
                                    connectionQuality: spotPerson.connectionQuality
                                    focused: true
                                    onActivated: root.leaveSpotlight()
                                }
                            }
                        }

                        // Only when there is genuinely nobody to spotlight —
                        // a pinned participant who has left, for instance.
                        Loader {
                            anchors.centerIn: parent
                            active: !root.spotlightHasSurface
                            visible: active
                            sourceComponent: Text {
                                text: qsTr("Nobody to show here yet")
                                color: AppTheme.stormTextSecondary
                                font.pixelSize: 13
                            }
                        }

                        AppButton {
                            objectName: "callBackToGridButton"
                            storm: true
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.margins: AppTheme.spacing8
                            size: "sm"
                            text: qsTr("Back to grid")
                            // DISMISS, never a layout write. The dismissed
                            // share stays live, stays a row in the model and
                            // stays a tile in the grid — so this exit is not
                            // a door that locks behind you.
                            onClicked: root.leaveSpotlight()
                        }
                    }

                    // Strip of every OTHER surface, compact. Excluded BY
                    // shareId (never by identity alone): the router holds one
                    // screen sink per participant, so the spotlighted share
                    // must not also be drawn here — but a sharer's CAMERA is a
                    // different track and legitimately stays in the strip,
                    // which the old identity-based exclusion got wrong.
                    Flickable {
                        objectName: "callStrip"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 96
                        contentWidth: stripRow.width
                        contentHeight: height
                        flickableDirection: Flickable.HorizontalFlick
                        boundsBehavior: Flickable.StopAtBounds
                        clip: true

                        Row {
                            id: stripRow
                            height: parent.height
                            spacing: AppTheme.spacing8

                            Repeater {
                                model: root.shareModel
                                delegate: Loader {
                                    id: stripShare
                                    required property string shareId
                                    required property string ownerIdentity
                                    required property string ownerDisplayName
                                    required property string trackKey
                                    required property bool local

                                    height: 88
                                    width: active ? 140 : 0
                                    active: !root.stageState
                                            || stripShare.shareId
                                               !== root.stageState.spotlightShareId
                                    visible: active
                                    sourceComponent: CallShareTile {
                                        compact: true
                                        shareId: stripShare.shareId
                                        ownerIdentity: stripShare.ownerIdentity
                                        ownerDisplayName: stripShare.ownerDisplayName
                                        trackKey: stripShare.trackKey
                                        local: stripShare.local
                                        onActivated: {
                                            if (root.stageState)
                                                root.stageState.restoreShare(
                                                    stripShare.shareId);
                                        }
                                    }
                                }
                            }

                            Repeater {
                                model: root.participantModel
                                delegate: Loader {
                                    id: stripPerson
                                    required property string identity
                                    required property string userId
                                    required property string displayName
                                    required property string avatarMxc
                                    required property bool local
                                    required property bool micKnown
                                    required property bool micMuted
                                    required property bool cameraKnown
                                    required property bool cameraOn
                                    required property string cameraTrackKey
                                    required property bool screenSharing
                                    required property bool speaking
                                    required property real speakingLevel
                                    required property bool handRaised
                                    required property string connectionQuality

                                    height: 88
                                    width: active ? 140 : 0
                                    // Only the person whose CAMERA is on the
                                    // spotlight is removed from the strip.
                                    active: root.spotlightShareRow >= 0
                                            || !root.stageState
                                            || stripPerson.identity
                                               !== root.stageState.pinnedIdentity
                                    visible: active
                                    sourceComponent: CallParticipantTile {
                                        compact: true
                                        identity: stripPerson.identity
                                        userId: stripPerson.userId
                                        displayName: stripPerson.displayName
                                        avatarMxc: stripPerson.avatarMxc
                                        local: stripPerson.local
                                        micKnown: stripPerson.micKnown
                                        micMuted: stripPerson.micMuted
                                        cameraKnown: stripPerson.cameraKnown
                                        cameraOn: stripPerson.cameraOn
                                        cameraTrackKey: stripPerson.cameraTrackKey
                                        screenSharing: stripPerson.screenSharing
                                        mediaKind: "camera"
                                        speaking: stripPerson.speaking
                                        speakingLevel: stripPerson.speakingLevel
                                        handRaised: stripPerson.handRaised
                                        connectionQuality: stripPerson.connectionQuality
                                        onActivated: {
                                            if (root.stageState)
                                                root.stageState.pin(
                                                    stripPerson.identity);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // The control dock: ONE control surface, at the bottom of the stage,
        // where a call client puts it.
        //
        // It is the SAME component as the header bar, in its "dock"
        // placement — not a second control bar. That distinction is the whole
        // lesson of this surface: the stage used to carry a partial bar of
        // its own, and when the media controls moved to the header what was
        // left were two orphan buttons floating under the call UI. There is
        // one definition of the control set; the header instance hides itself
        // while this stage is showing (CallHeaderBar.stageOwnsControls), so
        // the controls are never drawn twice.
        Loader {
            Layout.fillWidth: true
            Layout.topMargin: AppTheme.spacing4
            active: !root.collapsed
            visible: active
            sourceComponent: CallHeaderBar {
                objectName: "callStageDock"
                placement: "dock"
                onParticipantsRequested: root.participantsRequested()
            }
        }
    }
}
