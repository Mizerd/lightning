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
// Deliberately plain: a saturation/value field, a hue strip, a hex field and a
// before/after pair. No alpha channel, because a translucent shell surface
// composites over whatever is behind it and that makes the resulting contrast
// unknowable, while every contrast rule in this app is written against opaque
// values (CustomThemeStore refuses 8-digit hex for the same reason).
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
    property bool canReset: false

    signal picked(color value)
    signal resetRequested()
    signal closed()

    implicitWidth: 260
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
        hue = c.hsvHue >= 0 ? c.hsvHue : 0
        sat = c.hsvSaturation
        val = c.hsvValue
        selectedColor = c
        originalColor = c
        hexField.text = root.toHex(c)
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
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightStrong
                elide: Label.ElideRight
            }
            IconButton {
                iconName: "close"
                size: "sm"
                storm: true
                Accessible.name: qsTr("Close the colour picker")
                onClicked: root.closed()
            }
        }

        // Saturation (x) against value (y), over the current hue.
        Item {
            id: field
            Layout.fillWidth: true
            Layout.preferredHeight: 150

            Rectangle {
                anchors.fill: parent
                radius: AppTheme.radiusSm
                clip: true
                color: Qt.hsva(root.hue, 1, 1, 1)

                // White on the left, then black toward the bottom. Two
                // gradients rather than a shader: this is a 260x150 static
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
                width: 14
                height: 14
                radius: 7
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
            Layout.preferredHeight: 18

            Rectangle {
                anchors.fill: parent
                radius: AppTheme.radiusSm
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
            Rectangle {
                x: root.hue * hueStrip.width - width / 2
                width: 6
                height: parent.height + 4
                y: -2
                radius: 3
                color: "transparent"
                border.width: 2
                border.color: "#FFFFFF"
            }
            MouseArea {
                anchors.fill: parent
                onPositionChanged: (m) => hueStrip.pick(m)
                onPressed: (m) => hueStrip.pick(m)
            }
            function pick(m) {
                root.hue = Math.max(0, Math.min(0.9999, m.x / width))
                root.commit()
            }
        }

        // Hex, before/after, reset.
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            // Before | after, sharing one outline so the pair reads as one
            // control rather than two swatches.
            Rectangle {
                implicitWidth: 52
                implicitHeight: 28
                radius: AppTheme.radiusSm
                color: "transparent"
                border.width: 1
                border.color: AppTheme.stormBorder
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

            AppTextField {
                id: hexField
                objectName: "colorHexField"
                storm: true
                Layout.fillWidth: true
                Accessible.name: qsTr("Colour, as a hex value")
                // Typed hex is applied only when it is COMPLETE and valid, so
                // the preview does not flicker through the partial values a
                // user types on the way to a full one.
                onTextEdited: {
                    var t = text.trim()
                    if (!/^#[0-9A-Fa-f]{6}$/.test(t))
                        return
                    var c = Qt.color(t)
                    root.hue = c.hsvHue >= 0 ? c.hsvHue : root.hue
                    root.sat = c.hsvSaturation
                    root.val = c.hsvValue
                    root.selectedColor = c
                    root.picked(c)
                }
            }
        }

        AppButton {
            objectName: "colorResetButton"
            Layout.fillWidth: true
            visible: root.canReset
            size: "sm"
            storm: true
            text: qsTr("Reset to the base theme")
            onClicked: root.resetRequested()
        }
    }
}
