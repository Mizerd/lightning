import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: root

    objectName: "callHeaderBar"
    signal participantsRequested()

    property bool previewMode: false

    property string placement: "header"
    readonly property bool dock: root.placement === "dock"
    property bool compact: false
    readonly property bool stageOwnsControls:
        app.groupCall.active && app.groupCall.roomId === app.currentRoomId
    property bool narrow: false
    property real fullNeed: 0
    readonly property bool tight: root.compact || root.narrow
    function reassessRoom() {
        if (root.dock || !root.visible || root.width <= 0)
            return;
        if (!root.narrow) {
            root.fullNeed = bar.implicitWidth + AppTheme.spacing16 * 2;
            if (root.width + 0.5 < root.fullNeed)
                root.narrow = true;
        } else if (root.width + 0.5 >= root.fullNeed) {
            root.narrow = false;
        }
    }
    onWidthChanged: reassessRoom()
    onVisibleChanged: reassessRoom()
    Connections {
        target: bar
        function onImplicitWidthChanged() { root.reassessRoom(); }
    }
    readonly property int controlDiameter:
        root.dock ? (root.compact ? 32 : 48) : 40
    readonly property int controlGlyph:
        root.dock ? (root.compact ? 17 : 22) : 19

    readonly property bool legacyLive:
        app.calls.state === CallController.Inviting
        || app.calls.state === CallController.Connecting
        || app.calls.state === CallController.Active
    readonly property bool groupLive: app.groupCall.active
    readonly property bool live: previewMode || legacyLive || groupLive

    readonly property string callRoomId: groupLive ? app.groupCall.roomId
                                                   : app.calls.activeRoomId

    readonly property bool micMuted: groupLive ? app.groupCall.microphoneMuted
                                               : app.calls.microphoneMuted
    readonly property bool deafened: groupLive ? app.groupCall.deafened
                                               : app.calls.deafened
    readonly property bool audioControlAvailable:
        previewMode || (groupLive ? true : app.calls.muteControlAvailable)
    readonly property bool richMedia: previewMode || groupLive
    readonly property bool legacyCamera:
        !previewMode && !groupLive && legacyLive && app.calls.videoSupported
    readonly property string formatText: groupLive ? qsTr("MatrixRTC")
                                                   : qsTr("Direct")
    readonly property string formatTooltip: groupLive
        ? qsTr("MatrixRTC call: media goes through a call server")
        : qsTr("Direct call (m.call): media goes straight to the other person")

    readonly property string stateText: {
        if (root.groupLive) {
            switch (app.groupCall.state) {
            case SfuCallController.Preparing:
            case SfuCallController.Authorizing:
            case SfuCallController.Connecting:
                return qsTr("Connecting…")
            case SfuCallController.Reconnecting:
                return qsTr("Reconnecting…")
            default:
                return qsTr("Voice call")
            }
        }
        if (app.calls.state === CallController.Inviting)
            return qsTr("Calling…")
        if (app.calls.state === CallController.Connecting)
            return qsTr("Connecting…")
        return app.calls.videoCall ? qsTr("Video call") : qsTr("Voice call")
    }

    visible: previewMode
             || (live && callRoomId === app.currentRoomId
                 && (root.dock || !root.stageOwnsControls))
    implicitHeight: visible
                    ? bar.implicitHeight
                      + (root.compact ? AppTheme.spacing4 : AppTheme.spacing12) * 2
                    : 0
    height: implicitHeight
    // Needed: the collapsed strip and the full-screen window host this bar in
    // Loaders that adopt its implicit size. With a 0 implicit width the next
    // row item would be placed over the centred controls.
    implicitWidth: visible
                   ? bar.implicitWidth
                     + (root.compact ? 0 : AppTheme.spacing16 * 2)
                   : 0
    color: root.dock ? "transparent" : AppTheme.stormInset
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: AppTheme.stormBorder
        visible: root.visible && !root.dock
    }

    Rectangle {
        anchors.centerIn: bar
        width: bar.implicitWidth + AppTheme.spacing16 * 2
        height: bar.implicitHeight + AppTheme.spacing8 * 2
        radius: height / 2
        visible: root.dock && root.visible && !root.compact
        color: AppTheme.stormPanel
        border.width: 1
        border.color: AppTheme.stormBorder
    }

    RowLayout {
        id: bar
        objectName: "callHeaderBarRow"
        anchors.verticalCenter: parent.verticalCenter
        x: Math.min(Math.round((parent.width - width) / 2),
                    parent.width - width)
        spacing: AppTheme.spacing8

        RowLayout {
            spacing: 6
            visible: !root.dock
            Layout.rightMargin: root.dock ? 0 : AppTheme.spacing8
            Layout.preferredWidth: visible ? implicitWidth : 0
            Icon {
                name: "call"
                size: 16
                color: root.groupLive
                       && app.groupCall.state === SfuCallController.Reconnecting
                       ? AppTheme.warning : AppTheme.accent
            }
            Text {
                visible: !root.narrow
                text: root.stateText
                color: AppTheme.stormText
                font.pixelSize: 13
                font.weight: Font.Medium
            }
            Text {
                id: formatLabel
                objectName: "callBarFormatLabel"
                visible: !root.narrow
                text: root.formatText
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                HoverHandler { id: formatHover }
                ToolTip.text: root.formatTooltip
                ToolTip.visible: formatHover.hovered
                ToolTip.delay: 500
            }
        }

        RowLayout {
            spacing: 0
            CallControlButton {
                objectName: "callBarMicButton"
                iconName: root.micMuted ? "mic_off" : "mic"
                role: root.micMuted ? "active" : "neutral"
                diameter: root.controlDiameter
                glyphSize: root.controlGlyph
                tooltip: root.micMuted ? qsTr("Unmute microphone")
                                       : qsTr("Mute microphone")
                enabled: root.audioControlAvailable
                onClicked: {
                    if (root.groupLive)
                        app.groupCall.toggleMicrophoneMuted()
                    else
                        app.calls.toggleMicrophoneMuted()
                }
            }
            CallDeviceChevron {
                objectName: "callBarMicChevron"
                visible: !root.tight
                Layout.preferredWidth: visible ? implicitWidth : 0
                kind: "microphone"
                accessibleName: qsTr("Choose microphone")
                Layout.alignment: Qt.AlignVCenter
                Layout.leftMargin: 2
                warn: app.callDevices.preferredMicrophoneMissing
            }
        }

        RowLayout {
            spacing: 0
            CallControlButton {
                objectName: "callBarDeafenButton"
                iconName: root.deafened ? "headset_off" : "headset_mic"
                role: root.deafened ? "active" : "neutral"
                diameter: root.controlDiameter
                glyphSize: root.controlGlyph
                tooltip: root.deafened ? qsTr("Undeafen") : qsTr("Deafen")
                enabled: root.audioControlAvailable
                onClicked: {
                    if (root.groupLive)
                        app.groupCall.toggleDeafened()
                    else
                        app.calls.toggleDeafened()
                }
            }
            CallDeviceChevron {
                objectName: "callBarSpeakerChevron"
                visible: !root.tight
                Layout.preferredWidth: visible ? implicitWidth : 0
                kind: "speaker"
                accessibleName: qsTr("Choose output device")
                Layout.alignment: Qt.AlignVCenter
                Layout.leftMargin: 2
            }
        }

        Loader {
            active: root.legacyCamera
            visible: active
            sourceComponent: RowLayout {
                spacing: 0
                CallControlButton {
                    objectName: "callBarLegacyCameraButton"
                    iconName: app.calls.cameraOn ? "videocam" : "videocam_off"
                    role: app.calls.cameraOn ? "active" : "neutral"
                    diameter: root.controlDiameter
                    glyphSize: root.controlGlyph
                    enabled: app.calls.videoCall
                             || app.calls.state === CallController.Connecting
                             || app.calls.state === CallController.Active
                    tooltip: app.calls.cameraOn ? qsTr("Turn off camera")
                                                : qsTr("Turn on camera")
                    onClicked: app.calls.toggleCamera()
                }
                CallDeviceChevron {
                    objectName: "callBarLegacyCameraChevron"
                    visible: !root.tight
                    Layout.preferredWidth: visible ? implicitWidth : 0
                    kind: "camera"
                    accessibleName: qsTr("Choose camera")
                    Layout.alignment: Qt.AlignVCenter
                    Layout.leftMargin: 2
                }
            }
        }

        Loader {
            active: root.richMedia
            visible: active
            sourceComponent: RowLayout {
                spacing: 0
                CallControlButton {
                    objectName: "callBarCameraButton"
                    iconName: app.groupCall.cameraOn ? "videocam"
                                                     : "videocam_off"
                    role: app.groupCall.cameraOn ? "active" : "neutral"
                    diameter: root.controlDiameter
                    glyphSize: root.controlGlyph
                    tooltip: app.groupCall.cameraOn ? qsTr("Turn off camera")
                                                    : qsTr("Turn on camera")
                    onClicked: app.groupCall.toggleCamera()
                }
                CallDeviceChevron {
                    objectName: "callBarCameraChevron"
                    visible: !root.tight
                    Layout.preferredWidth: visible ? implicitWidth : 0
                    kind: "camera"
                    accessibleName: qsTr("Choose camera")
                    Layout.alignment: Qt.AlignVCenter
                    Layout.leftMargin: 2
                }
            }
        }

        Loader {
            active: root.richMedia
            visible: active
            sourceComponent: RowLayout {
                spacing: 0
                CallControlButton {
                    objectName: "callBarScreenShareButton"
                    iconName: app.groupCall.screenSharing
                              ? "stop_screen_share" : "screen_share"
                    role: app.groupCall.screenSharing ? "active" : "neutral"
                    diameter: root.controlDiameter
                    glyphSize: root.controlGlyph
                    tooltip: app.groupCall.screenSharing
                             ? qsTr("Stop sharing your screen")
                             : qsTr("Share your screen")
                    onClicked: {
                        if (app.groupCall.screenSharing)
                            app.groupCall.stopScreenShare()
                        else
                            app.groupCall.requestScreenShare()
                    }
                }
                AbstractButton {
                    id: shareChevron
                    objectName: "callBarShareOptionsChevron"
                    Layout.preferredWidth: implicitWidth
                    Layout.alignment: Qt.AlignVCenter
                    Layout.leftMargin: 2
                    implicitWidth: 20
                    implicitHeight: 26
                    hoverEnabled: true
                    focusPolicy: Qt.StrongFocus
                    Accessible.role: Accessible.ButtonMenu
                    Accessible.name: qsTr("Screen share options")
                    background: Rectangle {
                        radius: AppTheme.radiusSm
                        color: shareChevron.pressed
                               ? AppTheme.selectedHover
                               : (shareChevron.hovered
                                  || shareChevron.activeFocus
                                  ? AppTheme.hover
                                  : AppTheme.surfaceElevated)
                        border.width: shareChevron.activeFocus ? 2 : 1
                        border.color: shareChevron.activeFocus
                                      ? AppTheme.focusRing
                                      : AppTheme.borderSubtle
                        Behavior on color { ColorAnimation { duration: 90 } }
                    }
                    contentItem: Icon {
                        name: "expand_more"
                        size: 14
                        color: AppTheme.textSecondary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    ToolTip.visible: shareChevron.hovered
                                     || shareChevron.activeFocus
                    ToolTip.delay: 400
                    ToolTip.text: qsTr("Sound, resolution and frame rate")
                    onClicked: {
                        var gap = AppTheme.spacing4
                        var win = shareChevron.Window.window
                        var below = true
                        if (win) {
                            var at = shareChevron.mapToItem(null, 0, 0)
                            below = at.y + shareChevron.height / 2
                                    <= win.height / 2
                        }
                        shareOptions.flipsUp = !below
                        shareOptions.popup(shareChevron, 0, below
                                           ? shareChevron.height + gap
                                           : -Math.max(shareOptions.height,
                                                       shareOptions.implicitHeight)
                                             - gap)
                    }
                    CallShareOptionsMenu {
                        id: shareOptions
                        property bool flipsUp: false
                        onOpened: if (flipsUp) y = -height - AppTheme.spacing4
                        onHeightChanged: if (visible && flipsUp)
                                             y = -height - AppTheme.spacing4
                        onChooseAppsRequested: shareAudioApps.open()
                    }
                    ShareAudioAppsDialog {
                        id: shareAudioApps
                    }
                }
            }
        }

        Loader {
            active: root.richMedia && !root.tight
            visible: active
            sourceComponent: CallControlButton {
                objectName: "callBarHandButton"
                iconName: "front_hand"
                role: app.groupCall.handRaised ? "active" : "neutral"
                diameter: root.controlDiameter
                glyphSize: root.controlGlyph
                tooltip: app.groupCall.handRaised ? qsTr("Lower your hand")
                                                  : qsTr("Raise your hand")
                onClicked: app.groupCall.toggleHandRaised()
            }
        }

        Loader {
            active: root.richMedia && !root.tight
            visible: active
            sourceComponent: CallControlButton {
                id: reactButton
                objectName: "callBarReactButton"
                iconName: "add_reaction"
                role: "neutral"
                diameter: root.controlDiameter
                glyphSize: root.controlGlyph
                tooltip: qsTr("Send a reaction")
                onClicked: reactionPopup.opened ? reactionPopup.close()
                                                : reactionPopup.open()

                EmojiPicker {
                    id: callEmojiPicker
                    objectName: "callEmojiPicker"
                    mode: "reaction"
                    openExpanded: true
                    allowCustomEmoji: false
                    onEmojiChosen: (emoji) => app.groupCall.sendCallReaction(emoji, "")
                }

                Popup {
                    id: reactionPopup
                    objectName: "callBarReactionPopup"
                    x: (reactButton.width - width) / 2
                    y: root.dock ? -height - 8 : reactButton.height + 8
                    padding: 6
                    // Modal without dim, so handlers beneath the popup stay
                    // inactive.
                    modal: true
                    // Required for CloseOnEscape: Qt routes Escape only to a
                    // popup with active focus.
                    focus: true
                    dim: false
                    closePolicy: Popup.CloseOnEscape
                                 | Popup.CloseOnPressOutside
                    background: Rectangle {
                        radius: AppTheme.radiusMd
                        color: AppTheme.surfaceElevated
                        border.width: 1
                        border.color: AppTheme.borderSubtle
                    }

                    contentItem: Row {
                        spacing: 2
                        Repeater {
                            model: [
                                { emoji: "👍", name: "thumbsup",
                                  label: qsTr("Thumbs up") },
                                { emoji: "🎉", name: "party",
                                  label: qsTr("Party") },
                                { emoji: "👏", name: "clapping",
                                  label: qsTr("Applause") },
                                { emoji: "🐶", name: "dog",
                                  label: qsTr("Dog") },
                                { emoji: "🐱", name: "cat",
                                  label: qsTr("Cat") }
                            ]
                            delegate: AbstractButton {
                                id: reactionChoice
                                required property var modelData
                                objectName: "callReactionChoice"
                                implicitWidth: 36
                                implicitHeight: 36
                                hoverEnabled: true
                                focusPolicy: Qt.StrongFocus
                                Accessible.role: Accessible.Button
                                Accessible.name: reactionChoice.modelData.label
                                background: Rectangle {
                                    radius: AppTheme.radiusSm
                                    color: reactionChoice.pressed
                                           ? AppTheme.selectedHover
                                           : (reactionChoice.hovered
                                              || reactionChoice.activeFocus
                                              ? AppTheme.hover
                                              : "transparent")
                                    border.width: reactionChoice.activeFocus
                                                  ? 2 : 0
                                    border.color: AppTheme.focusRing
                                }
                                contentItem: Text {
                                    // Emoji text. The family is resolved in C++
                                    // (QML has no `font.families`; Qt's
                                    // fallback may pick a monochrome face).
                                    textFormat: Text.PlainText
                                    text: reactionChoice.modelData.emoji
                                    font.pixelSize: 20
                                    font.family:
                                        (typeof app !== "undefined" && app
                                         && app.emojiFontFamily) || ""
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                                ToolTip.visible: reactionChoice.hovered
                                ToolTip.delay: 400
                                ToolTip.text: reactionChoice.modelData.label
                                onClicked: {
                                    app.groupCall.sendCallReaction(
                                        reactionChoice.modelData.emoji,
                                        reactionChoice.modelData.name)
                                    reactionPopup.close()
                                }
                            }
                        }
                        AbstractButton {
                            objectName: "callReactionMore"
                            implicitWidth: 36
                            implicitHeight: 36
                            hoverEnabled: true
                            focusPolicy: Qt.StrongFocus
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("More reactions")
                            ToolTip.visible: hovered
                            ToolTip.delay: 400
                            ToolTip.text: qsTr("More reactions")
                            background: Rectangle {
                                radius: AppTheme.radiusSm
                                color: parent.pressed ? AppTheme.selectedHover
                                       : (parent.hovered || parent.activeFocus
                                          ? AppTheme.hover : "transparent")
                                border.width: parent.activeFocus ? 2 : 0
                                border.color: AppTheme.focusRing
                            }
                            contentItem: Icon {
                                name: "add_reaction"
                                size: 20
                                color: AppTheme.textPrimary
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: {
                                callEmojiPicker.openExpanded = true
                                callEmojiPicker.anchorPoint = reactButton.mapToItem(
                                    Overlay.overlay, reactButton.width / 2,
                                    root.dock ? 0 : reactButton.height)
                                reactionPopup.close()
                                callEmojiPicker.open()
                            }
                        }
                    }
                }
            }
        }

        Loader {
            active: root.richMedia && !root.tight
            visible: active
            sourceComponent: CallControlButton {
                objectName: "callBarParticipantsButton"
                iconName: "group"
                role: "neutral"
                diameter: root.controlDiameter
                glyphSize: root.controlGlyph
                tooltip: app.groupCall.participantCount > 0
                         ? qsTr("Show who's in the call (%1)")
                           .arg(app.groupCall.participantCount)
                         : qsTr("Show who's in the call")
                onClicked: root.participantsRequested()
            }
        }

        Loader {
            active: root.live && !root.tight
            visible: active
            sourceComponent: CallControlButton {
                objectName: "callBarPipButton"
                iconName: "close_fullscreen"
                role: "neutral"
                diameter: root.controlDiameter
                glyphSize: root.controlGlyph
                tooltip: qsTr("Pop the call out into a floating window")
                onClicked: {
                    if (app.groupCall && app.groupCall.stageState)
                        app.groupCall.stageState.setPictureInPicture(true)
                }
            }
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 22
            Layout.alignment: Qt.AlignVCenter
            Layout.leftMargin: 2
            Layout.rightMargin: 2
            color: AppTheme.stormBorder
        }

        CallControlButton {
            objectName: "callBarHangUpButton"
            iconName: "call_end"
            role: "danger"
            diameter: root.controlDiameter
            implicitWidth: root.dock ? (root.compact ? 46 : 70) : 58
            glyphSize: root.controlGlyph
            tooltip: qsTr("Leave call")
            onClicked: {
                if (root.groupLive)
                    app.groupCall.leave()
                else
                    app.calls.hangup()
            }
        }
    }
}
