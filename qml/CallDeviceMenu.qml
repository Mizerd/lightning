import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Device chooser for a call control — the chevron dropdown beside the mic and
// camera buttons.
//
// 2026-09-12: it is also where a call's INPUT LEVEL lives. Before this the
// only microphone volume in the application was in Settings, so changing it
// mid-call meant leaving the conversation to find it. The slider below writes
// the same `app.settings.microphoneGain` that Settings does — one stored
// value, two surfaces — and SfuCallController connects
// `microphoneGainChanged` to `applyAudioState()`, which hands it to the
// engine's `volume` element, so a mid-call drag is applied to the live
// pipeline rather than only to the next call.
//
// Only the microphone menu carries it. There is no global OUTPUT level in
// this client — the engine exposes per-participant volumes (the call stage's
// own control) and a deafen switch, and a slider here that reached neither
// would be a control that does nothing, which is worse than no control.
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

    // `kind` is set once by the chevron that owns this menu and never
    // changes, but the rows below still bind rather than branch at
    // construction: a visible-gated AppMenuItem / AppMenuSeparator collapses
    // to implicitHeight 0 (both files say so explicitly), so an unused row
    // costs no band of empty space the way a plain `visible: false` would.
    readonly property bool isMicrophone: kind === "microphone"

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
        active: root.isMicrophone
                && app.callDevices.preferredMicrophoneMissing
        visible: active
        sourceComponent: AppMenuItem {
            text: qsTr("Your chosen microphone isn't connected")
            enabled: false
        }
    }

    // ── Input level ──────────────────────────────────────────────────────
    AppMenuSeparator { visible: root.isMicrophone }

    // A PLAIN Item, not a Layout, as the menu row. QQuickMenu lays its rows
    // out in a ListView and sizes each one whose width was not set
    // explicitly to the content width — so the block is anchored to its own
    // left and right edges and reports a height, which is the only geometry
    // contract a menu row has. The implicitWidth is a floor for the menu's
    // own width binding, not the width this ends up at.
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
            // The same inset AppMenuItem uses for its content, so the label
            // lines up with the device names above it.
            anchors.leftMargin: AppTheme.menuItemPadding + 6
            anchors.rightMargin: AppTheme.menuItemPadding + 6
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing6

                Icon {
                    // `graphic_eq` reads as LEVEL; `mic` is the device. Both
                    // are mapped in Icon.qml — the bundled Material Symbols
                    // font is a SUBSET and an unmapped name renders as tofu.
                    name: micGain.value > 100 ? "graphic_eq" : "mic"
                    size: 15
                    color: micGain.value > 100 ? AppTheme.bolt
                                               : AppTheme.stormTextSecondary
                }
                Label {
                    // fillWidth + elide, never a Layout.minimumWidth floor:
                    // a floor does not create room, it makes the row overflow
                    // its menu instead of shrinking.
                    Layout.fillWidth: true
                    text: qsTr("Input volume")
                    elide: Label.ElideRight
                    color: AppTheme.stormTextSecondary
                    font.pixelSize: AppTheme.textMeta
                    font.weight: AppTheme.weightMedium
                }
                Label {
                    objectName: "callMenuMicGainReadout"
                    // Never empty, so it does not need the Loader treatment a
                    // possibly-empty Label in a repeated row does.
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
                // THE SAME RANGE AND THE SAME NEUTRAL POINT as the Settings
                // control and the per-participant control. Two level sliders
                // in one application that disagree about what 100 means is a
                // worse outcome than either of them being wrong.
                from: 0
                to: 200
                stepSize: 1
                snapMode: Slider.SnapAlways
                // A plain binding: a change made in Settings, or by a reset,
                // or by the next account's stored value, is reflected here.
                value: app.settings.microphoneGain
                Accessible.name: qsTr("Input volume")
                // `onMoved`, never `onValueChanged` — the latter also fires
                // when the binding above delivers a value that came FROM the
                // store, which writes it straight back.
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
                    // White: the thumb rides the fill boundary, so a dark
                    // disc reads as disabled past half range.
                    color: "#FFFFFF"
                    border.width: micGain.visualFocus ? 2 : 0
                    border.color: AppTheme.bolt
                }
            }

            Label {
                Layout.fillWidth: true
                // Preferred width 1 + fillWidth: a wrapping paragraph reports
                // its whole unwrapped sentence as its preferred width, and a
                // parent too narrow for that shrinks its children in
                // proportion to it.
                Layout.preferredWidth: 1
                // Hidden below the neutral point, and a ColumnLayout
                // already excludes an invisible child from its layout — no
                // preferredHeight override, which on a WRAPPING label is how
                // a layout binding loop is usually written.
                visible: micGain.value > 100
                wrapMode: Text.WordWrap
                text: qsTr("Above 100% amplifies and can clip.")
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
            }
        }
    }

    // Only while it is off its neutral point, so it is not permanent
    // furniture — and as a real row rather than a hidden gesture on the
    // slider, because a reset nobody can find is not a reset.
    AppMenuItem {
        objectName: "callMenuMicGainReset"
        visible: root.isMicrophone && app.settings.microphoneGain !== 100
        text: qsTr("Reset input volume")
        // Written explicitly: assigning the slider's value is not a user
        // gesture, so `onMoved` never fires and a reset that only moved the
        // thumb would change nothing at all.
        onTriggered: app.settings.microphoneGain = 100
    }

    // ── Everything else ──────────────────────────────────────────────────
    //
    // The camera picker, the float-the-call switch and the media playback
    // level are genuinely settings, not call controls, and duplicating them
    // onto a menu that opens over a live call would give this application two
    // places to change one value. One row that goes to the section that owns
    // them instead.
    AppMenuSeparator {}

    AppMenuItem {
        objectName: "callMenuSoundSettings"
        iconName: "settings"
        text: qsTr("Sound settings…")
        onTriggered: app.showSettingsSection("sound")
    }
}
