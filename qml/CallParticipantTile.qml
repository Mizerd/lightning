import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import MatrixClient

Item {
    id: root

    /// True under Qt Quick's software renderer, which cannot draw video even
    /// though frames arrive and the sink reports a size. The tile then stays in
    /// its placeholder state and CallStage explains why once. `app` is looked
    /// up defensively because tests load this component standalone.
    readonly property bool softwareRendererHidesVideo:
        typeof app !== "undefined" && app && app.softwareRenderer === true

    property string userId: ""
    property string deviceId: ""
    property string displayName: ""
    property string avatarMxc: ""

    property bool micKnown: false
    property bool micMuted: false
    property bool cameraKnown: false
    property bool cameraOn: false
    property string identity: ""
    property bool screenSharing: false
    property bool handRaised: false
    property string reactionEmoji: ""
    property string connectionQuality: ""

    property string mediaKind: "camera"
    property string cameraTrackKey: ""
    property string screenTrackKey: ""
    readonly property string activeTrackKey: root.mediaKind === "screen" ? root.screenTrackKey : root.cameraTrackKey
    property string unsupportedCodec: {
        // Read so a backup codec published later re-evaluates this.
        app.groupCall.participantCount
        return root.local || root.activeTrackKey.length === 0
            ? "" : app.groupCall.unsupportedVideoCodec(root.activeTrackKey)
    }
    // Re-attach when the routing key arrives; an attach under an empty key
    // never receives a frame. Declared here rather than in a Connections inside
    // the Loader, because an `on…Changed` handler can't reach a property whose
    // name starts with an underscore.
    onActiveTrackKeyChanged: if (videoLoader.item)
        videoLoader.item.attach()

    property bool speaking: false
    property real speakingLevel: 0.0
    property bool local: false
    property bool focused: false
    property bool compact: false
    property bool bare: false
    readonly property bool _drawsCard:
        !root.bare || videoLoader.visible || root.focused || root.activeFocus

    signal activated()

    implicitWidth: compact ? 148 : 240
    implicitHeight: compact ? 96 : 168

    readonly property real ringTarget:
        root.speaking ? 3 + 6 * Math.max(0, Math.min(1, root.speakingLevel)) : 0
    property real ringGap: 0
    onRingTargetChanged: {
        ringMotion.duration = root.ringTarget > root.ringGap ? 60 : 220
        root.ringGap = root.ringTarget
    }
    Component.onCompleted: root.ringGap = root.ringTarget
    Behavior on ringGap {
        NumberAnimation {
            id: ringMotion
            duration: 60
            easing.type: Easing.OutCubic
        }
    }

    readonly property int _avatarSize: {
        var box = Math.min(width, height - (compact ? 18 : 26))
        var size = Math.round(box * (root.bare && !root.compact ? 0.74 : 0.52))
        return Math.max(compact ? 28 : 40,
                        Math.min(size, root.bare && !root.compact ? 148 : 96))
    }

    readonly property string _label: root.local
                                     ? qsTr("You")
                                     : (root.displayName.length > 0
                                        ? root.displayName : root.userId)
    readonly property string _avatarName: root.displayName.length > 0
                                          ? root.displayName
                                          : root.userId

    readonly property bool _volumeOffered:
        !root.local && root.identity.length > 0
        && typeof app !== "undefined" && app && app.groupCall

    function currentVolumePercent() {
        if (!root._volumeOffered)
            return 100;
        var value = app.groupCall.participantVolume(root.identity);
        return (value === undefined || value === null) ? 100 : value;
    }

    property int volumeRevision: 0

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

        Loader {
            id: videoLoader
            anchors.fill: parent
            active: root.identity.length > 0
                    && (root.mediaKind === "screen"
                        ? root.screenSharing
                        : (root.local
                            ? app.groupCall.cameraOn
                            : (root.cameraKnown && root.cameraOn)))
            visible: active && item && item.hasFrame
                     && !root.softwareRendererHidesVideo
            sourceComponent: Item {
                readonly property bool hasFrame:
                    output.videoSink && output.videoSink.videoSize.width > 0

                VideoOutput {
                    id: output
                    anchors.fill: parent
                    fillMode: root.mediaKind === "screen"
                              ? VideoOutput.PreserveAspectFit
                              : VideoOutput.PreserveAspectCrop
                }

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
                    app.groupCall.detachSink(output.videoSink);
                }
                Component.onCompleted: attach()
                Component.onDestruction: detach()

            }
        }

        Item {
            id: avatarBlock
            visible: !videoLoader.visible
            anchors.centerIn: parent
            anchors.verticalCenterOffset: root.compact ? -6 : -8
            width: root._avatarSize
            height: root._avatarSize

            Rectangle {
                anchors.centerIn: parent
                width: parent.width + 2 * root.ringGap
                height: parent.height + 2 * root.ringGap
                radius: width / 2
                color: "transparent"
                border.width: root.bare ? 3 : 2
                border.color: AppTheme.success
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
                name: root._avatarName
                colorKey: root.userId
                size: root._avatarSize
            }
        }

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
                color: Qt.rgba(0, 0, 0, 0.55)

                RowLayout {
                    id: plate
                    objectName: "callTileNameplateRow"
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
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: root._label
                        color: "#FFFFFF"
                        font.pixelSize: root.compact ? 11 : 12
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                        maximumLineCount: 1
                    }
                }
            }
        }

        Loader {
            active: root._label.length > 0 && root.bare && !videoLoader.visible
            visible: active
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: avatarBlock.bottom
            anchors.topMargin: 8
            width: parent.width - 12
            sourceComponent: Text {
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

        RowLayout {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: root.compact ? 6 : 8
            spacing: 4

            Loader {
                active: root.connectionQuality === "poor"
                visible: active
                sourceComponent: CallTileBadge {
                    iconName: "warning"
                    tooltip: qsTr("Poor connection")
                    tone: "danger"
                }
            }
            Loader {
                active: root.handRaised
                visible: active
                sourceComponent: CallTileBadge {
                    iconName: "front_hand"
                    tooltip: qsTr("Hand raised")
                    tone: "accent"
                }
            }
            Loader {
                active: root.screenSharing
                visible: active
                sourceComponent: CallTileBadge {
                    iconName: "screen_share"
                    tooltip: qsTr("Sharing their screen")
                    tone: "accent"
                }
            }
            Loader {
                active: root.cameraKnown && !root.cameraOn && !root.bare
                visible: active
                sourceComponent: CallTileBadge {
                    iconName: "videocam_off"
                    tooltip: qsTr("Camera off")
                    tone: "muted"
                }
            }
            Loader {
                active: false
                visible: active
                sourceComponent: CallTileBadge {
                    objectName: "callTileUnsupportedCodec"
                    iconName: "videocam_off"
                    tooltip: qsTr("Can't play this video: %1 isn't supported by this build").arg(root.unsupportedCodec)
                    tone: "danger"
                }
            }
        }

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
                color: Qt.rgba(0, 0, 0, 0.55)
                border.width: 1
                border.color: Qt.rgba(1, 1, 1, 0.18)

                Text {
                    id: reactionGlyph
                    anchors.centerIn: parent
                    textFormat: Text.PlainText
                    text: root.reactionEmoji
                    color: "#FFFFFF"
                    font.pixelSize: root.compact ? 15 : 20
                    font.family: (typeof app !== "undefined" && app
                                  && app.emojiFontFamily) || ""
                }

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
                HoverHandler { id: micBadgeHover }
                ToolTip.visible: micBadgeHover.hovered
                ToolTip.delay: 350
                ToolTip.text: qsTr("Microphone muted")
            }
        }

        TapHandler {
            acceptedButtons: Qt.LeftButton
            onTapped: root.activated()
        }

        TapHandler {
            enabled: root._volumeOffered
            acceptedButtons: Qt.RightButton
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onTapped: root.openVolumeControl()
        }

        Loader {
            id: volumeAffordance
            active: root._volumeOffered
                    && (tileHover.hovered || volumePopup.visible
                        || root.activeFocus || root._focusWithin)
            visible: active
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: root.compact ? 4 : 6
            sourceComponent: CallControlButton {
                objectName: "callParticipantVolumeButton"
                diameter: root.compact ? 24 : 28
                glyphSize: root.compact ? 15 : 17
                cornerRadius: AppTheme.radiusMd
                iconName: {
                    var _ = root.volumeRevision;
                    return root.currentVolumePercent() > 0 ? "volume_up"
                                                           : "volume_off";
                }
                tooltip: qsTr("Volume for %1").arg(root._label)
                onClicked: root.openVolumeControl()
            }
        }

        HoverHandler { id: tileHover }
    }

    property bool _focusWithin: false
    Connections {
        target: root.Window.window
        enabled: root._volumeOffered
        function onActiveFocusItemChanged() {
            var it = root.Window.window ? root.Window.window.activeFocusItem : null
            var within = false
            while (it) {
                if (it === root) { within = true; break }
                it = it.parent
            }
            root._focusWithin = within
        }
    }
    Connections {
        target: volumePopup
        function onOpened() { volumeSlider.forceActiveFocus() }
    }

    Popup {
        id: volumePopup
        objectName: "callParticipantVolumePopup"
        x: Math.round((root.width - width) / 2)
        y: Math.round((root.height - height) / 2)
        width: 268
        margins: 8
        padding: AppTheme.spacing12
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        onOpened: volumeSlider.value = root.currentVolumePercent()

        background: Rectangle {
            radius: AppTheme.radiusMd
            color: AppTheme.stormPanel
            border.width: 1
            border.color: AppTheme.stormBorder

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
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
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
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
                to: 200
                stepSize: 1
                snapMode: Slider.SnapAlways
                wheelEnabled: true
                // Custom background and handle set `height`, never
                // `implicitHeight`, so the control measured 0 px high in the
                // layout: nothing for a wheel or a press to hit.
                implicitHeight: 24
                Accessible.name: qsTr("Volume for %1").arg(root._label)

                // `onMoved`, never `onValueChanged`: the latter also fires for
                // the programmatic read in onOpened and would write the value
                // back on every open, recording the default the store
                // deliberately omits.
                onMoved: root.applyVolumePercent(value)
                onPressedChanged: if (app.settings.holdWritesWhileDragging)
                    app.settings.holdWritesWhileDragging(pressed)
                Component.onDestruction: if (pressed
                        && app.settings.holdWritesWhileDragging)
                    app.settings.holdWritesWhileDragging(false)

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
                    Layout.preferredWidth: 1
                    wrapMode: Text.WordWrap
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textMeta
                    text: qsTr("Above 100% amplifies and can clip. 200% applies the maximum the audio stage can reach.")
                }

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
                            root.applyVolumePercent(100);
                        }
                    }
                }
            }

            Loader {
                objectName: "callParticipantClientInfo"
                Layout.fillWidth: true
                active: volumePopup.visible && root.userId.length > 0
                        && typeof app !== "undefined" && app && app.settings
                        && app.settings.developerTools === true
                visible: active
                sourceComponent: UserClientsInfo {
                    userId: root.userId
                    deviceId: root.deviceId
                }
            }
        }
    }
}
