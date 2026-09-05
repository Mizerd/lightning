import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// An INLINE colour picker.
//
// It exists because the platform ColorDialog is a modal window: it opens over
// whatever is behind it, and in the theme editor that is the live preview the
// user is picking a colour FOR. Watching the result is the entire point of the
// editor, so a picker that hides it is not a smaller problem than a picker
// that is hard to use — it defeats the feature.
//
// A saturation/value field, a hue slider, a hex field, a before/after pair,
// and the base theme's own colours as one-click swatches. No alpha channel,
// because a translucent shell surface composites over whatever is behind it
// and that makes the resulting contrast unknowable, while every contrast rule
// in this app is written against opaque values (CustomThemeStore refuses
// 8-digit hex for the same reason).
//
// Its chrome uses AppTheme's INVARIANT editor tokens and its own text field,
// for the reason given in ThemeEditorDialog's header: a picker painted in the
// theme it is editing can be made invisible by the thing it is editing.
//
// The hex literals below are deliberate and are NOT theme colours: the hue
// strip is the sRGB spectrum and the crosshair is a white ring over a dark
// halo so it stays visible on any colour underneath it. A themed crosshair
// would disappear exactly when the user dragged onto the theme's own colour.
Item {
    id: root

    // The colour being edited. Assign to load the picker; it does NOT write
    // back here — the host listens to `picked` so the caller decides what a
    // change means.
    property color selectedColor: "#000000"
    // What the role looked like before this editing session, for the
    // before/after swatch.
    property color originalColor: "#000000"
    property string title: ""
    property string subtitle: ""
    property bool canReset: false
    // "#RRGGBB" strings offered as one-click choices — the base theme's own
    // palette. Building a theme almost always means reusing a tone that is
    // already in it; a hand-typed near-miss is how a palette loses coherence.
    property var suggestions: []

    signal picked(color value)
    signal resetRequested()
    signal closed()

    implicitWidth: 288
    implicitHeight: layout.implicitHeight

    // HSV state is the SOURCE of truth while the panel is open, not the
    // colour. Round-tripping through RGB on every drag loses the hue of a
    // fully desaturated or fully dark colour — drag the field to black and
    // the hue strip would jump to red.
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

                // White on the left, then black toward the bottom. Two
                // gradients rather than a shader: this is a small static
                // surface, not something worth a GPU program.
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
                // A drag on the field or the hue strip must not become a Flickable
                // scroll: hosted inline in Settings (the name-colour picker), the page
                // used to steal the press once the pointer moved a few pixels.
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
            // A round handle rather than a bar: it reads as something you can
            // grab, and it shows the hue it is sitting on.
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
                // A drag on the field or the hue strip must not become a Flickable
                // scroll: hosted inline in Settings (the name-colour picker), the page
                // used to steal the press once the pointer moved a few pixels.
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

            // Before | after, sharing one outline so the pair reads as one
            // control rather than two swatches.
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
                    // Typed hex is applied only when it is COMPLETE and valid,
                    // so the preview does not flicker through the partial
                    // values a user types on the way to a full one.
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
