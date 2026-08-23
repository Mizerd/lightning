import QtQuick
import QtQuick.Controls
import MatrixClient

// Device chooser for a call control — the chevron dropdown beside the mic and
// camera buttons.
//
// Lists what the machine actually reports, marks the system default, and
// offers "System default" as an explicit first choice rather than leaving the
// user to guess which entry that is.
//
// Two states the list distinguishes honestly, because they are different
// facts and conflating them is how a user concludes the setting is broken:
//   * `chosen`  — what the user picked.
//   * `active`  — what audio is actually flowing through right now.
// They differ exactly when a chosen device is unplugged, and the menu then
// shows the choice as still chosen while marking the fallback as active.
AppMenu {
    id: root

    /// "microphone" | "speaker" | "camera"
    property string kind: "microphone"

    readonly property var _entries: {
        var _ = root.refreshTick
        if (kind === "speaker")
            return app.callDevices.speakers
        if (kind === "camera")
            return app.callDevices.cameras
        return app.callDevices.microphones
    }

    // The lists are C++ properties, but the rows inside them are rebuilt on
    // every read, so the menu is repopulated when the device list or the
    // selection changes rather than on a timer.
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
        // Explicit, because "no selection" IS a choice — it means "follow the
        // system default as it changes", which is different from pinning
        // whichever device happens to be default today.
        text: qsTr("System default")
        // AppMenuItem has its OWN selected-state idiom (radio +
        // radioSelected, drawn as a StormNode in the indicator column).
        // Qt's `checkable` makes the control draw its default indicator on
        // top of that custom contentItem, which is why the tick landed over
        // the label.
        radio: true
        radioSelected: {
            var _ = root.refreshTick
            if (root.kind === "speaker")
                return app.callDevices.activeSpeakerId === ""
            if (root.kind === "camera")
                return app.callDevices.activeCameraId === ""
            return app.callDevices.activeMicrophoneId === ""
        }
        onTriggered: root.selectDevice("")
    }

    AppMenuSeparator {}

    Repeater {
        model: root._entries
        delegate: AppMenuItem {
            required property var modelData
            // The device's own name. Never translated: it is hardware the
            // system named, not our string.
            text: modelData.description
                  + (modelData.isDefault ? " " + qsTr("(default)") : "")
            radio: true
            radioSelected: modelData.chosen
            onTriggered: root.selectDevice(modelData.id)
        }
    }

    // Only shown when the user's choice is genuinely unavailable, so it does
    // not become permanent furniture.
    Loader {
        active: root.kind === "microphone"
                && app.callDevices.preferredMicrophoneMissing
        visible: active
        sourceComponent: AppMenuItem {
            text: qsTr("Your chosen microphone isn't connected")
            enabled: false
        }
    }
}
