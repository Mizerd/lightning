import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import MatrixClient

// The call surface: a panel at the top of the conversation column, with the
// timeline still visible and scrolling beneath it. The host (TimelinePane)
// owns the split and the divider; this component only requests collapse.
//
// Voice-only calls draw circular avatars with an amplitude-driven speaking
// ring. Once anyone has a camera or share on, every participant becomes a
// tile. A share is a tile, not a mode: one person sharing with their camera
// on is two cells.
//
// There is no local layout state. What the stage shows is derived from
// `app.groupCall.stageState`; dismissal affects the spotlight, never the
// share's existence. Invariant (CallUiContractTest): while any share is live
// there is always an on-screen control that puts it back on the spotlight —
// its grid tile and the header's "Show screen share" button.
Rectangle {
    id: root

    objectName: "callStage"
    color: AppTheme.stormCanvas

    /// The participant list was requested from the dock; the host decides where
    /// it opens.
    signal participantsRequested()

    /// The host owns the panel's height, so collapsing is a request.
    property bool collapsed: false
    signal collapseToggled()

    // ── Data layer ──
    // Models are bound directly, never copied into a JS array: reassigning an
    // array is a model reset, which would rebuild every tile (and its
    // VideoOutput) on every speaker-level update.
    readonly property var participantModel: app.groupCall.participantModel
    readonly property var shareModel: app.groupCall.shareModel
    readonly property var stageState: app.groupCall.stageState

    readonly property int peopleCount:
        root.participantModel ? root.participantModel.count : 0
    readonly property int shareCount:
        root.shareModel ? root.shareModel.count : 0

    // Number of live cameras. A JS scan isn't a binding, so the model's change
    // signals drive an explicit recount. dataChanged also fires for speaker
    // levels; the walk is cheap for call-sized lists.
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
        // Assigning the same value emits nothing.
        root.camerasOn = n;
    }
    Connections {
        target: root.participantModel
        function onDataChanged() { root.recountCameras() }
        function onCountChanged() { root.recountCameras() }
    }
    Component.onCompleted: {
        root.recountCameras();
        // The host's Loader rebuilds the stage on every room change while the
        // call outlives it, so full screen may already be on at creation.
        root.syncFullScreenWindow();
    }

    /// True when nothing in the call sends video: drawn as avatars on the
    /// canvas rather than a grid of empty panels.
    readonly property bool voiceOnly:
        root.shareCount === 0 && root.camerasOn === 0

    // ── What the spotlight shows ── indexOfShare/indexOfIdentity are plain
    // calls Qt can't observe, so each binding also reads the model's count.
    // These only answer "is there anything to show?"; rendering goes through a
    // Repeater matching by id so a late track key still reaches the surface.
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

    /// Grid or spotlight. Derived only; a local override with no writer back
    /// would become a one-way latch.
    readonly property string effectiveLayout: {
        if (!root.stageState)
            return "grid";
        if (root.stageState.layoutPreference === "grid")
            return "grid";
        if (root.stageState.layoutPreference === "spotlight")
            return "spotlight";
        // A share or a manual pin takes the stage automatically.
        return (root.stageState.spotlightShareId.length > 0
                || root.stageState.pinnedIdentity.length > 0)
                ? "spotlight" : "grid";
    }

    /// Smallest tile strip worth drawing, and the bubble row that replaces it.
    /// Below an 80 px band the tile's avatar and nameplate collide; the bubble
    /// row carries the same information at 44 px (as in the collapsed strip).
    readonly property int minimumTileStrip: 80
    readonly property int bubbleStripHeight: 44

    /// Height of the spotlight's top-right overlay controls (Full screen, Back
    /// to grid): a 30 px button plus margins. Below this the row overflows.
    readonly property int spotlightOverlayHeight: 30 + 2 * AppTheme.spacing8

    /// The shortest this stage can be and still be useful. Read by the host,
    /// which owns the height but not the chrome sizes declared here.
    /// `minimumPictureHeight` makes it a policy: fitting only the chrome is not
    /// enough.
    readonly property int minimumPictureHeight: 132
    readonly property int minimumUsefulHeight:
        2 * AppTheme.spacing12          // the column's own margins
        // The header row, which carries the controls: the full dock's 48 px
        // buttons plus pill padding (see CallHeaderBar's controlDiameter).
        + 48 + 2 * AppTheme.spacing8 + AppTheme.spacing8
        + root.minimumPictureHeight
        // No dock term: there is no bottom dock.

    /// What the strip beneath the spotlight is for a stage of `available` px:
    /// "tiles" or "bubbles". The strip may take at most 40% of the stage so the
    /// share keeps most of the height (display scaling makes this matter); when
    /// 40% can't fit a usable tile it becomes the bubble row, which carries the
    /// same faces, badges and pin-on-click.
    function stripModeForStage(available) {
        if (!available || available <= 0)
            return "tiles";
        return Math.floor((available - AppTheme.spacing8) * 0.4)
                >= root.minimumTileStrip ? "tiles" : "bubbles";
    }

    /// Whether participant faces are drawn in the header instead of a strip
    /// under the spotlight, so a share keeps that height. Both hosts read this,
    /// so they can't both draw the faces.
    readonly property bool bubblesInHeader:
        !root.collapsed && !root.fullScreenActive
        && root.effectiveLayout === "spotlight"

    /// Strip height, capped at 96. A function so tests can call it;
    /// CallUiContractTest also pins the call site.
    function stripHeightForStage(available) {
        if (!available || available <= 0)
            return 96;
        if (root.stripModeForStage(available) !== "tiles")
            return root.bubbleStripHeight;
        return Math.min(96,
                        Math.floor((available - AppTheme.spacing8) * 0.4));
    }

    /// "Back to grid": dismisses the spotlighted share and drops the pin. It
    /// must never write a layout preference, or the exit becomes one-way. The
    /// next live share takes over and the dismissed one stays in the grid.
    function leaveSpotlight() {
        if (!root.stageState)
            return;
        if (root.stageState.spotlightShareId.length > 0)
            root.stageState.dismissShare(root.stageState.spotlightShareId);
        root.stageState.clearPin();
    }

    // ── Chrome that gets out of the way ──
    // Idle means the pointer hasn't moved, not that it left the window (on a
    // single monitor it rests inside the share). Both the spotlight overlay and
    // the full-screen window's chrome follow this.
    readonly property int idleTickMs: 500
    readonly property int idleTicksToHide: 6   // 6 x 500 ms = 3 s

    /// Ticks since the pointer last moved over the spotlight. The full-screen
    /// window keeps its own count; HoverHandler only sees its own window.
    property int stageIdleTicks: 0
    readonly property bool stageChromeIdle:
        root.spotlightHasSurface && !root.fullScreenActive
        && root.stageIdleTicks >= root.idleTicksToHide

    // ── Full screen ── The focused surface on a whole monitor, in its own
    // Window (an overlay can only fill the app window). Declared inside this
    // Item so it is transient for the main window and dies with the stage. Not
    // gated on `collapsed`.
    //
    // Full screen must never be empty: CallStageState refuses to enter without
    // a focused surface and drops the flag when the spotlight empties; this
    // binding checks the resolved rows as well.
    readonly property bool fullScreenActive:
        root.stageState ? (root.stageState.fullScreen
                           && root.spotlightHasSurface)
                        : false

    function enterFullScreen() {
        if (root.stageState)
            root.stageState.setFullScreen(true);
    }
    function exitFullScreen() {
        if (root.stageState)
            root.stageState.setFullScreen(false);
    }

    /// One level back: leaves full screen, otherwise leaves the spotlight.
    /// Entering full screen is only ever a button.
    function focusedSurfaceActivated() {
        if (root.fullScreenActive)
            root.exitFullScreen();
        else
            root.leaveSpotlight();
    }

    // Driven imperatively rather than binding Window.visibility, which the
    // window manager writes on close and would break the binding. `onClosing`
    // accepts the close (refusing would veto Ctrl+Q) and writes the flag back.
    function syncFullScreenWindow() {
        // The flag can change while this component is still being built, when
        // the window id still reads null.
        if (!fullScreenWindow)
            return;
        if (root.fullScreenActive) {
            root.placeOnThisApplicationsScreen();
            fullScreenWindow.showFullScreen();
            fullScreenSurface.forceActiveFocus();
            if (root.stageState && root.stageState.traceEnabled) {
                // Where it actually landed, to compare with the request.
                console.info("call-fullscreen landed on="
                             + (fullScreenWindow.screen
                                ? fullScreenWindow.screen.name : "?"));
            }
        } else if (fullScreenWindow.visible) {
            fullScreenWindow.hide();
        }
    }

    // Never restarted: only the count resets. Re-phasing the timer while the
    // count is at the budget would hide the chrome again on the next tick.
    Timer {
        id: stageIdleTimer
        objectName: "stageIdleTimer"
        interval: root.idleTickMs
        repeat: true
        running: root.spotlightHasSurface && !root.fullScreenActive
        onTriggered: root.stageIdleTicks += 1
    }

    /// Pointer movement over the stage wakes its chrome. Compare positions
    /// rather than trusting `pointChanged`: Qt re-delivers hover at the same
    /// position when the item under the pointer changes (as hiding the chrome
    /// does), which would otherwise reset the count in a loop.
    property real lastPointerX: -1
    property real lastPointerY: -1
    HoverHandler {
        id: stageHover
        onPointChanged: {
            const p = stageHover.point.scenePosition;
            if (Math.abs(p.x - root.lastPointerX) < 1
                && Math.abs(p.y - root.lastPointerY) < 1)
                return;
            root.lastPointerX = p.x;
            root.lastPointerY = p.y;
            root.stageIdleTicks = 0;
        }
    }

    /// Put the full-screen window on the monitor the application is on. A QML
    /// Window doesn't inherit its transient parent's screen and
    /// showFullScreen() picks none, so it would default to the primary screen.
    /// Setting `screen` alone isn't enough: QWindowPrivate::create() re-derives
    /// the screen from geometry, so the geometry must be set in virtual-desktop
    /// coordinates too. `Screen` is attached to this item so it follows the app
    /// across monitors.
    ///
    /// On Wayland the compositor chooses (QTBUG-54883). NOT TESTED on X11 or
    /// Wayland; LIGHTNING_CALL_TRACE=1 logs the requested and actual screens.
    function placeOnThisApplicationsScreen() {
        var target = root.Screen;
        if (!target)
            return;
        // Before the first show, since create() reads the geometry.
        fullScreenWindow.screen = target;
        fullScreenWindow.x = target.virtualX;
        fullScreenWindow.y = target.virtualY;
        fullScreenWindow.width = target.width;
        fullScreenWindow.height = target.height;
        if (root.stageState && root.stageState.traceEnabled) {
            // Distinguishes "asked for the wrong monitor" from "the compositor
            // chose". Names and numbers only.
            console.info("call-fullscreen"
                         + " platform=" + Qt.platform.pluginName
                         + " appScreen=" + target.name
                         + " virtualX=" + target.virtualX
                         + " virtualY=" + target.virtualY
                         + " size=" + target.width + "x" + target.height);
        }
    }
    onFullScreenActiveChanged: root.syncFullScreenWindow()

    // ── The focused surface, defined once ──
    // Hosted by the spotlight or the full-screen window, never both: the router
    // holds one sink per key, so two surfaces would fight over it. Going full
    // screen rebuilds the video item (items can't move between scene graphs);
    // SfuVideoRouter's ownership rule (new surface claims the key, old one
    // releases by sink) keeps that rebuild from blanking the video.
    Component {
        id: focusedSurface

        Item {
            // A Repeater over the model rather than a get(row) snapshot: the
            // track key fills in late.
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
                        onActivated: root.focusedSurfaceActivated()
                    }
                }
            }

            // The pinned person, when no share is spotlighted.
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
                    // Transient and usually "", hence the tile's Loader.
                    required property string reactionEmoji
                    required property string connectionQuality

                    anchors.fill: parent
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
                        reactionEmoji: spotPerson.reactionEmoji
                        connectionQuality: spotPerson.connectionQuality
                        focused: true
                        onActivated: root.focusedSurfaceActivated()
                    }
                }
            }

            // Only when nobody can be spotlighted, e.g. a pinned participant
            // who left.
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
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: root.collapsed ? AppTheme.spacing8 : AppTheme.spacing12
        spacing: AppTheme.spacing8

        // ── Header: who, where, and the controls ──
        RowLayout {
            // Named: the dock's compaction latch asks how much room the row
            // could give it (see reassessControlRoom()).
            id: callHeaderRow
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            Icon {
                name: "call"
                size: 18
                color: AppTheme.accent
            }
            Text {
                Layout.fillWidth: !root.collapsed
                // Ask for 1 px while filling: a RowLayout squeezes children in
                // proportion to preferred width, and a long title would
                // otherwise squeeze the controls (a squeezed AppButton draws
                // its label over its neighbour). The title elides; controls
                // can't.
                Layout.preferredWidth: root.collapsed ? implicitWidth : 1
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
            // Reconnecting and degraded states are shown, never left as a
            // frozen picture.
            Loader {
                active: app.groupCall.state === SfuCallController.Reconnecting
                visible: active
                sourceComponent: Text {
                    text: qsTr("Reconnecting…")
                    color: AppTheme.warning
                    font.pixelSize: 12
                }
            }
            // The padlock turns into a warning while remote media is being
            // dropped for lack of a usable key, rather than reassuring over a
            // participant you can't hear.
            Loader {
                active: app.groupCall.mediaEncrypted
                visible: active
                sourceComponent: Icon {
                    // "warning": the icon map has no lock-with-alert, and an
                    // unknown name renders nothing.
                    name: app.groupCall.remoteMediaBlocked
                          ? "warning" : "lock"
                    size: 14
                    color: app.groupCall.remoteMediaBlocked
                           ? AppTheme.warning : AppTheme.success
                    ToolTip.visible: lockHover.hovered
                    ToolTip.delay: 400
                    ToolTip.text: app.groupCall.remoteMediaBlocked
                        ? qsTr("This call is encrypted, but media from "
                               + "someone here cannot be decrypted and is "
                               + "being dropped. You will not hear or see "
                               + "them.")
                        : qsTr("This call is end-to-end encrypted.")
                    HoverHandler { id: lockHover }
                }
            }

            // Our own capture is publishing nothing audible. Nothing else in
            // the UI shows this: silence encodes and encrypts like speech, so
            // transport, padlock and frame counters all look healthy.
            Loader {
                objectName: "callHeaderMicSilentBadge"
                active: app.groupCall.microphoneSilent
                visible: active
                sourceComponent: Icon {
                    name: "warning"
                    size: 14
                    color: AppTheme.warning
                    ToolTip.visible: micSilentHover.hovered
                    ToolTip.delay: 400
                    ToolTip.text: qsTr("Your microphone is not picking "
                                       + "anything up, so nobody here can "
                                       + "hear you. Check which microphone "
                                       + "is selected in call settings.")
                    HoverHandler { id: micSilentHover }
                }
            }

            // The bubble row: the whole strip when collapsed, or beside a
            // spotlight. Otherwise the stage already draws every participant.
            Loader {
                objectName: "callHeaderBubblesHost"
                // Collapsed it fills; beside a spotlight it takes only what the
                // faces need.
                Layout.fillWidth: root.collapsed
                Layout.preferredHeight: active ? implicitHeight : 0
                Layout.alignment: Qt.AlignVCenter
                active: root.collapsed || root.bubblesInHeader
                visible: active
                sourceComponent: CallSpeakerBubbles {
                    objectName: "callHeaderBubbles"
                    model: root.participantModel
                    onActivated: identity => {
                        if (root.stageState)
                            root.stageState.pin(identity);
                    }
                }
            }

            // The call controls, in the header both collapsed and expanded. The
            // header bar instance stands down while the stage is on screen
            // (CallHeaderBar.stageOwnsControls), so they are never drawn twice.
            Loader {
                id: controlsHost
                active: true
                visible: true
                Layout.preferredHeight: implicitHeight
                // The cell must be squeezable, or on a narrow window the
                // RowLayout overflows to the right and pushes the end of the
                // control row (chevrons, collapse, hang-up) out of the panel.
                // The bar answers a squeeze by going compact.
                //
                // `Layout.minimumWidth: 0` alone does nothing: without
                // fillWidth an item's horizontal size policy is fixed (min =
                // preferred = max). The maximum caps growth at the natural size
                // so wide windows are unchanged.
                Layout.minimumWidth: 0
                Layout.fillWidth: true
                Layout.maximumWidth: implicitWidth
                Layout.alignment: Qt.AlignVCenter

                // A latch, not a binding: `compact` changes implicitWidth, so
                // `compact: width < implicitWidth` would oscillate. The
                // expanded requirement is learned while expanded and frozen on
                // the way into compact; coming back out asks whether that
                // measured width now fits.
                property bool cramped: false
                property real expandedNeed: 0

                // Room this cell could have, which is not its own width: the
                // maximumWidth cap pins our width to the compact size, so the
                // latch would never release. What the other cells need (row
                // implicit minus ours) is the same in both shapes.
                //
                // A function, not a property: as a binding it depends on the
                // row's implicitWidth, which depends on ours, causing a binding
                // loop that stops the latch. Because our width is pinned while
                // cramped, the row's width changes are listened for below.
                function roomAvailable() {
                    return callHeaderRow.width
                           - (callHeaderRow.implicitWidth - implicitWidth);
                }

                Connections {
                    target: callHeaderRow
                    function onWidthChanged() {
                        controlsHost.reassessControlRoom();
                    }
                    function onImplicitWidthChanged() {
                        controlsHost.reassessControlRoom();
                    }
                }

                function reassessControlRoom() {
                    if (!item)
                        return;
                    if (!cramped) {
                        // Only record the requirement in the shape we want to
                        // return to (not cramped, not collapsed; the collapsed
                        // strip is much narrower).
                        if (!root.collapsed)
                            expandedNeed = implicitWidth;
                        // Both directions ask the same question via
                        // roomAvailable(), which errs towards "stay as you are"
                        // when the row hasn't caught up; comparing `width` on
                        // entry would re-trigger immediately after a release.
                        // The 0.5 is sub-pixel slack.
                        if (roomAvailable() + 0.5 < implicitWidth)
                            cramped = true;
                    } else if (expandedNeed > 0
                               && roomAvailable() + 0.5 >= expandedNeed) {
                        cramped = false;
                    }
                }
                onWidthChanged: reassessControlRoom()
                onImplicitWidthChanged: reassessControlRoom()
                Component.onCompleted: reassessControlRoom()
                sourceComponent: CallHeaderBar {
                    objectName: "callStageControls"
                    placement: "dock"
                    // Compact while collapsed, or when the window can't fit the
                    // full bar. `compact` also hides Share, Raise hand and the
                    // device chevrons; those have other routes, whereas hang-up
                    // must always stay visible.
                    compact: root.collapsed || controlsHost.cramped
                    onParticipantsRequested: root.participantsRequested()
                }
            }

            // ── The way back to a dismissed share ── The explicit half of the
            // invariant; the grid tile is the implicit one.
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

            // Collapse/expand: the panel's size, owned by the host.
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

        // ── Why there is no picture ── With Qt Quick's software renderer there
        // is no video node, so calls carry audio but draw no frames. The tiles
        // stay in their placeholder state (see `softwareRendererHidesVideo`)
        // and this one line explains why. Zero height on machines with a usable
        // GPU.
        Rectangle {
            objectName: "callSoftwareRendererNotice"
            // Not while collapsed: the collapsed panel has a fixed height and
            // this would overflow it.
            visible: !root.collapsed
                     && typeof app !== "undefined" && app
                     && app.softwareRenderer === true
            Layout.fillWidth: true
            implicitHeight: visible ? noticeRow.implicitHeight
                                      + AppTheme.spacing8 * 2 : 0
            color: AppTheme.warningFill
            radius: AppTheme.radiusSm

            RowLayout {
                id: noticeRow
                anchors.fill: parent
                anchors.margins: AppTheme.spacing8
                spacing: AppTheme.spacing8

                Icon {
                    name: "warning"
                    size: 16
                    color: AppTheme.warning
                    Layout.alignment: Qt.AlignVCenter
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: AppTheme.stormText
                    font.pixelSize: 12
                    // One string literal: lupdate cannot extract qsTr("a" +
                    // "b").
                    text: qsTr("Video can't be shown on this computer — there is no working graphics acceleration, so cameras and shared screens won't appear. Audio is unaffected.")
                }
            }
        }

        // ── The stage ────────────────────────────────────────────────────
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !root.collapsed

            // Grid: every surface the same size, shares first. Stood down in
            // full screen so two tiles never compete for one routing key.
            Loader {
                anchors.fill: parent
                active: !root.collapsed && !root.fullScreenActive
                        && root.effectiveLayout === "grid"
                visible: active
                sourceComponent: CallTileGrid {
                    objectName: "callGrid"
                    shareModel: root.shareModel
                    participantModel: root.participantModel
                    voiceOnly: root.voiceOnly
                    // Clicking a share tile restores it.
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

            // Spotlight: one large surface with everyone else in a strip
            // beneath.
            Loader {
                anchors.fill: parent
                active: !root.collapsed && !root.fullScreenActive
                        && root.effectiveLayout === "spotlight"
                visible: active
                sourceComponent: ColumnLayout {
                    id: spotlightColumn
                    spacing: AppTheme.spacing8

                    // The strip yields to the picture (see
                    // stripModeForStage()). The column's height comes from the
                    // Loader filling the stage, so the strip only redistributes
                    // what the column was given.
                    readonly property string stripMode:
                        root.stripModeForStage(spotlightColumn.height)
                    readonly property int stripHeight:
                        root.stripHeightForStage(spotlightColumn.height)
                    readonly property int stripTileHeight:
                        spotlightColumn.stripHeight - AppTheme.spacing8
                    // Derived from the height so a shrinking strip keeps the
                    // tile shape.
                    readonly property int stripTileWidth:
                        Math.round(spotlightColumn.stripTileHeight * 140 / 88)

                    Rectangle {
                        objectName: "callSpotlight"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: AppTheme.radiusTile
                        color: AppTheme.stormInset
                        border.width: 1
                        border.color: AppTheme.stormBorder
                        clip: true

                        Loader {
                            anchors.fill: parent
                            anchors.margins: 1
                            sourceComponent: focusedSurface
                        }

                        // Blanks the cursor over the picture only. It must be
                        // the top-most item to beat cursors set by controls
                        // above it; present only while idle so it doesn't
                        // override their cursors, and NoButton so it takes no
                        // clicks.
                        MouseArea {
                            objectName: "callSpotlightCursor"
                            anchors.fill: parent
                            z: 100
                            visible: root.stageChromeIdle
                            acceptedButtons: Qt.NoButton
                            cursorShape: Qt.BlankCursor
                        }

                        RowLayout {
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.margins: AppTheme.spacing8
                            spacing: AppTheme.spacing8
                            // Hidden rather than squeezed when the spotlight is
                            // too short, since the row sits in a clipped
                            // rectangle. The header button and the grid tile
                            // still lead back.
                            visible: parent.height
                                     >= root.spotlightOverlayHeight
                                        + AppTheme.spacing8
                            // Retires with the rest of the chrome, and is
                            // disabled too so an invisible control can't take a
                            // click.
                            opacity: root.stageChromeIdle ? 0 : 1
                            enabled: !root.stageChromeIdle
                            Behavior on opacity {
                                enabled: !AppTheme.reducedMotion
                                NumberAnimation { duration: 180 }
                            }

                            CallControlButton {
                                objectName: "callFullScreenButton"
                                iconName: "fit_screen"
                                diameter: 30
                                glyphSize: 16
                                tooltip: qsTr("Full screen")
                                onClicked: root.enterFullScreen()
                            }

                            // Dismisses, never writes a layout: the share stays
                            // live and in the grid.
                            AppButton {
                                objectName: "callBackToGridButton"
                                storm: true
                                size: "sm"
                                text: qsTr("Back to grid")
                                onClicked: root.leaveSpotlight()
                            }
                        }
                    }

                    // Every other surface, compact. Excluded by shareId, not
                    // identity: the spotlighted share must not be drawn twice
                    // (one screen sink per participant), but the sharer's
                    // camera is a different track and stays.
                    Flickable {
                        objectName: "callStrip"
                        Layout.fillWidth: true
                        // Hidden rather than squeezed below a legible tile
                        // height; an invisible child takes no height or
                        // spacing, so the picture gets the band back.
                        visible: spotlightColumn.stripMode === "tiles"
                                 && !root.bubblesInHeader
                        Layout.preferredHeight: visible
                                                ? spotlightColumn.stripHeight
                                                : 0
                        contentWidth: stripRow.width
                        contentHeight: height
                        flickableDirection: Flickable.HorizontalFlick
                        boundsBehavior: Flickable.StopAtBounds
                        clip: true

                        Row {
                            id: stripRow
                            // The tile's height, centred in the band.
                            height: spotlightColumn.stripTileHeight
                            anchors.verticalCenter: parent.verticalCenter
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

                                    height: spotlightColumn.stripTileHeight
                                    width: active
                                           ? spotlightColumn.stripTileWidth : 0
                                    // Inactive when the strip is hidden, so no
                                    // video sinks are held for an invisible
                                    // band.
                                    active: spotlightColumn.stripMode === "tiles"
                                            && (!root.stageState
                                                || stripShare.shareId
                                                   !== root.stageState.spotlightShareId)
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
                                    required property string reactionEmoji
                                    required property string connectionQuality

                                    height: spotlightColumn.stripTileHeight
                                    width: active
                                           ? spotlightColumn.stripTileWidth : 0
                                    // Only the person whose camera is
                                    // spotlighted leaves the strip.
                                    active: spotlightColumn.stripMode === "tiles"
                                            && (root.spotlightShareRow >= 0
                                                || !root.stageState
                                                || stripPerson.identity
                                                   !== root.stageState.pinnedIdentity)
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
                                        reactionEmoji: stripPerson.reactionEmoji
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

                    // ── The same strip at 44 px ── For a stage too short for
                    // tiles: the collapsed strip's bubble row, with the same
                    // faces, speaking ring and badges; clicking pins. People
                    // only; a second share stays reachable from the header and
                    // the grid.
                    Loader {
                        objectName: "callStripBubblesHost"
                        Layout.fillWidth: true
                        active: spotlightColumn.stripMode !== "tiles"
                                && !root.bubblesInHeader
                        visible: active
                        // The component's own height, 0 until anyone has
                        // arrived, so a connecting call reserves no empty band.
                        Layout.preferredHeight: active ? implicitHeight : 0
                        sourceComponent: CallSpeakerBubbles {
                            objectName: "callStripBubbles"
                            model: root.participantModel
                            onActivated: identity => {
                                if (root.stageState)
                                    root.stageState.pin(identity);
                            }
                        }
                    }
                }
            }

            // Shown in the panel while full screen is up, as a second exit on
            // the screen the user was already looking at.
            Loader {
                anchors.fill: parent
                active: !root.collapsed && root.fullScreenActive
                visible: active
                sourceComponent: Item {
                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: AppTheme.spacing8

                        Icon {
                            Layout.alignment: Qt.AlignHCenter
                            name: "fit_screen"
                            size: 28
                            color: AppTheme.stormTextSecondary
                        }
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("Playing full screen")
                            color: AppTheme.stormTextSecondary
                            font.pixelSize: 13
                        }
                        AppButton {
                            objectName: "callExitFullScreenFromStageButton"
                            Layout.alignment: Qt.AlignHCenter
                            storm: true
                            size: "sm"
                            text: qsTr("Exit full screen")
                            onClicked: root.exitFullScreen()
                        }
                    }
                }
            }
        }
    }

    // ── The full-screen window ──
    // A top-level window so full screen covers the monitor. Black around the
    // fitted picture on every theme.
    Window {
        id: fullScreenWindow

        objectName: "callFullScreenWindow"
        title: qsTr("Lightning — full screen")
        color: "#000000"
        // Fallback size only: placeOnThisApplicationsScreen() sets geometry
        // before every show. Constants, so the imperative writes break no
        // binding.
        width: 1280
        height: 720

        // No `visible`/`visibility` binding: see syncFullScreenWindow(). The
        // close is accepted (refusing would veto application quit) and the flag
        // is written back.
        onClosing: root.exitFullScreen()

        Item {
            id: fullScreenSurface
            objectName: "fullScreenSurface"
            anchors.fill: parent

            // ── Overlays retire when the pointer is still ──
            // A HoverHandler observes movement without consuming events, so the
            // overlays it wakes stay clickable.
            property bool overlaysIdle: false
            // Idle means the pointer hasn't moved, wherever it is (on one
            // monitor it rests inside the share). A ticking counter that is
            // never stopped or restarted, so no interruption can leave it
            // disarmed.
            property int idleTicks: 0
            readonly property int idleTicksToHide: 6   // 6 x 500 ms = 3 s
            Timer {
                id: fullScreenIdleTimer
                objectName: "fullScreenIdleTimer"
                interval: 500
                repeat: true
                running: root.fullScreenActive
                onTriggered: {
                    fullScreenSurface.idleTicks += 1;
                    if (fullScreenSurface.idleTicks >= fullScreenSurface.idleTicksToHide)
                        fullScreenSurface.overlaysIdle = true;
                }
            }
            MouseArea {
                objectName: "callFullScreenCursor"
                anchors.fill: parent
                z: 100
                visible: root.fullScreenActive
                         && fullScreenSurface.overlaysIdle
                acceptedButtons: Qt.NoButton
                cursorShape: Qt.BlankCursor
            }
            // Movement anywhere brings the chrome back and restarts the count.
            // Same position check as the stage: retiring the chrome re-delivers
            // hover.
            property real lastPointerX: -1
            property real lastPointerY: -1
            HoverHandler {
                id: fullScreenHover
                enabled: root.fullScreenActive
                onPointChanged: {
                    const p = fullScreenHover.point.scenePosition;
                    if (Math.abs(p.x - fullScreenSurface.lastPointerX) < 1
                        && Math.abs(p.y - fullScreenSurface.lastPointerY) < 1)
                        return;
                    fullScreenSurface.lastPointerX = p.x;
                    fullScreenSurface.lastPointerY = p.y;
                    fullScreenSurface.idleTicks = 0;
                    fullScreenSurface.overlaysIdle = false;
                }
            }
            // Leaving full screen must not strand the overlays hidden.
            Connections {
                target: root
                function onFullScreenActiveChanged() {
                    // Reset both, or a stale count hides the chrome as soon as
                    // the next session opens.
                    fullScreenSurface.overlaysIdle = false
                    fullScreenSurface.idleTicks = 0
                }
            }
            focus: true

            // A Keys handler, not a Shortcut: Escape is reserved in
            // ShortcutRegistry and two enabled Shortcuts on one sequence fire
            // neither. A handler in a different window can't collide.
            Keys.onPressed: function (event) {
                if (event.key === Qt.Key_Escape) {
                    root.exitFullScreen();
                    event.accepted = true;
                }
            }

            Loader {
                anchors.fill: parent
                // Only while shown: a hidden Window still instantiates
                // children, and an unseen VideoOutput would hold a frame and a
                // router key.
                active: root.fullScreenActive
                visible: active
                sourceComponent: focusedSurface
            }

            // Floating exit control.
            RowLayout {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: AppTheme.spacing12
                spacing: AppTheme.spacing8
                // Fades rather than vanishing under the pointer, but is
                // disabled while idle so invisible chrome can't take a click
                // (the dock also overlaps the reveal arrow).
                opacity: fullScreenSurface.overlaysIdle ? 0 : 1
                enabled: !fullScreenSurface.overlaysIdle
                Behavior on opacity {
                    enabled: !AppTheme.reducedMotion
                    NumberAnimation { duration: 180 }
                }

                CallControlButton {
                    objectName: "callExitFullScreenButton"
                    iconName: "close_fullscreen"
                    diameter: 36
                    glyphSize: 18
                    tooltip: qsTr("Exit full screen (Esc)")
                    onClicked: root.exitFullScreen()
                }
            }

            // The shared control bar in its dock placement.
            Loader {
                anchors.bottom: parent.bottom
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottomMargin: AppTheme.spacing16
                active: root.fullScreenActive
                visible: active
                opacity: fullScreenSurface.overlaysIdle ? 0 : 1
                enabled: !fullScreenSurface.overlaysIdle
                Behavior on opacity {
                    enabled: !AppTheme.reducedMotion
                    NumberAnimation { duration: 180 }
                }
                sourceComponent: CallHeaderBar {
                    objectName: "callFullScreenDock"
                    placement: "dock"
                    onParticipantsRequested: root.participantsRequested()
                }
            }
        }
    }
}
