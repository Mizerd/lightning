import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Label {
    id: origin
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
