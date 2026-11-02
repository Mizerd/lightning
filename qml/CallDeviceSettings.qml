import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

ColumnLayout {
    id: root

    spacing: AppTheme.spacing8

    property bool activated: false

    onActivatedChanged: {
        if (!activated && app.audioTester.mode !== "idle")
            app.audioTester.stop();
    }
    Component.onDestruction: {
        if (typeof app !== "undefined" && app && app.audioTester
                && app.audioTester.mode !== "idle")
            app.audioTester.stop();
    }

    property int refreshTick: 0
    Connections {
        target: app.callDevices
        function onDevicesChanged() {
            root.refreshTick++;
        }
        function onSelectionChanged() {
            root.refreshTick++;
        }
    }

    component DevicePicker: ColumnLayout {
        id: picker
        property string label: ""
        property string kind: "microphone"
        property var entries: []
        property string activeId: ""
        property string emptyText: ""
        property bool missing: false

        Layout.fillWidth: true
        spacing: 4

        Label {
            // Remote or externally chosen text: never markup.
            textFormat: Text.PlainText
            text: picker.label
            color: AppTheme.stormText
            font.pixelSize: AppTheme.textBody
            font.weight: AppTheme.weightMedium
        }

        AppComboBox {
            objectName: "callDevice_" + picker.kind
            Layout.fillWidth: true
            restoreAfterModelReset: false
            enabled: picker.entries.length > 0
            displayText: picker.missing ? qsTr("Chosen camera not connected")
                                        : currentText
            model: {
                var names = [qsTr("System default")];
                for (var i = 0; i < picker.entries.length; ++i) {
                    var e = picker.entries[i];
                    names.push(e.description + (e.isDefault ? " " + qsTr("(default)") : ""));
                }
                return names;
            }
            currentIndex: {
                var _ = root.refreshTick;
                if (picker.missing)
                    return -1;
                if (picker.activeId === "")
                    return 0;
                for (var i = 0; i < picker.entries.length; ++i) {
                    if (picker.entries[i].id === picker.activeId)
                        return i + 1;
                }
                return 0;
            }
            onActivated: index => {
                var id = index === 0 ? "" : picker.entries[index - 1].id;
                if (picker.kind === "speaker")
                    app.callDevices.selectSpeaker(id);
                else if (picker.kind === "camera")
                    app.callDevices.selectCamera(id);
                else
                    app.callDevices.selectMicrophone(id);
            }
        }

        Loader {
            active: picker.entries.length === 0 && picker.emptyText.length > 0
            visible: active
            Layout.fillWidth: true
            sourceComponent: Label {
                wrapMode: Text.WordWrap
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: picker.emptyText
            }
        }
    }

    DevicePicker {
        label: qsTr("Microphone")
        kind: "microphone"
        entries: {
            var _ = root.refreshTick;
            return root.activated ? app.callDevices.microphones : [];
        }
        activeId: {
            var _ = root.refreshTick;
            return app.callDevices.activeMicrophoneId;
        }
        emptyText: qsTr("No microphone was found. You can still join a call " + "and listen.")
    }

    Loader {
        active: root.activated && app.callDevices.preferredMicrophoneMissing
        visible: active
        Layout.fillWidth: true
        sourceComponent: Label {
            wrapMode: Text.WordWrap
            color: AppTheme.warning
            font.pixelSize: AppTheme.textMeta
            text: qsTr("Your chosen microphone isn't connected. Calls use the " + "system default until it's back.")
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.topMargin: AppTheme.spacing4
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            Icon {
                name: micGainSlider.value > 100 ? "graphic_eq" : "mic"
                size: 18
                color: micGainSlider.value > 100 ? AppTheme.accent : AppTheme.stormTextSecondary
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Microphone volume")
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightMedium
            }
            Label {
                objectName: "microphoneGainReadout"
                text: Math.round(micGainSlider.value) + "%"
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightMedium
            }
        }

        Slider {
            id: micGainSlider
            objectName: "microphoneGainSlider"
            Layout.fillWidth: true
            from: 0
            to: 200
            stepSize: 1
            snapMode: Slider.SnapAlways
            // A plain binding, so changes from elsewhere show here; the first
            // user drag breaks it, as intended.
            value: app.settings.microphoneGain
            Accessible.name: qsTr("Microphone volume")
            // `onMoved`, not `onValueChanged`, which also fires for values from
            // the store and would write them straight back.
            onMoved: app.settings.microphoneGain = Math.round(value)
            onPressedChanged: if (app.settings.holdWritesWhileDragging)
                app.settings.holdWritesWhileDragging(pressed)
            Component.onDestruction: if (pressed
                    && app.settings.holdWritesWhileDragging)
                app.settings.holdWritesWhileDragging(false)
            Keys.onPressed: event => {
                let handled = true
                let target = 0
                const cur = Math.round(value)
                if (event.key === Qt.Key_Home) target = from
                else if (event.key === Qt.Key_End) target = to
                else if (event.key === Qt.Key_PageUp) target = cur + 10
                else if (event.key === Qt.Key_PageDown) target = cur - 10
                else if (event.key === Qt.Key_Up) target = cur + 1
                else if (event.key === Qt.Key_Down) target = cur - 1
                else handled = false
                if (!handled)
                    return
                app.settings.microphoneGain = Math.max(from, Math.min(to, target))
                event.accepted = true
            }

            background: Rectangle {
                x: micGainSlider.leftPadding
                y: micGainSlider.topPadding + micGainSlider.availableHeight / 2 - 2
                width: micGainSlider.availableWidth
                height: 4
                radius: AppTheme.radiusPill
                color: AppTheme.stormInset

                Rectangle {
                    objectName: "microphoneGainFill"
                    x: micGainSlider.mirrored ? parent.width - width : 0
                    width: (micGainSlider.mirrored
                            ? 1 - micGainSlider.visualPosition
                            : micGainSlider.visualPosition) * parent.width
                    height: parent.height
                    radius: AppTheme.radiusPill
                    color: AppTheme.bolt
                }

                Rectangle {
                    objectName: "microphoneGainNeutralMark"
                    x: Math.round(parent.width / 2) - 1
                    y: -3
                    width: 2
                    height: parent.height + 6
                    radius: 1
                    color: AppTheme.stormTextMuted
                }
            }
            handle: Rectangle {
                x: micGainSlider.leftPadding + micGainSlider.visualPosition * (micGainSlider.availableWidth - width)
                y: micGainSlider.topPadding + micGainSlider.availableHeight / 2 - height / 2
                width: 16
                height: 16
                radius: 8
                color: "#FFFFFF"
                border.width: micGainSlider.visualFocus ? 2 : 1
                border.color: micGainSlider.visualFocus ? AppTheme.bolt
                                                        : AppTheme.stormBorderStrong
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            Label {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                wrapMode: Text.WordWrap
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: qsTr("Above 100% amplifies and can clip. 200% applies the maximum the audio stage can reach.")
            }

            Loader {
                active: Math.round(micGainSlider.value) !== 100
                visible: active
                sourceComponent: AppButton {
                    objectName: "microphoneGainReset"
                    storm: true
                    kind: "ghost"
                    text: qsTr("Reset")
                    onClicked: app.settings.microphoneGain = 100
                }
            }
        }
    }

    ColumnLayout {
        id: micTest
        objectName: "microphoneTestSection"
        Layout.fillWidth: true
        Layout.topMargin: AppTheme.spacing4
        spacing: 6

        readonly property bool running: app.audioTester.microphoneTestRunning
        readonly property bool usable: app.audioTester.available
                                       && !app.audioTester.blockedByCall
        property bool playBack: true

        readonly property string notice: {
            if (!app.audioTester.built)
                return qsTr("This build of Lightning has no call media engine, "
                            + "so it can't test audio devices.");
            if (!app.audioTester.available)
                return qsTr("Audio devices can't be tested: the call media "
                            + "engine didn't start on this computer.");
            if (app.audioTester.blockedByCall)
                return qsTr("Not available during a call. In a call, the "
                            + "menu next to the microphone button shows your "
                            + "level.");
            switch (app.audioTester.error) {
            case "microphone":
                return qsTr("The microphone couldn't be opened. Check that "
                            + "it's connected and that Lightning may use it.");
            case "output":
                return qsTr("The output device couldn't be opened.");
            case "no_meter":
                return qsTr("This build can't measure the microphone level.");
            case "pipeline":
            case "unavailable":
                return qsTr("The audio test couldn't start.");
            case "in_call":
                return qsTr("Not available during a call.");
            }
            return "";
        }

        Label {
            text: qsTr("Microphone test")
            color: AppTheme.stormText
            font.pixelSize: AppTheme.textBody
            font.weight: AppTheme.weightMedium
        }
        Label {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            text: micTest.playBack
                  ? qsTr("Say something: the bar shows what others would get, "
                         + "and you'll hear yourself through the output "
                         + "device below. Use headphones, or the microphone "
                         + "hears the playback and echoes.")
                  : qsTr("Say something: the bar shows what others would get.")
        }

        AudioLevelBar {
            objectName: "microphoneTestLevel"
            Layout.fillWidth: true
            Layout.preferredHeight: 10
            active: micTest.running
            level: micTest.running ? app.audioTester.level : 0
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            AppButton {
                objectName: "microphoneTestButton"
                storm: true
                kind: micTest.running ? "secondary" : "primary"
                enabled: micTest.usable
                text: micTest.running ? qsTr("Stop testing")
                                      : qsTr("Test microphone")
                onClicked: {
                    if (micTest.running)
                        app.audioTester.stop();
                    else
                        app.audioTester.startMicrophoneTest(micTest.playBack);
                }
            }
            SettingsCheckBox {
                objectName: "microphoneTestPlayBack"
                palette.windowText: AppTheme.stormText
                text: qsTr("Hear yourself")
                checked: micTest.playBack
                enabled: micTest.usable && !micTest.running
                onToggled: micTest.playBack = checked
            }
            Item {
                Layout.fillWidth: true
            }
            Label {
                objectName: "microphoneTestCountdown"
                visible: micTest.running && app.audioTester.secondsLeft > 0
                text: qsTr("Stops in %1 s").arg(app.audioTester.secondsLeft)
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
            }
        }

        Label {
            objectName: "microphoneTestNotice"
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            visible: text.length > 0
            wrapMode: Text.WordWrap
            color: AppTheme.warning
            font.pixelSize: AppTheme.textMeta
            text: micTest.notice
        }
    }

    DevicePicker {
        label: qsTr("Output device")
        kind: "speaker"
        entries: {
            var _ = root.refreshTick;
            return root.activated ? app.callDevices.speakers : [];
        }
        activeId: {
            var _ = root.refreshTick;
            return app.callDevices.activeSpeakerId;
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: AppTheme.spacing8

        AppButton {
            objectName: "outputTestButton"
            storm: true
            kind: "secondary"
            iconName: "volume_up"
            text: app.audioTester.toneRunning ? qsTr("Playing…")
                                              : qsTr("Play test sound")
            enabled: micTest.usable && !app.audioTester.toneRunning
            onClicked: app.audioTester.playTestSound()
        }
        Item {
            Layout.fillWidth: true
        }
    }

    DevicePicker {
        label: qsTr("Camera")
        kind: "camera"
        entries: {
            var _ = root.refreshTick;
            return root.activated ? app.callDevices.cameras : [];
        }
        activeId: {
            var _ = root.refreshTick;
            return app.callDevices.activeCameraId;
        }
        missing: {
            var _ = root.refreshTick;
            return root.activated && app.callDevices.preferredCameraMissing;
        }
        emptyText: app.callDevices.camerasChosenByDesktop
                   ? qsTr("Your desktop chooses the camera when you turn it on in a call.")
                   : qsTr("No camera was found.")
    }

    Loader {
        objectName: "callDeviceCameraMissing"
        active: root.activated && app.callDevices.preferredCameraMissing
        visible: active
        Layout.fillWidth: true
        sourceComponent: Label {
            wrapMode: Text.WordWrap
            color: AppTheme.warning
            font.pixelSize: AppTheme.textMeta
            text: qsTr("Your chosen camera isn't connected, so calls turn on "
                       + "no camera until it's back or you choose another.")
        }
    }

    SettingsCheckBox {
        objectName: "callPictureInPictureCheck"
        palette.windowText: AppTheme.stormText
        text: qsTr("Float the call when Lightning is minimised")
        checked: app.settings.callPictureInPicture
        onToggled: app.settings.callPictureInPicture = checked
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        lineHeight: AppTheme.lineHeightBody
        lineHeightMode: Text.ProportionalHeight
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        text: qsTr("A small always-on-top window with the call's video and "
                   + "its controls. It appears only while Lightning is "
                   + "minimised or in the tray, and goes away when the window "
                   + "comes back. You can also pop it out at any time from "
                   + "the call controls.")
    }

    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        lineHeight: AppTheme.lineHeightBody
        lineHeightMode: Text.ProportionalHeight
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        text: qsTr("Devices belong to this computer, not to your account. "
                   + "The microphone level belongs to your account. "
                   + "A change applies to your next call; during a call you "
                   + "can switch from the controls at the top of the "
                   + "conversation.")
    }
}
