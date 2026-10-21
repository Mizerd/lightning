import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// An inline colour picker. The platform ColorDialog is modal and would cover
// the theme editor's live preview, which is the point of the editor.
//
// A saturation/value field, a hue slider, a hex field, a before/after pair and
// the base theme's colours as swatches. No alpha: translucent shell surfaces
// make contrast unknowable, and every contrast rule assumes opaque values
// (CustomThemeStore refuses 8-digit hex too).
//
// Its chrome uses AppTheme's editor tokens and its own text field, since a
// picker painted in the theme being edited could be made invisible (see
// ThemeEditorDialog). The hex literals below aren't theme colours: the hue
// strip is the sRGB spectrum, and the crosshair is a white ring on a dark halo
// so it stays visible on any colour.
Item {
    id: root

    // The colour being edited. Assigning loads the picker; it isn't written
    // back here: the host listens to `picked`.
    property color selectedColor: "#000000"
    // The role's colour before this session, for the before/after swatch.
    property color originalColor: "#000000"
    property string title: ""
    property string subtitle: ""
    property bool canReset: false
    // The reset button's words: a role that follows another goes back to
    // following it, which "the base theme" would misdescribe.
    property string resetLabel: qsTr("Reset to the base theme")
    // "#RRGGBB" one-click choices from the base theme's palette, since themes
    // mostly reuse existing tones.
    property var suggestions: []
    // Host controls shown between the heading and the picker: the theme
    // editor puts a surface role's Fill choice (solid or gradient) here, so
    // it is the first thing that role offers rather than something found
    // below the swatches. Shown only while `accessoryVisible`.
    property alias accessory: accessorySlot.data
    property bool accessoryVisible: false
    // One line naming what the picker below is changing, when that is not
    // simply the role in the heading (a gradient's colour, say).
    property string bodyCaption: ""

    signal picked(color value)
    signal resetRequested()
    signal closed()
    // One deliberate change is complete: a drag released, a swatch or the
    // "before" half clicked, a hex value applied. The theme editor ends an
    // undo step here, so a drag is one step rather than every sample.
    signal gestureFinished()

    implicitWidth: 288
    implicitHeight: layout.implicitHeight

    // HSV is the source of truth while open: round-tripping through RGB loses
    // the hue of fully desaturated or dark colours.
    property real hue: 0
    property real sat: 0
    property real val: 0
    property bool loading: false

    // A host that assigns `selectedColor` directly (Settings' name colour
    // picker does on open) must still get a picker showing that colour: HSV is
    // the source of truth, so without this the thumb, the hue and the hex field
    // keep whatever they last held. An assignment this panel makes itself
    // (commit, show, applyColor) already matches its HSV and is skipped.
    onSelectedColorChanged: {
        if (loading)
            return
        if (root.toHex(selectedColor)
                !== root.toHex(Qt.hsva(hue, sat, val, 1)))
            root.load(selectedColor)
    }

    // Becoming visible starts a new editing session: the "before" half and
    // Revert go back to the colour shown NOW, not to whatever the last session
    // started from. The selectedColor handler above cannot do this when the
    // host re-assigns the colour the panel already holds (reopen after a drag).
    onVisibleChanged: {
        if (visible && !loading)
            originalColor = selectedColor
    }

    // Moves a keyboard-operated control's value by one step.
    function nudge(dSat, dVal, dHue) {
        hue = Math.max(0, Math.min(0.9999, hue + dHue))
        sat = Math.max(0, Math.min(1, sat + dSat))
        val = Math.max(0, Math.min(1, val + dVal))
        root.commit()
        root.gestureFinished()
    }

    function load(c) {
        root.show(c)
        originalColor = selectedColor
    }

    // Shows a colour without making it the "before" half or emitting
    // `picked`: for a change made elsewhere (undo) while the picker is open.
    function show(c) {
        loading = true
        var col = typeof c === "string" ? Qt.color(c) : c
        hue = col.hsvHue >= 0 ? col.hsvHue : 0
        sat = col.hsvSaturation
        val = col.hsvValue
        selectedColor = col
        hexField.text = root.toHex(col)
        root.hexInvalid = false
        loading = false
    }

    // A typed value as "#RRGGBB", or "" if it is not one. Forgiving about
    // what people paste: no '#', lower case, surrounding spaces, and the
    // three-digit shorthand ("#abc" is "#AABBCC").
    function normalizeHex(text) {
        var t = String(text).trim()
        if (t.charAt(0) === "#")
            t = t.substring(1)
        if (/^[0-9A-Fa-f]{3}$/.test(t))
            t = t.charAt(0) + t.charAt(0) + t.charAt(1) + t.charAt(1)
                + t.charAt(2) + t.charAt(2)
        if (!/^[0-9A-Fa-f]{6}$/.test(t))
            return ""
        return "#" + t.toUpperCase()
    }

    // Set when the field is left holding something that is not a colour, so
    // it can say so instead of silently ignoring it.
    property bool hexInvalid: false

    function toHex(c) {
        function two(v) {
            var s = Math.round(v * 255).toString(16).toUpperCase()
            return s.length < 2 ? "0" + s : s
        }
        return "#" + two(c.r) + two(c.g) + two(c.b)
    }

    function commit() {
        if (loading)
            return
        var c = Qt.hsva(hue, sat, val, 1)
        selectedColor = c
        hexField.text = root.toHex(c)
        root.picked(c)
    }

    function applyColor(c) {
        loading = true
        hue = c.hsvHue >= 0 ? c.hsvHue : hue
        sat = c.hsvSaturation
        val = c.hsvValue
        selectedColor = c
        hexField.text = root.toHex(c)
        root.hexInvalid = false
        loading = false
        root.picked(c)
    }

    // Back to the colour the picker opened with.
    function revert() {
        root.applyColor(root.originalColor)
        root.gestureFinished()
    }

    ColumnLayout {
        id: layout
        anchors.fill: parent
        spacing: AppTheme.spacing8

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Label {
                Layout.fillWidth: true
                text: root.title
                color: AppTheme.editorText
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightStrong
                elide: Label.ElideRight
            }
            Rectangle {
                objectName: "colorPickerCloseButton"
                implicitWidth: 26
                implicitHeight: 26
                radius: AppTheme.radiusSm
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Close the colour picker")
                Keys.onPressed: (e) => {
                    if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter
                            || e.key === Qt.Key_Space) {
                        root.closed()
                        e.accepted = true
                    }
                }
                border.width: activeFocus ? 2 : 0
                border.color: AppTheme.editorAccent
                color: closeHover.containsMouse ? AppTheme.editorSelection
                                                : "transparent"
                Icon {
                    anchors.centerIn: parent
                    name: "close"
                    size: 14
                    color: AppTheme.editorTextSecondary
                }
                MouseArea {
                    id: closeHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Close the colour picker")
                    onClicked: root.closed()
                }
            }
        }

        Label {
            // Remote or externally chosen text: never markup.
            textFormat: Text.PlainText
            Layout.fillWidth: true
            visible: root.subtitle.length > 0
            text: root.subtitle
            wrapMode: Text.WordWrap
            color: AppTheme.editorTextMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }

        ColumnLayout {
            id: accessorySlot
            objectName: "colorPickerAccessory"
            Layout.fillWidth: true
            Layout.topMargin: AppTheme.spacing4
            visible: root.accessoryVisible
            spacing: AppTheme.spacing8
        }

        Label {
            objectName: "colorPickerBodyCaption"
            Layout.fillWidth: true
            visible: root.bodyCaption.length > 0
            text: root.bodyCaption
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: AppTheme.editorText
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
            font.weight: AppTheme.weightStrong
        }

        // Saturation (x) against value (y), over the current hue.
        Item {
            id: field
            objectName: "colorSvField"
            Layout.fillWidth: true
            Layout.preferredHeight: 176
            activeFocusOnTab: true
            Accessible.role: Accessible.Slider
            Accessible.name: qsTr("Saturation and brightness")
            Accessible.onIncreaseAction: root.nudge(0.02, 0, 0)
            Accessible.onDecreaseAction: root.nudge(-0.02, 0, 0)
            // Arrows move the thumb; Shift takes a larger step.
            Keys.onPressed: (e) => {
                var st = (e.modifiers & Qt.ShiftModifier) ? 0.1 : 0.02
                if (e.key === Qt.Key_Left) root.nudge(-st, 0, 0)
                else if (e.key === Qt.Key_Right) root.nudge(st, 0, 0)
                else if (e.key === Qt.Key_Up) root.nudge(0, st, 0)
                else if (e.key === Qt.Key_Down) root.nudge(0, -st, 0)
                else return
                e.accepted = true
            }
            Rectangle {
                anchors.fill: parent
                radius: AppTheme.radiusSm
                z: 5
                color: "transparent"
                visible: field.activeFocus
                border.width: 2
                border.color: AppTheme.editorAccent
            }

            Rectangle {
                anchors.fill: parent
                radius: AppTheme.radiusSm
                clip: true
                color: Qt.hsva(root.hue, 1, 1, 1)

                // White to the left, black toward the bottom: two gradients
                // rather than a shader for this small static surface.
                Rectangle {
                    anchors.fill: parent
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: "#FFFFFFFF" }
                        GradientStop { position: 1.0; color: "#00FFFFFF" }
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: "#00000000" }
                        GradientStop { position: 1.0; color: "#FF000000" }
                    }
                }
            }

            // Crosshair.
            Rectangle {
                x: root.sat * field.width - width / 2
                y: (1 - root.val) * field.height - height / 2
                width: 16
                height: 16
                radius: 8
                color: "transparent"
                border.width: 2
                border.color: "#FFFFFF"
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 2
                    radius: width / 2
                    color: "transparent"
                    border.width: 1
                    border.color: "#80000000"
                }
            }

            MouseArea {
                // Don't let a drag become a Flickable scroll when hosted inline
                // in a scrolling page (Settings' name-colour picker).
                preventStealing: true
                anchors.fill: parent
                onPositionChanged: (m) => field.pick(m)
                onPressed: (m) => field.pick(m)
                onReleased: root.gestureFinished()
            }
            function pick(m) {
                root.sat = Math.max(0, Math.min(1, m.x / width))
                root.val = Math.max(0, Math.min(1, 1 - m.y / height))
                root.commit()
            }
        }

        // Hue.
        Item {
            id: hueStrip
            objectName: "colorHueStrip"
            Layout.fillWidth: true
            Layout.preferredHeight: 22
            activeFocusOnTab: true
            Accessible.role: Accessible.Slider
            Accessible.name: qsTr("Hue")
            Accessible.onIncreaseAction: root.nudge(0, 0, 0.01)
            Accessible.onDecreaseAction: root.nudge(0, 0, -0.01)
            Keys.onPressed: (e) => {
                var st = (e.modifiers & Qt.ShiftModifier) ? 0.05 : 0.01
                if (e.key === Qt.Key_Left || e.key === Qt.Key_Down)
                    root.nudge(0, 0, -st)
                else if (e.key === Qt.Key_Right || e.key === Qt.Key_Up)
                    root.nudge(0, 0, st)
                else return
                e.accepted = true
            }
            Rectangle {
                anchors.fill: parent
                radius: 11
                z: 5
                color: "transparent"
                visible: hueStrip.activeFocus
                border.width: 2
                border.color: AppTheme.editorAccent
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                height: 14
                radius: 7
                clip: true
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.000; color: "#FF0000" }
                    GradientStop { position: 0.167; color: "#FFFF00" }
                    GradientStop { position: 0.333; color: "#00FF00" }
                    GradientStop { position: 0.500; color: "#00FFFF" }
                    GradientStop { position: 0.667; color: "#0000FF" }
                    GradientStop { position: 0.833; color: "#FF00FF" }
                    GradientStop { position: 1.000; color: "#FF0000" }
                }
            }
            // A round handle that shows the hue it's on.
            Rectangle {
                x: root.hue * hueStrip.width - width / 2
                anchors.verticalCenter: parent.verticalCenter
                width: 20
                height: 20
                radius: 10
                color: Qt.hsva(root.hue, 1, 1, 1)
                border.width: 3
                border.color: "#FFFFFF"
                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color: "transparent"
                    border.width: 1
                    border.color: "#66000000"
                }
            }
            MouseArea {
                // Don't let a drag become a Flickable scroll when hosted inline
                // in a scrolling page (Settings' name-colour picker).
                preventStealing: true
                anchors.fill: parent
                onPositionChanged: (m) => hueStrip.pick(m)
                onPressed: (m) => hueStrip.pick(m)
                onReleased: root.gestureFinished()
            }
            function pick(m) {
                root.hue = Math.max(0, Math.min(0.9999, m.x / width))
                root.commit()
            }
        }

        // Hex + before/after.
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            // Before | after, sharing one outline so they read as one control.
            Rectangle {
                implicitWidth: 62
                implicitHeight: 34
                radius: AppTheme.radiusSm
                color: "transparent"
                border.width: 1
                border.color: AppTheme.editorBorderStrong
                Row {
                    anchors.fill: parent
                    anchors.margins: 1
                    // The "before" half: click it to go back.
                    Rectangle {
                        id: beforeHalf
                        objectName: "colorBeforeSwatch"
                        width: parent.width / 2
                        height: parent.height
                        color: root.originalColor
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Go back to %1")
                                         .arg(root.toHex(root.originalColor))
                        Keys.onPressed: (e) => {
                            if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter
                                    || e.key === Qt.Key_Space) {
                                root.revert()
                                e.accepted = true
                            }
                        }
                        Rectangle {
                            anchors.fill: parent
                            visible: beforeHalf.activeFocus
                                     || beforeHover.containsMouse
                            color: "transparent"
                            border.width: 2
                            border.color: AppTheme.editorAccent
                        }
                        MouseArea {
                            id: beforeHover
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.revert()
                        }
                    }
                    Rectangle {
                        width: parent.width / 2
                        height: parent.height
                        color: root.selectedColor
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 34
                radius: AppTheme.radiusMd
                color: AppTheme.editorInset
                border.width: hexField.activeFocus || root.hexInvalid ? 2 : 1
                border.color: root.hexInvalid ? AppTheme.editorDanger
                              : hexField.activeFocus ? AppTheme.editorAccent
                                                     : AppTheme.editorBorderStrong

                TextInput {
                    id: hexField
                    objectName: "colorHexField"
                    anchors.fill: parent
                    anchors.leftMargin: AppTheme.spacing8
                    anchors.rightMargin: AppTheme.spacing8
                    verticalAlignment: TextInput.AlignVCenter
                    color: AppTheme.editorText
                    selectionColor: AppTheme.editorAccent
                    selectedTextColor: AppTheme.editorAccentInk
                    font.family: AppTheme.monoFont
                    font.pixelSize: AppTheme.textMeta
                    maximumLength: 7
                    Accessible.role: Accessible.EditableText
                    Accessible.name: qsTr("Colour, as a hex value")
                    // Apply typed hex only when complete and valid, so the
                    // preview doesn't flicker through partial values.
                    // Six digits apply as typed, with or without '#'; the
                    // three-digit shorthand waits for Enter, since it is also
                    // the first half of a six-digit value.
                    onTextEdited: {
                        root.hexInvalid = false
                        var t = text.trim()
                        if (t.charAt(0) === "#")
                            t = t.substring(1)
                        if (t.length !== 6)
                            return
                        var hex = root.normalizeHex(t)
                        if (hex.length === 0)
                            return
                        root.applyColor(Qt.color(hex))
                        root.gestureFinished()
                    }
                    onEditingFinished: {
                        if (root.loading)
                            return
                        var hex = root.normalizeHex(text)
                        if (hex.length === 0) {
                            root.hexInvalid = text.trim().length > 0
                            return
                        }
                        root.hexInvalid = false
                        if (hex !== root.toHex(root.selectedColor)) {
                            root.applyColor(Qt.color(hex))
                            root.gestureFinished()
                        }
                        // Shown in the form the store keeps.
                        text = hex
                    }
                }
            }
        }

        Label {
            objectName: "colorHexInvalidLabel"
            Layout.fillWidth: true
            visible: root.hexInvalid
            text: qsTr("Not a colour. Use six hex digits, like #3A6EA5.")
            wrapMode: Text.WordWrap
            color: AppTheme.editorDanger
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }

        // The base theme's own colours.
        Label {
            Layout.fillWidth: true
            Layout.topMargin: AppTheme.spacing4
            visible: root.suggestions.length > 0
            text: qsTr("Colours already in this theme")
            color: AppTheme.editorTextMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }
        Flow {
            Layout.fillWidth: true
            visible: root.suggestions.length > 0
            spacing: 4
            Repeater {
                model: root.suggestions
                delegate: Rectangle {
                    id: swatch
                    required property string modelData
                    objectName: "themeSuggestionSwatch"
                    width: 26
                    height: 26
                    radius: AppTheme.radiusSm
                    color: modelData
                    border.width: swatchHover.containsMouse || activeFocus ? 2 : 1
                    border.color: swatchHover.containsMouse || activeFocus
                                  ? AppTheme.editorAccent
                                  : AppTheme.editorBorderStrong
                    // Keyboard reachable, like every other control here.
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Use %1").arg(swatch.modelData)
                    Keys.onPressed: (e) => {
                        if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter
                                || e.key === Qt.Key_Space) {
                            root.applyColor(Qt.color(swatch.modelData))
                            root.gestureFinished()
                            e.accepted = true
                        }
                    }
                    MouseArea {
                        id: swatchHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.applyColor(Qt.color(swatch.modelData))
                            root.gestureFinished()
                        }
                    }
                }
            }
        }

        Rectangle {
            objectName: "colorResetButton"
            Layout.fillWidth: true
            Layout.topMargin: AppTheme.spacing4
            visible: root.canReset
            implicitHeight: 32
            radius: AppTheme.radiusMd
            color: resetHover.containsMouse ? AppTheme.editorSelection
                                            : "transparent"
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: root.resetLabel
            Keys.onPressed: (e) => {
                if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter
                        || e.key === Qt.Key_Space) {
                    root.resetRequested()
                    e.accepted = true
                }
            }
            border.width: activeFocus ? 2 : 1
            border.color: activeFocus ? AppTheme.editorAccent
                                      : AppTheme.editorBorderStrong
            Label {
                anchors.centerIn: parent
                width: Math.min(implicitWidth,
                                parent.width - AppTheme.spacing8 * 2)
                text: root.resetLabel
                elide: Label.ElideRight
                color: AppTheme.editorText
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textMeta
                font.weight: AppTheme.weightStrong
            }
            MouseArea {
                id: resetHover
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.resetRequested()
            }
        }
    }
}
