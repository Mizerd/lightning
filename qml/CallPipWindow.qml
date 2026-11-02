import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import MatrixClient

Window {
    id: root

    property var stageState: app.groupCall ? app.groupCall.stageState : null
    property var participantModel:
        app.groupCall ? app.groupCall.participantModel : null
    property var shareModel: app.groupCall ? app.groupCall.shareModel : null

    readonly property bool callLive:
        (app.groupCall && app.groupCall.active)
        || (app.calls && (app.calls.state === CallController.Active
                          || app.calls.state === CallController.Connecting))
    readonly property bool groupLive: app.groupCall && app.groupCall.active

    readonly property bool wanted:
        root.callLive && root.stageState && root.stageState.pictureInPicture

    readonly property int shareRow: {
        if (!root.shareModel || !root.stageState)
            return -1
        var live = root.shareModel.count
        if (root.stageState.spotlightShareId.length > 0)
            return root.shareModel.indexOfShare(root.stageState.spotlightShareId)
        return live > 0 ? 0 : -1
    }
    readonly property int pinnedRow: {
        if (!root.participantModel || !root.stageState
            || root.stageState.pinnedIdentity.length === 0)
            return -1
        var live = root.participantModel.count
        return live > 0
            ? root.participantModel.indexOfIdentity(
                  root.stageState.pinnedIdentity)
            : -1
    }
    readonly property bool hasSurface: root.shareRow >= 0 || root.pinnedRow >= 0
    readonly property int total:
        (root.shareModel ? root.shareModel.count : 0)
        + (root.participantModel ? root.participantModel.count : 0)
    readonly property string shareIdShown: {
        if (!root.shareModel || !root.stageState)
            return ""
        if (root.stageState.spotlightShareId.length > 0)
            return root.stageState.spotlightShareId
        var row = root.shareModel.count > 0 ? root.shareModel.get(0) : null
        return row ? (row.shareId || "") : ""
    }
    readonly property string pinnedIdentityShown:
        root.stageState ? root.stageState.pinnedIdentity : ""

    property bool shareFills: false
    property string fillShareId: ""
    readonly property string fillShareShown: {
        var live = root.shareModel ? root.shareModel.count : 0
        if (live > 0 && root.fillShareId.length > 0
            && root.shareModel.indexOfShare(root.fillShareId) >= 0)
            return root.fillShareId
        return root.shareIdShown
    }
    readonly property bool shareFillActive:
        root.shareFills && root.groupLive && root.fillShareShown.length > 0
    onFillShareShownChanged: if (root.fillShareShown.length === 0) root.shareFills = false
    Shortcut {
        sequence: "Escape"
        enabled: root.shareFillActive
        onActivated: root.shareFills = false
    }

    // No transient parent: several compositors hide or minimise a transient
    // child with its parent, which is exactly when this window is needed.
    transientParent: null

    title: qsTr("Lightning call")
    flags: Qt.Window | Qt.WindowStaysOnTopHint | Qt.WindowTitleHint
           | Qt.WindowCloseButtonHint
    width: 480
    height: 320
    minimumWidth: 240
    minimumHeight: 160
    color: AppTheme.background

    function sync() {
        if (root.wanted && !root.visible)
            root.show()
        else if (!root.wanted && root.visible)
            root.hide()
    }
    onWantedChanged: root.sync()
    Component.onCompleted: root.sync()

    // The flag must not survive the call. CallStageState::clear() only runs on
    // the group lane, but this window also serves the legacy 1:1 lane, so
    // without this the next 1:1 call would pop out by itself. Keyed on the
    // call ending, not the window hiding, so a deliberate pop-out isn't fought.
    onCallLiveChanged: {
        if (!root.callLive && root.stageState
                && root.stageState.pictureInPicture)
            root.stageState.setPictureInPicture(false)
    }

    onClosing: (close) => {
        close.accepted = true
        if (root.stageState)
            root.stageState.setPictureInPicture(false)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            id: surface
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            Repeater {
                id: shareFillTiles
                objectName: "pipShareFill"
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
                            app.calls.hangup()
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

    signal restoreRequested()
}
