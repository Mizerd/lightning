import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Microphone / output / camera pickers for Settings.
//
// Lists are populated lazily: reading them initialises Qt Multimedia, which
// is slow on a PipeWire desktop, so nothing is enumerated until the section
// is shown.
ColumnLayout {
    id: root

    spacing: AppTheme.spacing8

    /// Set by the host when this section becomes visible; construction alone
    /// doesn't enumerate.
    property bool activated: false

    // Bumped on device/selection changes. Bindings that call the controller
    // read it, since Qt can't observe a C++ call as a dependency.
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
            enabled: picker.entries.length > 0
            // "System default" (index 0) follows the system default as it
            // changes, rather than pinning today's default device.
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

        // Only when the machine has no device of this class.
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

    // Only when the chosen device is absent. The choice is kept, so
    // reconnecting the device restores it.
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

    // ── Microphone gain ──
    // What others hear, a property of this computer's microphone (unlike the
    // per-person playback volumes on the call stage). Bound to
    // `app.settings.microphoneGain`; SettingsManager owns the key, clamp and
    // account scoping and is the only writer. QML never touches QSettings.
    ColumnLayout {
        Layout.fillWidth: true
        Layout.topMargin: AppTheme.spacing4
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            Icon {
                // `graphic_eq` for level (mic/mic_off are the mute button's);
                // the only amplitude glyph mapped in Icon.qml (the bundled font
                // is a subset).
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

            background: Rectangle {
                x: micGainSlider.leftPadding
                y: micGainSlider.topPadding + micGainSlider.availableHeight / 2 - 2
                width: micGainSlider.availableWidth
                height: 4
                radius: AppTheme.radiusPill
                color: AppTheme.stormInset

                Rectangle {
                    width: micGainSlider.visualPosition * parent.width
                    height: parent.height
                    radius: AppTheme.radiusPill
                    color: AppTheme.bolt
                }

                // The neutral point (100%), marked as on the call stage's
                // slider.
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
                // White: a dark thumb on the fill boundary reads as disabled.
                color: "#FFFFFF"
                border.width: micGainSlider.visualFocus ? 2 : 0
                border.color: AppTheme.bolt
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            Label {
                Layout.fillWidth: true
                // Preferred width 1 + fillWidth: a wrapping paragraph reports
                // its unwrapped width as preferred and would squeeze the Reset
                // button.
                Layout.preferredWidth: 1
                wrapMode: Text.WordWrap
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                // Always shown, so the consequence is disclosed up front.
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
                    // Written explicitly: a programmatic `value` change doesn't
                    // fire onMoved.
                    onClicked: app.settings.microphoneGain = 100
                }
            }
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
        // In a Flatpak nothing can be listed: the camera portal supplies a
        // camera when it's turned on.
        emptyText: app.callDevices.camerasChosenByDesktop
                   ? qsTr("Your desktop chooses the camera when you turn it on in a call.")
                   : qsTr("No camera was found.")
    }

    // The floating call window.
    CheckBox {
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
        // Says when changes take effect, and which settings are per computer
        // (device ids, global keys) versus per account (the microphone level,
        // SettingsManager::setMicrophoneGain -> setAppearanceValue).
        text: qsTr("Devices belong to this computer, not to your account. "
                   + "The microphone level belongs to your account. "
                   + "A change applies to your next call; during a call you "
                   + "can switch from the controls at the top of the "
                   + "conversation.")
    }
}
