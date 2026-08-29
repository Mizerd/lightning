import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
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
    Component.onCompleted: {
        root.recountCameras();
        // The stage is destroyed and rebuilt by its host's Loader on every
        // room change, while the call — and `CallStageState` with it —
        // outlives that. So the flag can already be true when this component
        // is created, and a sync driven only by the CHANGE signal would leave
        // the stage drawing "Playing full screen" with no window anywhere.
        root.syncFullScreenWindow();
    }

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

    /// The smallest tile strip worth drawing, and the row of bubbles that
    /// replaces it when even that will not fit.
    ///
    /// A tile carries an avatar AND a nameplate: below an 80 px band (a
    /// 72 px tile) the 28 px avatar and the plate start to touch, so a
    /// smaller tile strip is not a smaller version of this — it is a broken
    /// one. The bubble row is the same information at 44 px, and it is the
    /// component the COLLAPSED call strip already uses.
    readonly property int minimumTileStrip: 80
    readonly property int bubbleStripHeight: 44

    /// The overlay controls the spotlight draws over its own top-right
    /// corner: Full screen, and Back to grid.
    ///
    /// A 30 px button plus its two margins. Below this the row does not
    /// shrink — it OVERFLOWS a clipped rectangle, which is what the
    /// maintainer photographed: a share tile squeezed to a few pixels with
    /// the two controls crushed and half-drawn across its top edge.
    readonly property int spotlightOverlayHeight: 30 + 2 * AppTheme.spacing8

    /// The shortest this stage can be and still be worth drawing.
    ///
    /// Read by the HOST, which owns the panel's height and cannot otherwise
    /// know what this surface spends before the picture starts: the header
    /// row, the dock, the column's own margins and spacings. Kept here
    /// because this is the file those bands are declared in — a copy in the
    /// host is a copy that drifts the first time one of them changes.
    ///
    /// `minimumPicture` is what makes it a POLICY rather than an accounting
    /// identity: a stage that fits its chrome and nothing else is exactly the
    /// state being fixed.
    readonly property int minimumPictureHeight: 132
    readonly property int minimumUsefulHeight:
        2 * AppTheme.spacing12          // the column's own margins
        // The header row, which now CARRIES the controls. This value is the
        // EXPANDED case, which is the only one it is asked about: the full
        // (non-compact) dock's 48 px buttons plus its pill padding, per
        // CallHeaderBar's own controlDiameter. It replaced a 28 px
        // title-only row.
        + 48 + 2 * AppTheme.spacing8 + AppTheme.spacing8
        + root.minimumPictureHeight
        // No dock term. There is no bottom dock any more, and the 48 px band
        // plus margin it used to reserve was height taken from the picture
        // for something that is no longer drawn.

    /// What the strip beneath the spotlight is, for a stage of `available`
    /// px: "tiles" or "bubbles".
    ///
    /// THE STRIP MUST NEVER BE THE BIGGER HALF OF THE STAGE. It used to ask
    /// for a flat 96 px whatever the panel had and the spotlight took what
    /// was left, so on the maintainer's Windows capture a 335 px call panel
    /// spent 96 on the strip and left the shared screen 61 — a 16:9 desktop
    /// arriving as a 112x61 stamp in a full-width letterbox. Display scale
    /// is what makes this bite: at 150% every number here is unchanged and
    /// two thirds as many of them fit.
    ///
    /// So the strip may have at most 40% of the stage, and when 40% cannot
    /// pay for a usable tile it becomes the bubble row rather than a squeeze
    /// of a shape that no longer works. Nothing is lost that has no other
    /// route: the bubbles carry the same faces, the same speaking ring and
    /// the same mute/sharing badges, they pin on click exactly as a strip
    /// tile does, and a dismissed share is still reachable from the header's
    /// "Show screen share" and from the grid.
    function stripModeForStage(available) {
        if (!available || available <= 0)
            return "tiles";
        return Math.floor((available - AppTheme.spacing8) * 0.4)
                >= root.minimumTileStrip ? "tiles" : "bubbles";
    }

    /// Are the participant faces drawn in the HEADER rather than in a strip
    /// under the spotlight?
    ///
    /// During a screen share the strip cost up to 96 px of picture for a row
    /// of faces, on the surface where height is worth the most. In the header
    /// they sit opposite the controls and cost the share nothing. Collapsed
    /// is unchanged — there is no spotlight to protect and the one-line strip
    /// IS the header.
    ///
    /// Exactly one of the two hosts may be active: this property is what both
    /// read, so they cannot disagree and draw the same faces twice.
    readonly property bool bubblesInHeader:
        !root.collapsed && !root.fullScreenActive
        && root.effectiveLayout === "spotlight"

    /// How tall that strip is. 96 is the band this surface has always drawn
    /// and stays the cap, so a roomy stage is untouched.
    ///
    /// A FUNCTION on the stage root rather than an expression buried in the
    /// spotlight's Loader so it can be exercised directly — but a policy
    /// test that calls this proves nothing about whether production reaches
    /// it, so `CallUiContractTest` also pins the call site.
    function stripHeightForStage(available) {
        if (!available || available <= 0)
            return 96;
        if (root.stripModeForStage(available) !== "tiles")
            return root.bubbleStripHeight;
        return Math.min(96,
                        Math.floor((available - AppTheme.spacing8) * 0.4));
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

    // ── Full screen ──────────────────────────────────────────────────────
    //
    // The focused surface on a whole screen, in its own window — Discord's
    // "Full Screen" on the focused stream, and the maintainer's "add an
    // option to full screen screen share so it takes full minotir".
    //
    // A SEPARATE Window, not an overlay inside the main one, because "full
    // monitor" is what was asked for: an overlay can only ever fill the
    // application window. It is declared inside this Item, so Qt makes it
    // transient for the main window and it dies with the stage.
    //
    // NOT gated on `collapsed`: watching a share full screen on one monitor
    // while the in-room panel is collapsed to read messages is a reason to
    // have this at all.
    //
    // THE ONE STATE THAT MUST NOT EXIST is full screen with nothing in it —
    // a black monitor with no obvious way out. `CallStageState` refuses to
    // enter without a focused surface and drops the flag the moment the
    // spotlight empties; this binding is the second half of the same rule,
    // reading the resolved model rows rather than the ids.
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

    /// One level back, whatever level you are on. Tapping the big picture
    /// leaves full screen; tapping the stage's spotlight leaves the
    /// spotlight. Entering full screen is a BUTTON, so this is only ever a
    /// forgiving way out, never the way in.
    function focusedSurfaceActivated() {
        if (root.fullScreenActive)
            root.exitFullScreen();
        else
            root.leaveSpotlight();
    }

    // Driven IMPERATIVELY, and that is deliberate. Binding `Window.visibility`
    // would put a binding on the exact property a window manager writes when
    // the user closes the window — and a QML binding that the platform
    // overwrites is how this repo has shipped one-way latches before. There is
    // no binding here to break: the flag is the single source of truth, and
    // `onClosing` ACCEPTS the close (refusing it would veto Ctrl+Q, §16) and
    // writes the flag back, so the two can never disagree for longer than one
    // call.
    function syncFullScreenWindow() {
        // The flag can change while this component is still being built —
        // `stageState` resolves partway through — and an id whose object has
        // not been created yet reads as null rather than throwing. Guard, or
        // the first evaluation logs a reference error nobody will connect to
        // a window that then never opens.
        if (!fullScreenWindow)
            return;
        if (root.fullScreenActive) {
            root.placeOnThisApplicationsScreen();
            fullScreenWindow.showFullScreen();
            fullScreenSurface.forceActiveFocus();
            if (root.stageState && root.stageState.traceEnabled) {
                // The second half of the measurement: where it ACTUALLY
                // landed. Equal to the line above means the request was
                // honoured; different means the compositor chose.
                console.info("call-fullscreen landed on="
                             + (fullScreenWindow.screen
                                ? fullScreenWindow.screen.name : "?"));
            }
        } else if (fullScreenWindow.visible) {
            fullScreenWindow.hide();
        }
    }

    /// Put the full-screen window on the monitor the APPLICATION is on.
    ///
    /// "the full screen feature always starts in the same monitor and not the
    /// one the client is in, it should full screen in same monitor as the app
    /// is." It did that by construction: a top-level QWindow with no target
    /// screen is connected to `QGuiApplication::primaryScreen()` in
    /// `QWindowPrivate::init()`, a QML Window does NOT inherit its transient
    /// parent's screen, and `showFullScreen()` selects no screen of its own —
    /// it is only setWindowStates + setVisible + requestActivate.
    ///
    /// TWO things are needed, and the second is the one that decides.
    ///
    /// 1. `screen`, for the window's own bookkeeping and DPI. On its own it
    ///    is NOT enough: `QWindowPrivate::create()` re-derives the screen from
    ///    the window's GEOMETRY (`screenForGeometry()`) just before the
    ///    platform window is made, so a default-positioned rectangle lands
    ///    back on the primary monitor. And assigning `screen` LATER does not
    ///    move an existing window: when the two screens are virtual siblings
    ///    — the ordinary single-desktop case — `windowRecreationRequired()`
    ///    is false and the setter is bookkeeping plus a signal.
    /// 2. The GEOMETRY, in virtual-desktop coordinates. That is what
    ///    `screenForGeometry()` reads, and what the window manager then
    ///    fullscreens onto.
    ///
    /// `Screen` attached to THIS item, not `Window.window.screen`: the
    /// attached object tracks the item's window and follows the app when the
    /// user drags it to another monitor, which is precisely the question
    /// being asked. Re-applied on every entry for the same reason.
    ///
    /// HONESTY: this is derived from the Qt sources and is expected to hold
    /// on X11. On Wayland there is no client-side global positioning, and Qt
    /// passes no `wl_output` to `xdg_toplevel.set_fullscreen` (QTBUG-54883,
    /// closed as out of scope), so the compositor chooses. Live-validated:
    /// NOT TESTED on either. `LIGHTNING_CALL_TRACE=1` prints the one line
    /// that tells the two cases apart.
    function placeOnThisApplicationsScreen() {
        var target = root.Screen;
        if (!target)
            return;
        // Assigned before the FIRST show, because create() reads the
        // geometry — see (1) above.
        fullScreenWindow.screen = target;
        fullScreenWindow.x = target.virtualX;
        fullScreenWindow.y = target.virtualY;
        fullScreenWindow.width = target.width;
        fullScreenWindow.height = target.height;
        if (root.stageState && root.stageState.traceEnabled) {
            // The measurement this lane's own rule asks for: it distinguishes
            // "we asked for the wrong monitor" from "the compositor overrode
            // us", which no amount of source reading can settle. Names and
            // numbers only — never a room, a user or a track.
            console.info("call-fullscreen"
                         + " platform=" + Qt.platform.pluginName
                         + " appScreen=" + target.name
                         + " virtualX=" + target.virtualX
                         + " virtualY=" + target.virtualY
                         + " size=" + target.width + "x" + target.height);
        }
    }
    onFullScreenActiveChanged: root.syncFullScreenWindow()

    // ── The focused surface, defined ONCE ────────────────────────────────
    //
    // Hosted by the stage's spotlight and by the full-screen window, and
    // NEVER by both: `fullScreenActive` stands the stage's grid and spotlight
    // Loaders down. That matters for one reason beyond tidiness — the router
    // holds ONE sink per key, so two live surfaces asking for one
    // participant's screen would take turns owning it, and whichever lost the
    // race would be blank.
    //
    // Going full screen therefore does REBUILD the video item; a QQuickItem
    // cannot move between scene graphs, so no arrangement of Loaders could
    // avoid that. What makes the rebuild safe is the router's ownership rule
    // (SfuVideoRouter): the surface being built CLAIMS the key, and the one
    // being torn down releases by SINK, so by the time its deferred
    // destruction runs it owns nothing and takes nothing with it. Without
    // that rule this transition would blank the video exactly as the
    // grid→spotlight one did.
    Component {
        id: focusedSurface

        Item {
            // The spotlighted SHARE. A Repeater over the real model rather
            // than a `get(row)` snapshot: the track key fills in late, and a
            // snapshot taken before it arrives never attaches a sink.
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
                        onActivated: root.focusedSurfaceActivated()
                    }
                }
            }

            // Only when there is genuinely nobody to spotlight — a pinned
            // participant who has left, for instance.
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
                // A LABEL MUST NOT DECIDE HOW MUCH ROOM THE CONTROLS GET.
                //
                // A RowLayout that cannot fit every child shrinks them all in
                // proportion to their PREFERRED widths, and an item's default
                // preferred width is its implicit one — so a long title would
                // take its share of the squeeze out of the collapse button
                // and the "Show screen share" button beside it, and a
                // squeezed AppButton draws its label straight across its
                // neighbour (its content Row is centred and unconstrained).
                // Asking for 1 px while filling means this text still takes
                // every spare pixel and yields all of them back first. It
                // elides, so nothing is lost; a control cannot elide.
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
                objectName: "callHeaderBubblesHost"
                // Collapsed the strip fills; beside a spotlight it takes only
                // what the faces need, so the title keeps the rest.
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

            // THE control surface, collapsed AND expanded.
            //
            // It used to be here only while collapsed, with a full-width dock
            // across the BOTTOM of the expanded stage. That cost a whole strip
            // of picture on every screen share, and it moved the controls to a
            // different place depending on a state the user did not choose —
            // so muting meant finding the button first. The maintainer asked
            // for the collapsed arrangement to become the permanent one.
            //
            // Same component, same definition, one placement now. The header
            // instance still stands down while the stage is on screen
            // (CallHeaderBar.stageOwnsControls), so they are never drawn twice.
            Loader {
                active: true
                visible: true
                Layout.preferredHeight: implicitHeight
                // A FLOOR, so the squeeze lands on the title and the bubbles
                // rather than on the controls. A RowLayout too narrow for
                // its children shrinks them in proportion to their preferred
                // widths, and this cell's contents cannot elide: the bar's
                // control row is centred in whatever box it is given and
                // simply draws outside a box too small for it. The title
                // beside it elides and the bubble strip scrolls.
                Layout.minimumWidth: implicitWidth
                Layout.alignment: Qt.AlignVCenter
                sourceComponent: CallHeaderBar {
                    objectName: "callStageControls"
                    placement: "dock"
                    // Compact ONLY while collapsed. `compact` is not just a
                    // size: CallHeaderBar hides the screen-share button, the
                    // raise-hand button and all three device chevrons behind
                    // it, because a one-line strip has no room for them. The
                    // ask was to move the controls to the top, NOT to reduce
                    // the set — an expanded call that had lost Share and
                    // Raise hand would be a worse bug than the one being
                    // fixed. Expanded gets the full bar, in the same place.
                    compact: root.collapsed
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
            //
            // Stood down while full screen is up: one surface per routing key
            // at a time, or two live tiles fight over it.
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
                active: !root.collapsed && !root.fullScreenActive
                        && root.effectiveLayout === "spotlight"
                visible: active
                sourceComponent: ColumnLayout {
                    id: spotlightColumn
                    spacing: AppTheme.spacing8

                    // THE STRIP YIELDS TO THE PICTURE — see the policy and
                    // the measurement on `stripModeForStage`.
                    //
                    // `spotlightColumn.height` comes from ABOVE: the Loader
                    // that hosts this fills the stage, so this reads a height
                    // the column does not compute, and the strip only ever
                    // redistributes what the column was already given.
                    readonly property string stripMode:
                        root.stripModeForStage(spotlightColumn.height)
                    readonly property int stripHeight:
                        root.stripHeightForStage(spotlightColumn.height)
                    readonly property int stripTileHeight:
                        spotlightColumn.stripHeight - AppTheme.spacing8
                    // 16:9-ish, and derived rather than a second literal, so
                    // a strip that shrinks cannot leave its tiles the shape
                    // they had at full size.
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

                        RowLayout {
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.margins: AppTheme.spacing8
                            spacing: AppTheme.spacing8
                            // ABSENT, NOT SQUEEZED — the same rule the strip
                            // below already follows. This row has a fixed
                            // height and is anchored inside a CLIPPED
                            // rectangle, so on a spotlight shorter than it
                            // needs it does not compact: it draws across the
                            // tile's top edge and is cut in half. Hiding it
                            // costs no route — the header's "Show screen
                            // share" and the share's own grid tile both lead
                            // back, which is the invariant recorded at the
                            // top of this file.
                            visible: parent.height
                                     >= root.spotlightOverlayHeight
                                        + AppTheme.spacing8

                            CallControlButton {
                                objectName: "callFullScreenButton"
                                iconName: "fit_screen"
                                diameter: 30
                                glyphSize: 16
                                tooltip: qsTr("Full screen")
                                onClicked: root.enterFullScreen()
                            }

                            // DISMISS, never a layout write. The dismissed
                            // share stays live, stays a row in the model and
                            // stays a tile in the grid — so this exit is not a
                            // door that locks behind you.
                            AppButton {
                                objectName: "callBackToGridButton"
                                storm: true
                                size: "sm"
                                text: qsTr("Back to grid")
                                onClicked: root.leaveSpotlight()
                            }
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
                        // Absent, not squeezed, once the stage is too short
                        // for a legible tile: an invisible child takes no
                        // height AND no spacing from a layout, so the
                        // picture gets the whole band back.
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
                            // The TILE's height, centred, rather than the
                            // strip's: the tiles were 88 in a 96 band and
                            // top-aligned, so the band carried 8 px of dead
                            // space along its bottom edge. Centring is also
                            // what keeps them centred once the strip shrinks.
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
                                    // The MODE is part of being wanted: a
                                    // hidden strip whose delegates still
                                    // existed would go on holding a video
                                    // sink per surface for a band nobody can
                                    // see.
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
                                    required property string connectionQuality

                                    height: spotlightColumn.stripTileHeight
                                    width: active
                                           ? spotlightColumn.stripTileWidth : 0
                                    // Only the person whose CAMERA is on the
                                    // spotlight is removed from the strip.
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

                    // ── The same strip, at 44 px ─────────────────────────
                    //
                    // What a stage too short for tiles gets instead: the
                    // bubble row the collapsed call strip already uses, with
                    // the same faces, the same speaking ring driven by the
                    // SFU's own levels, and the same mute / sharing badges.
                    // Clicking one pins that person, exactly as clicking a
                    // strip tile does.
                    //
                    // It is a strip of PEOPLE only, so a second share has no
                    // tile here — it is still reachable from the header's
                    // "Show screen share" and from the grid, which is the
                    // invariant this surface actually holds.
                    Loader {
                        objectName: "callStripBubblesHost"
                        Layout.fillWidth: true
                        active: spotlightColumn.stripMode !== "tiles"
                                && !root.bubblesInHeader
                        visible: active
                        // The COMPONENT's own height, which is 0 while
                        // nobody has arrived yet — so a connecting call
                        // reserves no empty band. `bubbleStripHeight` is the
                        // same number, and it is what the mode decision is
                        // made against.
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

            // WHERE THE PICTURE WENT. The stage's own surfaces are stood down
            // while full screen is up, so without this the panel is an empty
            // rectangle and the only way back is a window that may be on
            // another monitor. This is the second exit, and it is on the
            // screen the user was already looking at.
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

        // NO BOTTOM DOCK. The control set lives in the stage's top row in
        // both states — see the Loader up there. A full-width strip along the
        // bottom took a band of the picture away from every screen share and
        // put the controls somewhere different depending on whether the panel
        // happened to be collapsed. One surface, one position.
    }

    // ── The full-screen window ───────────────────────────────────────────
    //
    // Its own top-level window, so "full screen" means the MONITOR and not
    // the application window. Declared inside this Item, which is what makes
    // Qt treat it as transient for the main window and destroy it with the
    // stage.
    //
    // Black, not a theme surface: every pixel around a fitted picture should
    // be the absence of a picture, and that is black on every theme.
    Window {
        id: fullScreenWindow

        objectName: "callFullScreenWindow"
        title: qsTr("Lightning — full screen")
        color: "#000000"
        // A FALLBACK size only. placeOnThisApplicationsScreen() overwrites
        // both, plus x/y, before every show — and it is the GEOMETRY, not the
        // `screen` assignment, that decides which monitor the window is born
        // on (QWindowPrivate::create() → screenForGeometry()). These literals
        // are what remains if the Screen attached object is unavailable.
        // They are constants, not bindings, so the imperative write destroys
        // nothing.
        width: 1280
        height: 720

        // NO `visible` and NO `visibility` binding: see syncFullScreenWindow().
        // ACCEPTED, never refused. A window that refuses its close event
        // vetoes application quit — §16 records close-to-tray eating Ctrl+Q
        // for exactly that reason — so this lets the close happen and writes
        // the flag back so the state cannot disagree with the screen.
        onClosing: root.exitFullScreen()

        Item {
            id: fullScreenSurface
            objectName: "fullScreenSurface"
            anchors.fill: parent

            // ── Full-screen overlays retire when the pointer is still ──────
            //
            // Reported: the controls "just sit there for good" in full screen.
            // Every video surface hides its chrome once the pointer stops, and
            // this one is showing somebody else's screen — the whole reason to
            // be full screen is to see it unobstructed.
            //
            // The pointer, not a keystroke: a HoverHandler reports movement
            // without consuming anything, so the overlays it wakes are still
            // clickable and nothing below them loses an event.
            property bool overlaysIdle: false
            // IDLE MEANS THE POINTER HAS NOT MOVED, wherever it is.
            //
            // Not "the pointer is off the window", and not "the pointer is
            // off the dock". Both of those looked right on two monitors —
            // tabbing away moved the pointer out and the chrome went — and
            // both leave it on screen forever for anyone with ONE monitor,
            // because the pointer simply rests inside the share. What was
            // asked for is the plain version: still for a few seconds and it
            // goes.
            //
            // A ticking counter rather than a restartable one-shot. The timer
            // is never stopped and never restarted, so there is no path where
            // an interruption leaves it disarmed — which is exactly how the
            // previous attempt got stuck on screen permanently.
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
            // Movement — anywhere over the share — brings it back at once and
            // restarts the count. That is also the "move toward the edge and
            // it reappears" behaviour, since reaching for the controls IS
            // movement.
            HoverHandler {
                id: fullScreenHover
                enabled: root.fullScreenActive
                onPointChanged: {
                    fullScreenSurface.idleTicks = 0;
                    fullScreenSurface.overlaysIdle = false;
                }
            }
            // Leaving full screen must not strand the overlays hidden: the
            // flag is state, and the state it belongs to has ended.
            Connections {
                target: root
                function onFullScreenActiveChanged() {
                    // Both halves, or a count left over from the last
                    // full-screen session retires the chrome the instant the
                    // next one opens.
                    fullScreenSurface.overlaysIdle = false
                    fullScreenSurface.idleTicks = 0
                }
            }
            focus: true

            // A Keys handler, NOT a Shortcut. Escape is in ShortcutRegistry's
            // RESERVED list (it closes the find bar, room information, a
            // thread and Settings), and two enabled Shortcuts on one sequence
            // fire NEITHER — a window-level Shortcut here would break closing
            // a dialog while a call is up. A handler on the focused item of a
            // DIFFERENT window cannot collide with any of that.
            Keys.onPressed: function (event) {
                if (event.key === Qt.Key_Escape) {
                    root.exitFullScreen();
                    event.accepted = true;
                }
            }

            Loader {
                anchors.fill: parent
                // Only while shown. A hidden Window still instantiates its
                // children, and a VideoOutput nobody can see would hold a
                // decoded frame and a router key for the whole call.
                active: root.fullScreenActive
                visible: active
                sourceComponent: focusedSurface
            }

            // Floating controls. Deliberately always visible rather than
            // fading on idle: a control that has to be summoned cannot
            // explain how to leave, and leaving is the thing a full-screen
            // surface most needs to make obvious.
            RowLayout {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: AppTheme.spacing12
                spacing: AppTheme.spacing8
                // Fades rather than disappearing, so a control the user is
                // reaching for does not vanish from under the pointer -- but
                // retired chrome must not ANSWER a click. It is invisible and
                // the pointer may be resting on it, and the dock is stacked
                // over the reveal arrow, so a live one both swallowed the tap
                // meant for the arrow and let a still click hit a button
                // nobody could see.
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

            // The handle that says the chrome is still there.
            //
            // Without it a retired dock is indistinguishable from a call that
            // has no controls at all — the user asked for "a small arrow at
            // the bottom", which is exactly Discord's affordance. It is the
            // INVERSE of the dock: visible only while the dock is hidden, and
            // clicking it brings everything back for people who would rather
            // click than hover.
            Loader {
                objectName: "callFullScreenRevealHandle"
                anchors.bottom: parent.bottom
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottomMargin: AppTheme.spacing8
                active: root.fullScreenActive
                        && fullScreenSurface.overlaysIdle
                visible: active
                opacity: active ? 1 : 0
                Behavior on opacity {
                    enabled: !AppTheme.reducedMotion
                    NumberAnimation { duration: 180 }
                }
                sourceComponent: Rectangle {
                    implicitWidth: 44
                    implicitHeight: 20
                    radius: height / 2
                    color: Qt.alpha(AppTheme.stormPanel, 0.72)
                    border.width: 1
                    border.color: Qt.alpha(AppTheme.stormBorder, 0.72)
                    Icon {
                        anchors.centerIn: parent
                        name: "expand_less"
                        size: 16
                        color: AppTheme.stormTextSecondary
                    }
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Show call controls")
                    TapHandler {
                        onTapped: {
                            // The count, not the timer. It is already AT the
                            // budget, so re-phasing alone retired the chrome
                            // again on the very next tick.
                            fullScreenSurface.overlaysIdle = false
                            fullScreenSurface.idleTicks = 0
                        }
                    }
                }
            }

            // The same control set as everywhere else, in its dock
            // placement. One definition; this is a placement of it, not
            // another bar.
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
