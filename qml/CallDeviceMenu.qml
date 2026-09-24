import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Device chooser for a call control: the chevron dropdown beside the mic,
// speaker and camera buttons.
//
// The microphone menu also carries the input level. It writes the same
// `app.settings.microphoneGain` as Settings, and SfuCallController applies
// `microphoneGainChanged` to the live pipeline. There is no global output
// level (only per-participant volumes and deafen), so no slider for that.
//
// Lists what the machine reports, marks the system default, and offers
// "System default" as an explicit first choice. `chosen` (what the user
// picked) and `active` (what audio flows through) differ when a chosen device
// is unplugged; the menu then shows both.
AppMenu {
    id: root

    /// "microphone" | "speaker" | "camera"
    property string kind: "microphone"

    // Rows bind on `kind` rather than branch at construction: a hidden
    // AppMenuItem / AppMenuSeparator collapses to implicitHeight 0, so unused
    // rows take no space.
    readonly property bool isMicrophone: kind === "microphone"

    readonly property var _entries: {
        var _ = root.refreshTick
        if (kind === "speaker")
            return app.callDevices.speakers
        if (kind === "camera")
            return app.callDevices.cameras
        return app.callDevices.microphones
    }

    // The rows are rebuilt on every read, so the menu refreshes when the device
    // list or selection changes.
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
        // Explicit: no selection means "follow the system default", which
        // differs from pinning today's default device.
        text: qsTr("System default")
        // AppMenuItem's own radio idiom; Qt's `checkable` would draw its
        // default indicator over the custom content.
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
            // The device's own name; not ours to translate.
            text: modelData.description
                  + (modelData.isDefault ? " " + qsTr("(default)") : "")
            radio: true
            radioSelected: modelData.chosen
            onTriggered: root.selectDevice(modelData.id)
        }
    }

    // Only when the chosen device is unavailable.
    Loader {
        active: root.isMicrophone
                && app.callDevices.preferredMicrophoneMissing
        visible: active
        sourceComponent: AppMenuItem {
            text: qsTr("Your chosen microphone isn't connected")
            enabled: false
        }
    }

    // ── Input level ──
    AppMenuSeparator { visible: root.isMicrophone }

    // A plain Item, not a Layout: QQuickMenu sizes rows it didn't size
    // explicitly to the content width, so this anchors to its own edges and
    // reports a height. implicitWidth is a floor for the menu's width.
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
            // AppMenuItem's content inset, so the label aligns with the
            // devices.
            anchors.leftMargin: AppTheme.menuItemPadding + 6
            anchors.rightMargin: AppTheme.menuItemPadding + 6
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing6

                Icon {
                    // `graphic_eq` for level above 100, `mic` otherwise; both
                    // mapped in Icon.qml (the bundled font is a subset).
                    name: micGain.value > 100 ? "graphic_eq" : "mic"
                    size: 15
                    color: micGain.value > 100 ? AppTheme.bolt
                                               : AppTheme.stormTextSecondary
                }
                Label {
                    // fillWidth + elide, not a minimumWidth floor, which would
                    // make the row overflow the menu.
                    Layout.fillWidth: true
                    text: qsTr("Input volume")
                    elide: Label.ElideRight
                    color: AppTheme.stormTextSecondary
                    font.pixelSize: AppTheme.textMeta
                    font.weight: AppTheme.weightMedium
                }
                Label {
                    objectName: "callMenuMicGainReadout"
                    // Never empty, so no Loader needed.
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
                // Same range and neutral point as the Settings and
                // per-participant controls.
                from: 0
                to: 200
                stepSize: 1
                snapMode: Slider.SnapAlways
                // A plain binding, so changes from Settings, resets or another
                // account's value show here.
                value: app.settings.microphoneGain
                Accessible.name: qsTr("Input volume")
                // `onMoved`, never `onValueChanged`, which also fires for
                // values arriving from the store and would write them straight
                // back.
                onMoved: app.settings.microphoneGain = Math.round(value)

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
                    // White: a dark thumb on the fill boundary reads as
                    // disabled.
                    color: "#FFFFFF"
                    border.width: micGain.visualFocus ? 2 : 0
                    border.color: AppTheme.bolt
                }
            }

            Label {
                Layout.fillWidth: true
                // Preferred width 1 + fillWidth: a wrapping paragraph reports
                // its unwrapped width as preferred and would squeeze siblings.
                Layout.preferredWidth: 1
                // Hidden below 100; a ColumnLayout skips invisible children, so
                // no preferredHeight override (a common binding-loop source on
                // wrapping labels).
                visible: micGain.value > 100
                wrapMode: Text.WordWrap
                text: qsTr("Above 100% amplifies and can clip.")
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
            }
        }
    }

    // Shown only off the neutral point, as a real row rather than a hidden
    // slider gesture.
    AppMenuItem {
        objectName: "callMenuMicGainReset"
        visible: root.isMicrophone && app.settings.microphoneGain !== 100
        text: qsTr("Reset input volume")
        // Written explicitly: moving the slider's value programmatically
        // doesn't fire onMoved.
        onTriggered: app.settings.microphoneGain = 100
    }

    // ── Everything else ──
    // Camera picker, float-the-call and playback level are settings, not call
    // controls; one row links to the section that owns them.
    AppMenuSeparator {}

    AppMenuItem {
        objectName: "callMenuSoundSettings"
        iconName: "settings"
        text: qsTr("Sound settings…")
        onTriggered: app.showSettingsSection("sound")
    }
}
