import QtQuick
import QtQuick.Controls
import MatrixClient

AppTextField {
    id: box

    property Slider slider: null
    property real valueScale: 1
    // What the slider's own `value: <setting>` binding reads, in the slider's
    // units. Typing a number assigns `slider.value`, and an assignment to a
    // bound property destroys its binding for good: from then on the slider
    // would not follow the setting (a reset, a synced import, another control
    // writing it). A Binding is not on the property, so a later assignment
    // cannot remove it. Leave unset for a slider that is not bound to a
    // setting.
    property var follows: undefined
    Binding {
        target: box.slider
        property: "value"
        value: box.follows
        when: box.slider !== null && box.follows !== undefined
        restoreMode: Binding.RestoreNone
    }

    readonly property real shownValue: slider ? slider.value * valueScale : 0
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

    property bool userEdited: false
    onTextEdited: userEdited = true

    text: shownText
    onShownTextChanged: if (!activeFocus || !userEdited) text = shownText

    function revertTyped() {
        userEdited = false
        text = shownText
    }

    function commitTyped() {
        if (!slider)
            return
        if (!userEdited) {
            text = shownText
            return
        }
        userEdited = false
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
