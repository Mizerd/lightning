import QtQuick
import QtQuick.Controls
import MatrixClient

// A typed percentage that sits beside a settings slider and stays in step with
// it. It shows the slider's value (live, while the slider is being dragged)
// and, when a number is typed, clamps it to the slider's range, hands it to the
// slider and calls the slider's `moved()`, so whatever the slider's onMoved
// writes to the settings store runs for a typed value exactly as for a drag.
//
// `valueScale` is how many displayed percent one slider unit is: 1 for a slider
// that already runs 0..100, 100 for one that runs 0..1.
AppTextField {
    id: box

    // The Slider this box mirrors and drives.
    property Slider slider: null
    property real valueScale: 1

    readonly property real shownValue: slider ? slider.value * valueScale : 0
    // What the box shows when nothing is being typed.
    readonly property string shownText: Math.round(shownValue).toString()

    storm: true
    implicitWidth: 68
    implicitHeight: 30
    leftPadding: AppTheme.spacing8
    rightPadding: AppTheme.spacing8 + suffixLabel.implicitWidth
    horizontalAlignment: TextInput.AlignRight
    font.family: AppTheme.monoFont
    font.pixelSize: AppTheme.textMeta
    inputMethodHints: Qt.ImhDigitsOnly
    validator: IntValidator { bottom: 0; top: 999 }
    selectByMouse: true
    enabled: slider ? slider.enabled : false
    Accessible.name: slider && slider.Accessible.name.length > 0
        ? qsTr("%1, percent").arg(slider.Accessible.name)
        : qsTr("Percent")

    // Follows the slider whenever the field is not being edited.
    text: shownText
    onShownTextChanged: if (!activeFocus) text = shownText

    function commitTyped() {
        if (!slider)
            return
        const typed = parseInt(text, 10)
        if (isNaN(typed)) {
            text = shownText
            return
        }
        const low = slider.from * valueScale
        const high = slider.to * valueScale
        const clamped = Math.max(low, Math.min(high, typed))
        slider.value = clamped / valueScale
        slider.moved()
        // Show what was stored, not what was typed.
        text = shownText
    }
    onEditingFinished: commitTyped()
    onActiveFocusChanged: if (!activeFocus) text = shownText

    Label {
        id: suffixLabel
        text: "%"
        color: AppTheme.stormTextMuted
        font.family: AppTheme.monoFont
        font.pixelSize: AppTheme.textMeta
        anchors.right: parent.right
        anchors.rightMargin: AppTheme.spacing8
        anchors.verticalCenter: parent.verticalCenter
    }
}
