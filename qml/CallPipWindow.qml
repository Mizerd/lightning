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
// One surface, chosen the way a person would: a live screen share if there
// is one, otherwise the participant the stage has focused, otherwise nobody
// — a voice call gets a name and the controls, not an empty black box.
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

    title: qsTr("Lightning call")
    // A tool window on the platforms that have one — it belongs beside the
    // application rather than in the task switcher as a second app — and
    // always on top, which is the entire point of a floating call window.
    flags: Qt.Window | Qt.WindowStaysOnTopHint | Qt.WindowTitleHint
           | Qt.WindowCloseButtonHint
    width: 340
    height: 232
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

            // Repeaters over the REAL models, not a `get(row)` snapshot.
            // CallStage learned this the hard way: a share's track key fills
            // in AFTER the row appears, and a snapshot taken before it
            // arrives never attaches a sink — the tile renders black for the
            // rest of the call.
            //
            // `active` is also gated on `root.visible`, so a window that is
            // not showing builds no surface and therefore owns no sink. That
            // is what keeps this window from silently stealing the main
            // stage's video the moment it is constructed.
            Repeater {
                model: root.shareModel
                delegate: Loader {
                    id: pipShare
                    required property string shareId
                    required property string ownerIdentity
                    required property string ownerDisplayName
                    required property string trackKey
                    required property bool local

                    anchors.fill: parent
                    active: root.visible && root.groupLive
                            && root.shareRow >= 0
                            && pipShare.shareId === root.shareIdShown
                    visible: active
                    sourceComponent: CallShareTile {
                        shareId: pipShare.shareId
                        ownerIdentity: pipShare.ownerIdentity
                        ownerDisplayName: pipShare.ownerDisplayName
                        trackKey: pipShare.trackKey
                        local: pipShare.local
                        focused: true
                    }
                }
            }
            Repeater {
                model: root.participantModel
                delegate: Loader {
                    id: pipPerson
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
                    active: root.visible && root.groupLive
                            && root.shareRow < 0
                            && root.pinnedIdentityShown.length > 0
                            && pipPerson.identity === root.pinnedIdentityShown
                    visible: active
                    sourceComponent: CallParticipantTile {
                        identity: pipPerson.identity
                        userId: pipPerson.userId
                        displayName: pipPerson.displayName
                        avatarMxc: pipPerson.avatarMxc
                        local: pipPerson.local
                        micKnown: pipPerson.micKnown
                        micMuted: pipPerson.micMuted
                        cameraKnown: pipPerson.cameraKnown
                        cameraOn: pipPerson.cameraOn
                        cameraTrackKey: pipPerson.cameraTrackKey
                        screenSharing: pipPerson.screenSharing
                        mediaKind: "camera"
                        speaking: pipPerson.speaking
                        speakingLevel: pipPerson.speakingLevel
                        handRaised: pipPerson.handRaised
                        connectionQuality: pipPerson.connectionQuality
                        focused: true
                    }
                }
            }

            // Voice-only, or nothing focused: say so rather than show black.
            ColumnLayout {
                anchors.centerIn: parent
                width: parent.width - AppTheme.spacing16 * 2
                spacing: AppTheme.spacing4
                visible: !root.hasSurface || !root.groupLive
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
