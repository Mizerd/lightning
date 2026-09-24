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
    // "#RRGGBB" one-click choices from the base theme's palette, since themes
    // mostly reuse existing tones.
    property var suggestions: []

    signal picked(color value)
    signal resetRequested()
    signal closed()

    implicitWidth: 288
    implicitHeight: layout.implicitHeight

    // HSV is the source of truth while open: round-tripping through RGB loses
    // the hue of fully desaturated or dark colours.
    property real hue: 0
    property real sat: 0
    property real val: 0
    property bool loading: false

    function load(c) {
        loading = true
        var col = typeof c === "string" ? Qt.color(c) : c
        hue = col.hsvHue >= 0 ? col.hsvHue : 0
        sat = col.hsvSaturation
        val = col.hsvValue
        selectedColor = col
        originalColor = col
        hexField.text = root.toHex(col)
        loading = false
    }

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
        loading = false
        root.picked(c)
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

        // Saturation (x) against value (y), over the current hue.
        Item {
            id: field
            Layout.fillWidth: true
            Layout.preferredHeight: 176

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
            Layout.fillWidth: true
            Layout.preferredHeight: 22

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
                    Rectangle {
                        width: parent.width / 2
                        height: parent.height
                        color: root.originalColor
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
                border.width: hexField.activeFocus ? 2 : 1
                border.color: hexField.activeFocus ? AppTheme.editorAccent
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
                    onTextEdited: {
                        var t = text.trim()
                        if (!/^#[0-9A-Fa-f]{6}$/.test(t))
                            return
                        root.applyColor(Qt.color(t))
                    }
                }
            }
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
                    border.width: swatchHover.containsMouse ? 2 : 1
                    border.color: swatchHover.containsMouse
                                  ? AppTheme.editorAccent
                                  : AppTheme.editorBorderStrong
                    MouseArea {
                        id: swatchHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Use %1").arg(swatch.modelData)
                        onClicked: root.applyColor(Qt.color(swatch.modelData))
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
            border.width: 1
            border.color: AppTheme.editorBorderStrong
            Label {
                anchors.centerIn: parent
                text: qsTr("Reset to the base theme")
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
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Reset to the base theme")
                onClicked: root.resetRequested()
            }
        }
    }
}
