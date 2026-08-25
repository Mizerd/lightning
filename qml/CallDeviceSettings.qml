import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Microphone / output / camera pickers for Settings.
//
// A separate component rather than another inline block in SettingsScreen.qml,
// which is already one of the largest files in the tree.
//
// The lists are populated LAZILY: reading them initialises Qt Multimedia,
// which costs real time on a PipeWire desktop, so nothing is enumerated until
// this section is actually shown.
ColumnLayout {
    id: root

    spacing: AppTheme.spacing8

    /// Set by the host when this section becomes visible. Enumeration is
    /// deliberately not triggered by mere construction.
    property bool activated: false

    // Bumped when the device list or selection changes; every binding that
    // calls into the controller reads it, because Qt cannot observe a C++
    // function call as a dependency.
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
            text: picker.label
            color: AppTheme.stormText
            font.pixelSize: AppTheme.textBody
            font.weight: AppTheme.weightMedium
        }

        AppComboBox {
            objectName: "callDevice_" + picker.kind
            Layout.fillWidth: true
            enabled: picker.entries.length > 0
            // "System default" is index 0 and is a real choice: it keeps
            // following the system default as it changes, rather than
            // pinning whichever device happens to be default today.
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

        // Only when the machine genuinely has none of this device class, so
        // it does not become permanent furniture.
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

    // Shown only when the chosen device is genuinely absent — the choice is
    // KEPT, so reconnecting the device restores it rather than having
    // silently lost it.
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

    // ── Microphone gain ──────────────────────────────────────────────────
    //
    // What OTHERS hear, not what this device plays. It belongs beside the
    // microphone picker because it is a property of this computer's
    // microphone — some capture devices are simply quiet — and NOT beside the
    // per-person volumes on the call stage, which are the opposite direction
    // and are per person.
    //
    // Two-way bound to `app.settings.microphoneGain`, a Q_PROPERTY with a
    // NOTIFY. No QSettings is touched from QML: SettingsManager owns the key,
    // the clamp and the account scoping, and it is the only writer.
    ColumnLayout {
        Layout.fillWidth: true
        Layout.topMargin: AppTheme.spacing4
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            Icon {
                // `graphic_eq` reads as LEVEL, which is what this control
                // changes. `mic`/`mic_off` are the mute button's icons and
                // reusing one here would say "microphone", not "how loud".
                // The bundled Material Symbols font is a SUBSET — only names
                // mapped in Icon.qml render, anything else is tofu — and of
                // what is mapped this is the only amplitude glyph.
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
            // A plain binding, so a change made anywhere else — another
            // window, another surface, a reset — is reflected here. Qt breaks
            // it on the first user drag, which is the desired behaviour and
            // the same arrangement the Settings text-scale slider uses.
            value: app.settings.microphoneGain
            Accessible.name: qsTr("Microphone volume")
            // `onMoved`, not `onValueChanged`: `onValueChanged` also fires
            // when the BINDING above delivers a value that came from the
            // store, which would write it straight back — a store write per
            // account switch, and a loop waiting for a rounding difference.
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

                // The neutral point. Same mark, same reason, as the
                // per-person slider on the call stage: 100 is the one value
                // that changes nothing, and it must be findable by eye.
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
                // White: the thumb rides the fill boundary, so a dark disc
                // reads as disabled past half range.
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
                wrapMode: Text.WordWrap
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                // Always shown. A consequence disclosed only once the user is
                // already past the line is not a disclosure.
                text: qsTr("Above 100% amplifies and can clip.")
            }

            Loader {
                active: Math.round(micGainSlider.value) !== 100
                visible: active
                sourceComponent: AppButton {
                    objectName: "microphoneGainReset"
                    storm: true
                    kind: "ghost"
                    text: qsTr("Reset")
                    // Written explicitly: assigning `value` is not a user
                    // gesture, so `onMoved` never fires and a reset that only
                    // moved the thumb would change nothing at all.
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
        emptyText: qsTr("No camera was found.")
    }

    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        lineHeight: AppTheme.lineHeightBody
        lineHeightMode: Text.ProportionalHeight
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        // Honest about when a change takes effect, rather than letting the
        // user wonder why a mid-call switch did nothing.
        text: qsTr("These devices belong to this computer, not to your " + "account. A change applies to your next call; during a " + "call you can switch from the controls at the top of the " + "conversation.")
    }
}
