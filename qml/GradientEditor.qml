import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
import MatrixClient

// Gradient for one surface role of the custom theme (theme 12), hosted under
// the colour picker in ThemeEditorDialog for roles where
// app.customTheme.isGradientRole(role) is true. Drawn in the editor's own
// chrome tokens (AppTheme.editor*), like everything else in that dialog, so
// it stays legible while the theme being edited is half finished.
//
// Writes go straight to app.customTheme.setGradient()/resetGradient(), as the
// picker writes setColor(); the store sanitises them. The readability line
// grades every check at the WORST stop (CustomThemeStore::auditWithGradients)
// and warns, never refuses. Gradients are not part of the editor's undo
// history (it records colours and the base).
ColumnLayout {
    id: editor
    objectName: "gradientEditor"

    property string role: "background"
    // The role's flat colour in the theme being edited (the editor passes its
    // effectiveColor), used to seed a new gradient and as the fallback.
    property color baseColor: AppTheme.surfaceFlat(role)
    // The resolved palette graded with the gradient (the editor passes its
    // throttled audit palette).
    property var gradedPalette: AppTheme.paletteForTheme(12)
    spacing: AppTheme.spacing8

    readonly property bool available: typeof app !== "undefined" && app
                                      && !!app.customTheme
                                      && app.customTheme.isGradientRole(role)
    // The stored spec for the role, or null.
    readonly property var stored: {
        if (!editor.available)
            return null
        var all = app.customTheme.gradients
        return all && all[editor.role] ? all[editor.role] : null
    }
    readonly property bool hasGradient: !!editor.stored
    readonly property bool radial: editor.hasGradient && editor.stored.type === "radial"
    readonly property int stopCount: editor.hasGradient ? editor.stored.stops.length : 0

    function stopAt(i) {
        if (!editor.hasGradient)
            return editor.baseColor
        return editor.stored.stops[Math.max(0, Math.min(editor.stopCount - 1, i))]
    }
    function hexOf(c) {
        return Qt.color(String(c)).toString().toUpperCase()
    }
    function specWith(changes) {
        var spec = {
            type: editor.hasGradient ? editor.stored.type : "linear",
            angle: editor.hasGradient ? editor.stored.angle : 180,
            stops: editor.hasGradient ? editor.stored.stops.slice(0)
                                      : [editor.hexOf(editor.baseColor),
                                         editor.hexOf(editor.baseColor)]
        }
        for (var k in changes)
            spec[k] = changes[k]
        return spec
    }
    function write(spec) {
        for (var i = 0; i < spec.stops.length; ++i) {
            if (!app.customTheme.isValidColor(spec.stops[i]))
                return false
        }
        return app.customTheme.setGradient(editor.role, spec)
    }
    function setStop(i, text) {
        var hex = String(text).trim().toUpperCase()
        if (hex.length > 0 && hex.charAt(0) !== "#")
            hex = "#" + hex
        if (!app.customTheme.isValidColor(hex))
            return false
        var stops = editor.specWith({}).stops
        stops[i] = hex
        return editor.write(editor.specWith({ stops: stops }))
    }

    // Grades the palette as it would render with every stored gradient.
    readonly property var failures: {
        if (!editor.available || !editor.hasGradient)
            return []
        return app.customTheme.auditWithGradients(editor.gradedPalette,
                                                  app.customTheme.gradients)
    }

    // The editor's button, in its chrome (EditorButton is private to the
    // dialog).
    component ChromeButton: AbstractButton {
        id: btn
        property bool selected: false
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        implicitHeight: 30
        implicitWidth: btnLabel.implicitWidth + AppTheme.spacing16 * 2
        Accessible.role: Accessible.Button
        Accessible.name: text
        background: Rectangle {
            radius: AppTheme.radiusMd
            color: btn.selected ? AppTheme.editorSelection
                 : btn.down ? AppTheme.editorSelection
                 : btn.hovered ? AppTheme.editorInset : "transparent"
            border.width: 1
            border.color: btn.selected ? AppTheme.editorAccent
                                       : AppTheme.editorBorderStrong
            Rectangle {
                anchors.fill: parent
                anchors.margins: 2
                visible: btn.visualFocus
                radius: AppTheme.radiusSm
                color: "transparent"
                border.width: 2
                border.color: AppTheme.editorAccent
            }
        }
        contentItem: Label {
            id: btnLabel
            text: btn.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: AppTheme.editorText
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
            font.weight: AppTheme.weightStrong
            opacity: btn.enabled ? 1.0 : 0.45
        }
    }

    Label {
        Layout.fillWidth: true
        text: qsTr("Gradient")
        color: AppTheme.editorTextMuted
        font.family: AppTheme.menuSectionFont
        font.pixelSize: AppTheme.menuSectionSize
        font.weight: AppTheme.menuSectionWeight
    }

    Flow {
        Layout.fillWidth: true
        spacing: AppTheme.spacing6
        ChromeButton {
            objectName: "gradientFlatButton"
            text: qsTr("Flat")
            selected: !editor.hasGradient
            enabled: editor.available
            onClicked: if (editor.hasGradient) app.customTheme.resetGradient(editor.role)
        }
        ChromeButton {
            objectName: "gradientLinearButton"
            text: qsTr("Linear")
            selected: editor.hasGradient && !editor.radial
            enabled: editor.available
            onClicked: {
                if (!editor.hasGradient) {
                    // Seeded with the flat colour and a gentle step down, so
                    // turning it on changes little until edited.
                    var base = editor.hexOf(editor.baseColor)
                    editor.write({ type: "linear", angle: 180,
                                   stops: [base, editor.hexOf(Qt.darker(editor.baseColor, 1.12))] })
                } else {
                    editor.write(editor.specWith({ type: "linear" }))
                }
            }
        }
        ChromeButton {
            objectName: "gradientRadialButton"
            text: qsTr("Radial")
            selected: editor.radial
            enabled: editor.available
            onClicked: {
                if (!editor.hasGradient) {
                    var base = editor.hexOf(editor.baseColor)
                    editor.write({ type: "radial", angle: 180,
                                   stops: [editor.hexOf(Qt.lighter(editor.baseColor, 1.08)), base] })
                } else {
                    editor.write(editor.specWith({ type: "radial" }))
                }
            }
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        visible: editor.hasGradient
        spacing: AppTheme.spacing8

        // Exactly what will be drawn.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 40
            radius: AppTheme.radiusMd
            color: AppTheme.editorInset
            border.color: AppTheme.editorBorder
            border.width: 1
            ThemedSurface {
                anchors.fill: parent
                anchors.margins: 1
                role: editor.role
                flatFill: false
                specOverride: editor.stored
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: !editor.radial
            spacing: AppTheme.spacing8
            Label {
                text: qsTr("Angle")
                color: AppTheme.editorTextSecondary
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textMeta
            }
            Slider {
                id: angleSlider
                objectName: "gradientAngleSlider"
                Layout.fillWidth: true
                from: 0
                to: 345
                stepSize: 15
                snapMode: Slider.SnapAlways
                value: editor.hasGradient ? editor.stored.angle : 180
                Accessible.name: qsTr("Gradient angle")
                onMoved: editor.write(editor.specWith({ angle: Math.round(value) }))
                background: Rectangle {
                    x: angleSlider.leftPadding
                    y: angleSlider.topPadding + angleSlider.availableHeight / 2 - 2
                    width: angleSlider.availableWidth
                    height: 4
                    radius: AppTheme.radiusPill
                    color: AppTheme.editorInset
                    Rectangle {
                        width: angleSlider.visualPosition * parent.width
                        height: parent.height
                        radius: AppTheme.radiusPill
                        color: AppTheme.editorAccent
                    }
                }
                handle: Rectangle {
                    x: angleSlider.leftPadding
                       + angleSlider.visualPosition * (angleSlider.availableWidth - width)
                    y: angleSlider.topPadding + angleSlider.availableHeight / 2 - height / 2
                    width: 14
                    height: 14
                    radius: 7
                    color: AppTheme.editorPanel
                    border.width: angleSlider.visualFocus ? 2 : 1
                    border.color: angleSlider.visualFocus ? AppTheme.editorAccent
                                                          : AppTheme.editorBorderStrong
                }
            }
            Label {
                Layout.preferredWidth: 36
                horizontalAlignment: Text.AlignRight
                text: qsTr("%1°").arg(Math.round(angleSlider.value))
                color: AppTheme.editorTextMuted
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textMeta
            }
        }

        // The stops, start to end (top to bottom at 180°, centre outwards
        // for radial).
        Repeater {
            model: editor.stopCount
            delegate: RowLayout {
                id: stopRow
                required property int index
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                Rectangle {
                    implicitWidth: 22
                    implicitHeight: 22
                    radius: AppTheme.radiusSm
                    border.color: AppTheme.editorBorderStrong
                    border.width: 1
                    color: editor.stopAt(stopRow.index)
                }
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 28
                    radius: AppTheme.radiusSm
                    color: AppTheme.editorInset
                    border.width: 1
                    border.color: stopField.activeFocus ? AppTheme.editorAccent
                                                        : AppTheme.editorBorder
                    TextInput {
                        id: stopField
                        objectName: "gradientStopField"
                        anchors.fill: parent
                        anchors.leftMargin: AppTheme.spacing8
                        anchors.rightMargin: AppTheme.spacing8
                        verticalAlignment: TextInput.AlignVCenter
                        text: String(editor.stopAt(stopRow.index))
                        color: AppTheme.editorText
                        selectionColor: AppTheme.editorSelection
                        selectedTextColor: AppTheme.editorText
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.textMeta
                        maximumLength: 7
                        selectByMouse: true
                        Accessible.name: qsTr("Gradient colour %1").arg(stopRow.index + 1)
                        onEditingFinished: {
                            if (!editor.setStop(stopRow.index, text))
                                text = String(editor.stopAt(stopRow.index))
                        }
                    }
                }
                ChromeButton {
                    text: qsTr("Remove")
                    // Two stops is the minimum the store accepts.
                    visible: editor.stopCount > 2
                    onClicked: {
                        var stops = editor.specWith({}).stops
                        stops.splice(stopRow.index, 1)
                        editor.write(editor.specWith({ stops: stops }))
                    }
                }
            }
        }
        ChromeButton {
            text: qsTr("Add a middle colour")
            visible: editor.stopCount < 3
            onClicked: {
                var stops = editor.specWith({}).stops
                stops.splice(1, 0, stops[0])
                editor.write(editor.specWith({ stops: stops }))
            }
        }

        Label {
            objectName: "gradientReadability"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
            color: editor.failures.length === 0 ? AppTheme.editorTextSecondary
                                                : AppTheme.editorDanger
            text: {
                if (editor.failures.length === 0)
                    return qsTr("Text stays readable at every point of this gradient.")
                var labels = []
                for (var i = 0; i < editor.failures.length && i < 3; ++i)
                    labels.push(editor.failures[i].label)
                return qsTr("Hard to read where the gradient is at its worst: %1")
                       .arg(labels.join("; "))
            }
        }
    }
}
