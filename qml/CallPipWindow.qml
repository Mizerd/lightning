import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import MatrixClient

// PICTURE-IN-PICTURE: the live call, in a small always-on-top window, for
// when the main window is somewhere else.
//
// # Why this is a REPLACEMENT and never a duplicate
//
// `SfuVideoRouter` holds ONE sink per track and the last attach owns it (see
// CallShareTile's header, and the 2026-08-27 round that fixed "when i full
// screen it it stop shwoing video"). Two surfaces rendering the same
// participant therefore means one of them goes black, and which one depends
// on event-loop ordering — the worst kind of defect to own.
//
// So this window's video surface is built ONLY while the window is actually
// showing, and `CallStageState` makes picture-in-picture and full screen
// mutually exclusive. When the PiP is up, the main stage's own tiles are
// gone: its host Loader is inactive because the room is not on screen, which
// is the only situation this window opens in.
//
// # What it shows
//
// The call's grid — every participant as a tile and every screen share as a
// tile of its own — through the same CallTileGrid the main stage uses. It
// used to show ONE surface (a share, else the pinned face, else a label);
// the maintainer asked for people and shares to each have a box.
//
// # What it does NOT do
//
// It does not follow the active speaker. That needs a debounce policy the
// stage does not have either, and a window that flips between faces every
// time somebody says "mm" is worse than one that holds still. Pinning from
// the main window already chooses the subject and this obeys it.
Window {
    id: root

    /// The stage state that owns the flag. Handed in so this file performs
    /// no global lookups and can be built in a test.
    property var stageState: app.groupCall ? app.groupCall.stageState : null
    property var participantModel:
        app.groupCall ? app.groupCall.participantModel : null
    property var shareModel: app.groupCall ? app.groupCall.shareModel : null

    /// Whether a call is live at all. Both lanes: the SFU lane carries video,
    /// the legacy 1:1 lane is audio-only but still worth a floating control.
    readonly property bool callLive:
        (app.groupCall && app.groupCall.active)
        || (app.calls && (app.calls.state === CallController.Active
                          || app.calls.state === CallController.Connecting))
    readonly property bool groupLive: app.groupCall && app.groupCall.active

    readonly property bool wanted:
        root.callLive && root.stageState && root.stageState.pictureInPicture

    // ── Which surface ────────────────────────────────────────────────────
    //
    // A share wins over a face: somebody sharing their screen is showing you
    // something, and a 320-pixel window of their forehead is not it.
    readonly property int shareRow: {
        if (!root.shareModel || !root.stageState)
            return -1
        if (root.stageState.spotlightShareId.length > 0)
            return root.shareModel.indexOfShare(root.stageState.spotlightShareId)
        return root.shareModel.count > 0 ? 0 : -1
    }
    readonly property int pinnedRow: {
        if (!root.participantModel || !root.stageState
            || root.stageState.pinnedIdentity.length === 0)
            return -1
        return root.participantModel.indexOfIdentity(
            root.stageState.pinnedIdentity)
    }
    readonly property bool hasSurface: root.shareRow >= 0 || root.pinnedRow >= 0
    /// Tiles the grid will draw: every share and every participant.
    readonly property int total:
        (root.shareModel ? root.shareModel.count : 0)
        + (root.participantModel ? root.participantModel.count : 0)
    /// The share this window is showing, by id. The delegates match on the
    /// id rather than on a row number, because a row number moves when
    /// somebody else starts or stops sharing and the tile would then follow
    /// a different person without anything changing on screen to say so.
    readonly property string shareIdShown: {
        if (!root.shareModel || !root.stageState)
            return ""
        if (root.stageState.spotlightShareId.length > 0)
            return root.stageState.spotlightShareId
        // Nothing spotlighted: the first live share, resolved through the
        // model so it is the same row `shareRow` reports.
        var row = root.shareModel.count > 0 ? root.shareModel.get(0) : null
        return row ? (row.shareId || "") : ""
    }
    readonly property string pinnedIdentityShown:
        root.stageState ? root.stageState.pinnedIdentity : ""

    // "Fill the window with the share" (2026-09-05 request): the popout
    // shows ONLY the share, edge to edge, so it can live on a second monitor
    // as a dedicated share window while the main window stays on the room.
    // Off again with the same button, and dropped on its own when the share
    // ends. Local state of this window: the stage's spotlight is untouched.
    property bool shareFills: false
    // The share the reader clicked to fill the window with; falls back to
    // the spotlighted/first share when it is gone.
    property string fillShareId: ""
    readonly property string fillShareShown:
        root.fillShareId.length > 0 && root.shareModel
            && root.shareModel.indexOfShare(root.fillShareId) >= 0
        ? root.fillShareId : root.shareIdShown
    readonly property bool shareFillActive:
        root.shareFills && root.groupLive && root.fillShareShown.length > 0
    onFillShareShownChanged: if (root.fillShareShown.length === 0) root.shareFills = false
    // No chrome at all while a share fills the window (2026-09-05 request):
    // the bar hides, a click on the share restores the tiles, so does
    // Escape. Clicking a share tile in the grid is what fills the window.
    Shortcut {
        sequence: "Escape"
        enabled: root.shareFillActive
        onActivated: root.shareFills = false
    }

    // NO TRANSIENT PARENT, and this is load-bearing rather than tidy.
    //
    // A Window declared inside another Window gets that one as its
    // `transientParent`, and several compositors hide or minimise a
    // transient child WITH its parent. That is precisely the state — the
    // main window minimised or in the tray — that this window exists to
    // serve, so inheriting the parent would make the automatic pop-out a
    // no-op on exactly the desktops it matters on.
    transientParent: null

    title: qsTr("Lightning call")
    // A tool window on the platforms that have one — it belongs beside the
    // application rather than in the task switcher as a second app — and
    // always on top, which is the entire point of a floating call window.
    flags: Qt.Window | Qt.WindowStaysOnTopHint | Qt.WindowTitleHint
           | Qt.WindowCloseButtonHint
    // Room for a 2x2 of tiles at a legible size; the user resizes from here.
    width: 480
    height: 320
    minimumWidth: 240
    minimumHeight: 160
    color: AppTheme.background

    // IMPERATIVE, exactly as CallStage drives its full-screen window and for
    // the same reason: binding `visible` would put a QML binding on the
    // property a window manager writes when the user closes the window, and
    // this repository has shipped a one-way latch that way before. The flag
    // is the single source of truth; onClosing writes it back.
    function sync() {
        if (root.wanted && !root.visible)
            root.show()
        else if (!root.wanted && root.visible)
            root.hide()
    }
    onWantedChanged: root.sync()
    Component.onCompleted: root.sync()

    // THE FLAG MUST NOT SURVIVE THE CALL.
    //
    // `CallStageState::clear()` drops it, but its only callers are on the
    // GROUP lane — and this window, and the pop-out button that sets the
    // flag, both serve the legacy 1:1 lane too. Without this, popping out
    // during a 1:1 call and then hanging up leaves the flag set, and the
    // NEXT 1:1 call opens an always-on-top window by itself — which the
    // settings copy explicitly promises will not happen.
    //
    // Keyed on the call ending rather than on the window hiding: `wanted`
    // already goes false on its own, and clearing there would fight the
    // user's own deliberate pop-out.
    onCallLiveChanged: {
        if (!root.callLive && root.stageState
                && root.stageState.pictureInPicture)
            root.stageState.setPictureInPicture(false)
    }

    onClosing: (close) => {
        // ACCEPT the close and write the flag back. Refusing it would veto
        // the window manager and, on some desktops, Ctrl+Q with it (§16).
        close.accepted = true
        if (root.stageState)
            root.stageState.setPictureInPicture(false)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── The picture ──────────────────────────────────────────────────
        Item {
            id: surface
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            // THE WHOLE CALL, not one face. Reported with a mock-up: "make
            // the popout show people" — a 2x2 of participant tiles — "and
            // make sure screenshare also gets shown in the popout and gets
            // a separate box". The stage's own grid does exactly that
            // (shares as their own tiles, everyone else beside them), so
            // this window hosts it rather than choosing one surface.
            //
            // Still ONE owner per track: the grid is built only while this
            // window is showing (`active` below), which is the only time the
            // main stage's tiles are gone — see the header. The grid is
            // bound to the REAL models, so a share whose track key fills in
            // after its row appears still attaches its sink.
            // The share alone, when asked for. A Repeater over the share
            // model rather than a lookup: the tile's inputs are the model's
            // own roles, and only the row that is the share shown gets a
            // size — the others stay invisible and empty.
            Repeater {
                id: shareFillTiles
                objectName: "pipShareFill"
                // ONLY WHILE THE WINDOW SHOWS, like the grid below. A tile
                // claims its track's sink when it is BUILT, and the main
                // stage's tiles claim it back when this window hides; fill
                // tiles that outlived a pop-in kept existing unattached, so
                // the next pop-out showed a grey empty box until a restart
                // (2026-09-05 report). The reader's fill choice itself is
                // kept, so popping out again returns to the filled share.
                model: root.visible && root.shareFillActive ? root.shareModel : null
                delegate: Item {
                    id: fillCell
                    required property int index
                    required property string shareId
                    required property string ownerIdentity
                    required property string ownerDisplayName
                    required property string trackKey
                    required property bool local
                    readonly property bool shown: fillCell.shareId === root.fillShareShown
                    anchors.fill: parent
                    // A little breathing room so the picture's frame is not
                    // the window edge.
                    anchors.margins: AppTheme.spacing6
                    visible: shown
                    CallShareTile {
                        anchors.fill: parent
                        visible: fillCell.shown
                        shareId: fillCell.shareId
                        ownerIdentity: fillCell.ownerIdentity
                        ownerDisplayName: fillCell.ownerDisplayName
                        trackKey: fillCell.trackKey
                        local: fillCell.local
                        compact: true
                        focused: true
                        onActivated: root.shareFills = false
                    }
                }
            }

            Loader {
                id: pipGrid
                objectName: "pipGrid"
                anchors.fill: parent
                anchors.margins: AppTheme.spacing4
                active: root.visible && root.groupLive && !root.shareFillActive
                visible: active
                sourceComponent: CallTileGrid {
                    shareModel: root.shareModel
                    participantModel: root.participantModel
                    compact: true
                    focusedShareId: root.shareIdShown
                    focusedIdentity: root.pinnedIdentityShown
                    onShareActivated: shareId => {
                        root.fillShareId = shareId
                        root.shareFills = true
                    }
                    onParticipantActivated: identity => {
                        if (root.stageState)
                            root.stageState.pin(identity)
                    }
                }
            }

            // Voice-only, or nothing focused: say so rather than show black.
            ColumnLayout {
                anchors.centerIn: parent
                width: parent.width - AppTheme.spacing16 * 2
                spacing: AppTheme.spacing4
                visible: !root.groupLive || root.total === 0
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("Call in progress")
                    color: AppTheme.textPrimary
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightStrong
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    text: qsTr("%n participant(s)", "",
                               root.participantModel
                               ? root.participantModel.count : 0)
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.textMeta
                }
            }
        }

        // ── Controls ─────────────────────────────────────────────────────
        //
        // The four that matter away from the main window. Everything else —
        // devices, layout, participants — is a reason to go back to it, and
        // "Open Lightning" is right here.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            visible: !root.shareFillActive
            color: AppTheme.surface
            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 1
                color: AppTheme.border
            }
            RowLayout {
                anchors.centerIn: parent
                spacing: AppTheme.spacing8

                CallControlButton {
                    objectName: "pipMuteButton"
                    iconName: root.micMuted ? "mic_off" : "mic"
                    role: root.micMuted ? "active" : "neutral"
                    diameter: 30
                    glyphSize: 17
                    tooltip: root.micMuted ? qsTr("Unmute microphone")
                                           : qsTr("Mute microphone")
                    onClicked: {
                        if (root.groupLive)
                            app.groupCall.toggleMicrophoneMuted()
                        else
                            app.calls.toggleMicrophoneMuted()
                    }
                }
                CallControlButton {
                    objectName: "pipDeafenButton"
                    iconName: root.deafened ? "headset_off" : "headset_mic"
                    role: root.deafened ? "active" : "neutral"
                    diameter: 30
                    glyphSize: 17
                    tooltip: root.deafened ? qsTr("Undeafen") : qsTr("Deafen")
                    onClicked: {
                        if (root.groupLive)
                            app.groupCall.toggleDeafened()
                        else
                            app.calls.toggleDeafened()
                    }
                }
                CallControlButton {
                    objectName: "pipShareFillButton"
                    visible: root.groupLive && root.shareIdShown.length > 0
                    iconName: root.shareFillActive ? "close_fullscreen"
                                                   : "fit_screen"
                    role: root.shareFillActive ? "active" : "neutral"
                    diameter: 30
                    glyphSize: 17
                    tooltip: root.shareFillActive
                             ? qsTr("Show everyone again")
                             : qsTr("Fill this window with the share")
                    onClicked: root.shareFills = !root.shareFills
                }
                CallControlButton {
                    objectName: "pipRestoreButton"
                    iconName: "open_in_full"
                    role: "neutral"
                    diameter: 30
                    glyphSize: 17
                    tooltip: qsTr("Back to Lightning")
                    onClicked: root.restoreRequested()
                }
                CallControlButton {
                    objectName: "pipHangUpButton"
                    iconName: "call_end"
                    role: "danger"
                    diameter: 30
                    glyphSize: 17
                    tooltip: qsTr("Leave the call")
                    onClicked: {
                        if (root.groupLive)
                            app.groupCall.leave()
                        else
                            app.calls.hangUp()
                    }
                }
            }
        }
    }

    readonly property bool micMuted: root.groupLive
                                     ? app.groupCall.microphoneMuted
                                     : (app.calls ? app.calls.microphoneMuted
                                                  : false)
    readonly property bool deafened: root.groupLive ? app.groupCall.deafened
                                                    : (app.calls
                                                       ? app.calls.deafened
                                                       : false)

    /// Put the main window back in front and stand this one down. The host
    /// owns raising its own window — this component never touches it.
    signal restoreRequested()
}
