import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

AppMenu {
    id: root

    property string kind: "microphone"

    readonly property bool isMicrophone: kind === "microphone"

    readonly property var _entries: {
        var _ = root.refreshTick
        if (kind === "speaker")
            return app.callDevices.speakers
        if (kind === "camera")
            return app.callDevices.cameras
        return app.callDevices.microphones
    }

    property int refreshTick: 0
    Connections {
        target: app.callDevices
        function onDevicesChanged() { root.refreshTick++ }
        function onSelectionChanged() { root.refreshTick++ }
    }

    function selectDevice(id) {
        if (root.kind === "speaker")
            app.callDevices.selectSpeaker(id)
        else if (root.kind === "camera")
            app.callDevices.selectCamera(id)
        else
            app.callDevices.selectMicrophone(id)
    }

    MenuSectionLabel {
        text: root.kind === "speaker" ? qsTr("Output device")
                                      : (root.kind === "camera"
                                         ? qsTr("Camera") : qsTr("Microphone"))
    }

    AppMenuItem {
        text: qsTr("System default")
        radio: true
        radioSelected: {
            var _ = root.refreshTick
            if (root.kind === "speaker")
                return app.callDevices.activeSpeakerId === ""
            if (root.kind === "camera")
                return app.callDevices.activeCameraId === ""
                       && !app.callDevices.preferredCameraMissing
            return app.callDevices.activeMicrophoneId === ""
        }
        onTriggered: root.selectDevice("")
    }

    AppMenuSeparator {}

    Repeater {
        model: root._entries
        delegate: AppMenuItem {
            required property var modelData
            text: modelData.description
                  + (modelData.isDefault ? " " + qsTr("(default)") : "")
            radio: true
            radioSelected: modelData.chosen
            onTriggered: root.selectDevice(modelData.id)
        }
    }

    Loader {
        active: root.isMicrophone
                && app.callDevices.preferredMicrophoneMissing
        visible: active
        sourceComponent: AppMenuItem {
            text: qsTr("Your chosen microphone isn't connected")
            enabled: false
        }
    }
    Loader {
        objectName: "callMenuCameraMissing"
        active: root.kind === "camera"
                && app.callDevices.preferredCameraMissing
        visible: active
        sourceComponent: AppMenuItem {
            text: qsTr("Your chosen camera isn't connected")
            enabled: false
        }
    }

    readonly property bool showsLevel: root.isMicrophone
                                       && app.groupCall.active
    AppMenuSeparator { visible: root.showsLevel }

    Item {
        objectName: "callMenuMicLevelRow"
        visible: root.showsLevel
        implicitWidth: 200
        implicitHeight: visible
                        ? levelColumn.implicitHeight + AppTheme.spacing6 * 2 : 0

        ColumnLayout {
            id: levelColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: AppTheme.spacing6
            anchors.leftMargin: AppTheme.menuItemPadding + 6
            anchors.rightMargin: AppTheme.menuItemPadding + 6
            spacing: 4

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing6

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Input level")
                    elide: Label.ElideRight
                    color: AppTheme.stormTextSecondary
                    font.pixelSize: AppTheme.textMeta
                    font.weight: AppTheme.weightMedium
                }
                Label {
                    objectName: "callMenuMicLevelState"
                    visible: app.groupCall.microphoneMuted
                             || app.groupCall.microphoneSilent
                    text: app.groupCall.microphoneMuted
                          ? qsTr("Muted") : qsTr("Nothing heard")
                    color: app.groupCall.microphoneMuted
                           ? AppTheme.stormTextMuted : AppTheme.warning
                    font.pixelSize: AppTheme.textMeta
                }
            }

            AudioLevelBar {
                objectName: "callMenuMicLevelBar"
                Layout.fillWidth: true
                Layout.preferredHeight: 8
                active: root.opened && !app.groupCall.microphoneMuted
                level: root.opened ? app.groupCall.microphoneLevel : 0
            }
        }
    }

    AppMenuSeparator { visible: root.isMicrophone }

    Item {
        id: gainRow
        objectName: "callMenuMicGainRow"
        visible: root.isMicrophone
        implicitWidth: 200
        implicitHeight: visible
                        ? gainColumn.implicitHeight + AppTheme.spacing6 * 2 : 0

        ColumnLayout {
            id: gainColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: AppTheme.spacing6
            anchors.leftMargin: AppTheme.menuItemPadding + 6
            anchors.rightMargin: AppTheme.menuItemPadding + 6
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing6

                Icon {
                    name: micGain.value > 100 ? "graphic_eq" : "mic"
                    size: 15
                    color: micGain.value > 100 ? AppTheme.bolt
                                               : AppTheme.stormTextSecondary
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Input volume")
                    elide: Label.ElideRight
                    color: AppTheme.stormTextSecondary
                    font.pixelSize: AppTheme.textMeta
                    font.weight: AppTheme.weightMedium
                }
                Label {
                    objectName: "callMenuMicGainReadout"
                    text: Math.round(micGain.value) + "%"
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textMeta
                    font.weight: AppTheme.weightMedium
                }
            }

            Slider {
                id: micGain
                objectName: "callMenuMicGainSlider"
                Layout.fillWidth: true
                from: 0
                to: 200
                stepSize: 1
                snapMode: Slider.SnapAlways
                value: app.settings.microphoneGain
                Accessible.name: qsTr("Input volume")
                // `onMoved`, never `onValueChanged`, which also fires for
                // values arriving from the store and would write them straight
                // back.
                onMoved: app.settings.microphoneGain = Math.round(value)
                onPressedChanged: if (app.settings.holdWritesWhileDragging)
                    app.settings.holdWritesWhileDragging(pressed)
                Component.onDestruction: if (pressed
                        && app.settings.holdWritesWhileDragging)
                    app.settings.holdWritesWhileDragging(false)

                background: Rectangle {
                    x: micGain.leftPadding
                    y: micGain.topPadding + micGain.availableHeight / 2 - 2
                    width: micGain.availableWidth
                    height: 4
                    radius: AppTheme.radiusPill
                    color: AppTheme.stormInset

                    Rectangle {
                        width: micGain.visualPosition * parent.width
                        height: parent.height
                        radius: AppTheme.radiusPill
                        color: AppTheme.bolt
                    }

                    Rectangle {
                        objectName: "callMenuMicGainNeutralMark"
                        x: Math.round(parent.width / 2) - 1
                        y: -3
                        width: 2
                        height: parent.height + 6
                        radius: 1
                        color: AppTheme.stormTextMuted
                    }
                }
                handle: Rectangle {
                    x: micGain.leftPadding
                       + micGain.visualPosition * (micGain.availableWidth - width)
                    y: micGain.topPadding + micGain.availableHeight / 2 - height / 2
                    width: 14
                    height: 14
                    radius: 7
                    color: "#FFFFFF"
                    border.width: micGain.visualFocus ? 2 : 0
                    border.color: AppTheme.bolt
                }
            }

            Label {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                visible: micGain.value > 100
                wrapMode: Text.WordWrap
                text: qsTr("Above 100% amplifies and can clip.")
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
            }
        }
    }

    AppMenuItem {
        objectName: "callMenuMicGainReset"
        visible: root.isMicrophone && app.settings.microphoneGain !== 100
        text: qsTr("Reset input volume")
        // Written explicitly: moving the slider's value programmatically
        // doesn't fire onMoved.
        onTriggered: app.settings.microphoneGain = 100
    }

    AppMenuSeparator { visible: root.isMicrophone }

    MenuSectionLabel {
        objectName: "callMenuNoiseHeading"
        visible: root.isMicrophone
        text: qsTr("Noise suppression")
    }

    // A Q_INVOKABLE read is not a binding dependency: refreshed on open.
    property var _noiseChoices: []
    onAboutToShow: if (root.isMicrophone && app.groupCall)
                       root._noiseChoices = app.groupCall.noiseSuppressionChoices()

    function noiseTitle(key) {
        if (key === "off") return qsTr("Off")
        if (key === "webrtc") return qsTr("Standard (WebRTC)")
        if (key === "rnnoise") return qsTr("Strong (RNNoise) · Recommended")
        if (key === "deepfilternet") return qsTr("Advanced (DeepFilterNet)")
        return key
    }

    function noiseChoice(key) {
        for (var i = 0; i < root._noiseChoices.length; ++i)
            if (root._noiseChoices[i].key === key)
                return root._noiseChoices[i]
        return null
    }
    component NoiseModeItem: AppMenuItem {
        required property string modeKey
        readonly property var choice: root.noiseChoice(modeKey)
        objectName: "callMenuNoise_" + modeKey
        visible: root.isMicrophone && choice !== null
        text: root.noiseTitle(modeKey)
        radio: true
        radioSelected: app.settings.noiseSuppressionMode === modeKey
        enabled: choice !== null && choice.available
        ToolTip.visible: hovered && choice !== null && !choice.available
                         && (choice.reason || "").length > 0
        ToolTip.text: choice !== null ? (choice.reason || "") : ""
        onTriggered: {
            if (app.groupCall
                    && modeKey === app.groupCall.noiseSuppressionFailedMode
                    && modeKey === app.settings.noiseSuppressionMode)
                app.groupCall.retryNoiseSuppression()
            else
                app.settings.noiseSuppressionMode = modeKey
        }
    }
    NoiseModeItem { modeKey: "off" }
    NoiseModeItem { modeKey: "webrtc" }
    NoiseModeItem { modeKey: "rnnoise" }
    NoiseModeItem { modeKey: "deepfilternet" }

    AppMenuItem {
        objectName: "callMenuNoiseFailed"
        visible: root.isMicrophone && app.groupCall
                 && app.groupCall.noiseSuppressionFailedMode !== ""
        enabled: false
        text: (app.groupCall && app.groupCall.noiseSuppressionFallbackToWebrtc
               ? qsTr("%1 couldn't start; using WebRTC instead.")
               : qsTr("%1 couldn't start; your microphone is sent without it."))
              .arg(root.noiseTitle(app.groupCall
                                   ? app.groupCall.noiseSuppressionFailedMode
                                   : ""))
    }

    AppMenuItem {
        objectName: "callMenuNoiseSettings"
        visible: root.isMicrophone
        iconName: "settings"
        text: qsTr("More about these in Settings → Labs…")
        onTriggered: app.showSettingsSection("labs")
    }

    AppMenuSeparator {}

    AppMenuItem {
        objectName: "callMenuSoundSettings"
        iconName: "settings"
        text: qsTr("Sound settings…")
        onTriggered: app.showSettingsSection("sound")
    }
}
