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
        function onDevicesChanged() { root.refreshTick++ }
        function onSelectionChanged() { root.refreshTick++ }
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
                var names = [qsTr("System default")]
                for (var i = 0; i < picker.entries.length; ++i) {
                    var e = picker.entries[i]
                    names.push(e.description
                               + (e.isDefault ? " " + qsTr("(default)") : ""))
                }
                return names
            }
            currentIndex: {
                var _ = root.refreshTick
                if (picker.activeId === "")
                    return 0
                for (var i = 0; i < picker.entries.length; ++i) {
                    if (picker.entries[i].id === picker.activeId)
                        return i + 1
                }
                return 0
            }
            onActivated: (index) => {
                var id = index === 0 ? "" : picker.entries[index - 1].id
                if (picker.kind === "speaker")
                    app.callDevices.selectSpeaker(id)
                else if (picker.kind === "camera")
                    app.callDevices.selectCamera(id)
                else
                    app.callDevices.selectMicrophone(id)
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
            var _ = root.refreshTick
            return root.activated ? app.callDevices.microphones : []
        }
        activeId: {
            var _ = root.refreshTick
            return app.callDevices.activeMicrophoneId
        }
        emptyText: qsTr("No microphone was found. You can still join a call "
                        + "and listen.")
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
            text: qsTr("Your chosen microphone isn't connected. Calls use the "
                       + "system default until it's back.")
        }
    }

    DevicePicker {
        label: qsTr("Output device")
        kind: "speaker"
        entries: {
            var _ = root.refreshTick
            return root.activated ? app.callDevices.speakers : []
        }
        activeId: {
            var _ = root.refreshTick
            return app.callDevices.activeSpeakerId
        }
    }

    DevicePicker {
        label: qsTr("Camera")
        kind: "camera"
        entries: {
            var _ = root.refreshTick
            return root.activated ? app.callDevices.cameras : []
        }
        activeId: {
            var _ = root.refreshTick
            return app.callDevices.activeCameraId
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
        text: qsTr("These devices belong to this computer, not to your "
                   + "account. A change applies to your next call; during a "
                   + "call you can switch from the controls at the top of the "
                   + "conversation.")
    }
}
