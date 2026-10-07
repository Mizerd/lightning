import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
import MatrixClient

// The fill of one surface role of the custom theme (theme 12): a solid colour,
// or a linear or radial gradient. Hosted at the TOP of the colour picker in
// ThemeEditorDialog (ColorPickerPanel's accessory slot) for roles where
// app.customTheme.isGradientRole(role) is true, so the choice is the first
// thing a large surface offers instead of a control found below the picker.
// Drawn in the editor's own chrome tokens (AppTheme.editor*), like everything
// else in that dialog, so it stays legible while the theme being edited is
// half finished.
//
// One click makes a gradient that can be SEEN: the presets and the Linear /
// Radial buttons step the second colour a tenth of the lightness range away
// from the first. The old seed was Qt.darker(base, 1.12), which on a
// near-black ground (#02051D -> #02041A) changed nothing visible, so turning a
// gradient on looked like a control that did nothing.
//
// A stop is edited with the host's picker: clicking a stop emits
// stopSelected(index) and the host points its picker at that stop until
// stopSelected(-1) (the solid colour again).
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
    // The stop the host's picker is editing, or -1 for the solid colour. Set
    // by the host in answer to stopSelected().
    property int selectedStop: -1
    signal stopSelected(int index)

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

    onRoleChanged: if (editor.selectedStop >= 0) editor.stopSelected(-1)
    onHasGradientChanged: {
        if (!editor.hasGradient && editor.selectedStop >= 0)
            editor.stopSelected(-1)
    }

    function stopAt(i) {
        if (!editor.hasGradient)
            return editor.baseColor
        return editor.stored.stops[Math.max(0, Math.min(editor.stopCount - 1, i))]
    }
    function hexOf(c) {
        return Qt.color(String(c)).toString().toUpperCase()
    }
    // `c` moved `amount` of the HSL lightness range towards the middle: a
    // dark ground gets lighter, a light one darker, so the step is visible on
    // both (a near-black colour has no room to get darker).
    function stepped(c, amount) {
        var q = Qt.color(String(c))
        var l = q.hslLightness
        var nl = l < 0.5 ? Math.min(1, l + amount) : Math.max(0, l - amount)
        return editor.hexOf(Qt.hsla(Math.max(0, q.hslHue), q.hslSaturation, nl, 1))
    }
    function mixed(a, b, t) {
        var x = Qt.color(String(a))
        var y = Qt.color(String(b))
        return editor.hexOf(Qt.rgba(x.r + (y.r - x.r) * t,
                                    x.g + (y.g - x.g) * t,
                                    x.b + (y.b - x.b) * t, 1))
    }
    readonly property string baseHex: editor.hexOf(editor.baseColor)
    readonly property string accentHex: {
        var pal = editor.gradedPalette
        if (pal && pal.accent !== undefined)
            return editor.hexOf(pal.accent)
        return editor.baseHex
    }

    // One-click starting points, all built from the role's own colour (and
    // the theme's accent for the last), so each is a variation of the theme
    // rather than a stranger's palette.
    readonly property var presets: [
        { key: "soft", label: qsTr("Soft fade"),
          spec: { type: "linear", angle: 180,
                  stops: [editor.baseHex, editor.stepped(editor.baseColor, 0.07)] } },
        { key: "strong", label: qsTr("Strong fade"),
          spec: { type: "linear", angle: 180,
                  stops: [editor.baseHex, editor.stepped(editor.baseColor, 0.16)] } },
        { key: "diagonal", label: qsTr("Diagonal"),
          spec: { type: "linear", angle: 135,
                  stops: [editor.stepped(editor.baseColor, 0.12), editor.baseHex] } },
        { key: "glow", label: qsTr("Glow"),
          spec: { type: "radial", angle: 180,
                  stops: [editor.stepped(editor.baseColor, 0.12), editor.baseHex] } },
        { key: "accent", label: qsTr("Accent wash"),
          spec: { type: "linear", angle: 180,
                  stops: [editor.baseHex,
                          editor.mixed(editor.baseHex, editor.accentHex, 0.3)] } },
    ]

    function specWith(changes) {
        var spec = {
            type: editor.hasGradient ? editor.stored.type : "linear",
            angle: editor.hasGradient ? editor.stored.angle : 180,
            stops: editor.hasGradient ? editor.stored.stops.slice(0)
                                      : [editor.baseHex,
                                         editor.stepped(editor.baseColor, 0.10)]
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
    function applyPreset(spec) {
        if (!editor.write({ type: spec.type, angle: spec.angle,
                            stops: spec.stops.slice(0) }))
            return false
        // The picker may be on a stop the preset does not have.
        if (editor.selectedStop >= spec.stops.length)
            editor.stopSelected(-1)
        return true
    }
    function reverseStops() {
        var stops = editor.specWith({}).stops
        stops.reverse()
        if (!editor.write(editor.specWith({ stops: stops })))
            return
        // Keep the picker on the colour it was editing, now at the far end.
        if (editor.selectedStop >= 0)
            editor.stopSelected(stops.length - 1 - editor.selectedStop)
    }
    // "Start" / "Middle" / "End"; a radial gradient runs from the centre out.
    function stopName(i) {
        if (i === 0)
            return editor.radial ? qsTr("Centre") : qsTr("Start")
        if (i === editor.stopCount - 1)
            return editor.radial ? qsTr("Edge") : qsTr("End")
        return qsTr("Middle")
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
        implicitWidth: btnLabel.implicitWidth + AppTheme.spacing12 * 2
        Accessible.role: Accessible.Button
        Accessible.name: text
        background: Rectangle {
            radius: AppTheme.radiusMd
            color: btn.selected ? AppTheme.editorSelection
                 : btn.down ? AppTheme.editorSelection
                 : btn.hovered ? AppTheme.editorInset : "transparent"
            border.width: btn.selected ? 2 : 1
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
            elide: Label.ElideRight
            opacity: btn.enabled ? 1.0 : 0.45
        }
    }

    component SectionLabel: Label {
        Layout.fillWidth: true
        color: AppTheme.editorTextMuted
        font.family: AppTheme.menuSectionFont
        font.pixelSize: AppTheme.menuSectionSize
        font.weight: AppTheme.menuSectionWeight
    }

    component Hint: Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        color: AppTheme.editorTextSecondary
        font.family: AppTheme.uiFont
        font.pixelSize: AppTheme.textMeta
    }

    SectionLabel {
        objectName: "gradientFillHeading"
        text: qsTr("Fill")
    }

    // Solid, or one of the two gradient kinds. Three equal columns so the
    // longer labels never push a button onto a second line in the narrow
    // picker column.
    RowLayout {
        Layout.fillWidth: true
        spacing: AppTheme.spacing6
        ChromeButton {
            objectName: "gradientFlatButton"
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            text: qsTr("Solid")
            Accessible.name: qsTr("Solid colour")
            selected: !editor.hasGradient
            enabled: editor.available
            onClicked: if (editor.hasGradient) app.customTheme.resetGradient(editor.role)
        }
        ChromeButton {
            objectName: "gradientLinearButton"
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            text: qsTr("Linear")
            Accessible.name: qsTr("Linear gradient")
            selected: editor.hasGradient && !editor.radial
            enabled: editor.available
            onClicked: {
                if (!editor.hasGradient)
                    editor.write({ type: "linear", angle: 180,
                                   stops: [editor.baseHex,
                                           editor.stepped(editor.baseColor, 0.10)] })
                else
                    editor.write(editor.specWith({ type: "linear" }))
            }
        }
        ChromeButton {
            objectName: "gradientRadialButton"
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            text: qsTr("Radial")
            Accessible.name: qsTr("Radial gradient")
            selected: editor.radial
            enabled: editor.available
            onClicked: {
                if (!editor.hasGradient)
                    editor.write({ type: "radial", angle: 180,
                                   stops: [editor.stepped(editor.baseColor, 0.10),
                                           editor.baseHex] })
                else
                    editor.write(editor.specWith({ type: "radial" }))
            }
        }
    }

    Hint {
        objectName: "gradientSolidHint"
        visible: !editor.hasGradient
        text: qsTr("This area can be a gradient. Pick a starting point, then "
                   + "change its colours.")
    }

    // Presets: what each one looks like, painted from this role's colour.
    Flow {
        id: presetFlow
        objectName: "gradientPresets"
        Layout.fillWidth: true
        spacing: AppTheme.spacing6
        visible: editor.available
        // Three per row, sized from the column: at a fixed 58px "Strong fade"
        // and "Accent wash" were elided in the 240-300px picker column.
        readonly property real tileWidth:
            Math.max(58, Math.floor((presetFlow.width - presetFlow.spacing * 2) / 3))
        Repeater {
            model: editor.presets
            delegate: AbstractButton {
                id: preset
                required property var modelData
                objectName: "gradientPreset_" + modelData.key
                width: presetFlow.tileWidth
                height: presetSwatch.height + presetLabel.implicitHeight + 4
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Use the %1 gradient").arg(modelData.label)
                onClicked: editor.applyPreset(modelData.spec)
                Rectangle {
                    id: presetSwatch
                    width: parent.width
                    height: 34
                    radius: AppTheme.radiusSm
                    color: AppTheme.editorInset
                    border.width: preset.hovered || preset.visualFocus ? 2 : 1
                    border.color: preset.hovered || preset.visualFocus
                                  ? AppTheme.editorAccent
                                  : AppTheme.editorBorderStrong
                    ThemedSurface {
                        anchors.fill: parent
                        anchors.margins: 2
                        role: editor.role
                        flatFill: false
                        specOverride: preset.modelData.spec
                    }
                }
                Label {
                    id: presetLabel
                    anchors.top: presetSwatch.bottom
                    anchors.topMargin: 4
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: preset.modelData.label
                    textFormat: Text.PlainText
                    elide: Label.ElideRight
                    color: AppTheme.editorTextSecondary
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.menuSectionSize
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
            objectName: "gradientPreview"
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
                text: qsTr("Direction")
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

        SectionLabel {
            text: qsTr("Gradient colours")
        }

        // The stops, start to end (top to bottom at 180°, centre outwards for
        // radial). The swatch and the name select the stop for the picker; the
        // field takes a typed value.
        Repeater {
            model: editor.stopCount
            delegate: Rectangle {
                id: stopRow
                required property int index
                readonly property bool selected: editor.selectedStop === stopRow.index
                objectName: "gradientStopRow_" + stopRow.index
                Layout.fillWidth: true
                implicitHeight: 36
                radius: AppTheme.radiusSm
                color: stopRow.selected ? AppTheme.editorSelection
                     : stopHover.hovered ? AppTheme.editorInset : "transparent"
                border.width: stopRow.selected ? 2 : 0
                border.color: AppTheme.editorAccent

                HoverHandler { id: stopHover }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: AppTheme.spacing4
                    anchors.rightMargin: AppTheme.spacing4
                    spacing: AppTheme.spacing8

                    // Swatch + name: the target for "edit this colour".
                    AbstractButton {
                        id: stopPick
                        objectName: "gradientStopSwatch_" + stopRow.index
                        Layout.preferredWidth: 26 + AppTheme.spacing6
                                               + stopNameLabel.implicitWidth
                        Layout.fillHeight: true
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.role: Accessible.Button
                        Accessible.name: stopRow.selected
                            ? qsTr("Stop editing the %1 colour")
                              .arg(editor.stopName(stopRow.index))
                            : qsTr("Edit the %1 colour with the picker")
                              .arg(editor.stopName(stopRow.index))
                        onClicked: editor.stopSelected(stopRow.selected ? -1
                                                                        : stopRow.index)
                        Rectangle {
                            id: stopSwatch
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            width: 26
                            height: 26
                            radius: AppTheme.radiusSm
                            border.color: stopPick.hovered || stopPick.visualFocus
                                          ? AppTheme.editorAccent
                                          : AppTheme.editorBorderStrong
                            border.width: stopPick.hovered || stopPick.visualFocus ? 2 : 1
                            color: editor.stopAt(stopRow.index)
                        }
                        Label {
                            id: stopNameLabel
                            anchors.left: stopSwatch.right
                            anchors.leftMargin: AppTheme.spacing6
                            anchors.verticalCenter: parent.verticalCenter
                            text: editor.stopName(stopRow.index)
                            color: AppTheme.editorText
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            font.weight: stopRow.selected ? AppTheme.weightStrong
                                                          : AppTheme.weightMedium
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 64
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
                            clip: true
                            text: String(editor.stopAt(stopRow.index)).toUpperCase()
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
                                    text = String(editor.stopAt(stopRow.index)).toUpperCase()
                            }
                        }
                    }
                    ChromeButton {
                        objectName: "gradientStopRemove_" + stopRow.index
                        text: qsTr("Remove")
                        Accessible.name: qsTr("Remove the %1 colour")
                                         .arg(editor.stopName(stopRow.index))
                        // Two stops is the minimum the store accepts.
                        visible: editor.stopCount > 2
                        onClicked: {
                            var stops = editor.specWith({}).stops
                            stops.splice(stopRow.index, 1)
                            if (editor.selectedStop >= 0)
                                editor.stopSelected(-1)
                            editor.write(editor.specWith({ stops: stops }))
                        }
                    }
                }
            }
        }

        Flow {
            Layout.fillWidth: true
            spacing: AppTheme.spacing6
            ChromeButton {
                objectName: "gradientAddMiddleButton"
                text: qsTr("Add a middle colour")
                visible: editor.stopCount < 3
                onClicked: {
                    var stops = editor.specWith({}).stops
                    // Halfway between its neighbours, so adding it changes
                    // nothing until it is edited.
                    stops.splice(1, 0, editor.mixed(stops[0], stops[1], 0.5))
                    if (editor.selectedStop >= 1)
                        editor.stopSelected(editor.selectedStop + 1)
                    editor.write(editor.specWith({ stops: stops }))
                }
            }
            ChromeButton {
                objectName: "gradientReverseButton"
                text: qsTr("Reverse")
                Accessible.name: qsTr("Reverse the gradient")
                onClicked: editor.reverseStops()
            }
        }

        Hint {
            objectName: "gradientStopHint"
            text: editor.selectedStop >= 0
                  ? qsTr("The picker below changes the %1 colour. Click it again "
                         + "to go back to the solid colour.")
                    .arg(editor.stopName(editor.selectedStop))
                  : qsTr("Click a colour above to change it with the picker below.")
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

    // A thin rule between the fill and the picker below it.
    Rectangle {
        Layout.fillWidth: true
        Layout.topMargin: AppTheme.spacing4
        implicitHeight: 1
        color: AppTheme.editorBorder
    }
}
