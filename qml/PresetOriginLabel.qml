import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Under a layout control in Settings: says when the value shown comes from
// the selected layout preset rather than from the user's own choice, so the
// control and the actual layout never disagree in silence.
Label {
    id: origin
    // SettingsManager property the control edits (messageLayout,
    // messageSpacing, threadPlacement).
    property string key: ""
    // presetSuppliesValue is a Q_INVOKABLE with no NOTIFY, so the binding
    // reads every signal that can move its answer.
    visible: {
        var _deps = [app.settings.layoutPreset,
                     app.settings.layoutPresetCustomized,
                     app.settings.messageLayout,
                     app.settings.messageSpacing,
                     app.settings.threadPlacement]
        return app.settings.layoutPreset === 1
               && app.settings.presetSuppliesValue(origin.key)
    }
    Layout.fillWidth: true
    Layout.leftMargin: AppTheme.spacing4
    wrapMode: Text.WordWrap
    color: AppTheme.stormTextMuted
    font.pixelSize: AppTheme.settingsHintSize
    text: qsTr("Set by the Nheko compact preset. Changing it customizes the "
               + "preset only.")
}
