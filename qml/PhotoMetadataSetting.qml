import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings -> Privacy: what is removed from a photo before it is uploaded.
//
// Stored by SettingsManager::imageMetadataMode as "location" (default), "all"
// or "keep" and pushed into the composer by Main.qml. Applies to every image
// that leaves the attachment tray (picker, paste, drag and drop), in encrypted
// rooms too; GIFs from the GIF picker are provider media and are not touched.
ColumnLayout {
    id: section
    objectName: "photoMetadataSetting"
    spacing: AppTheme.spacing4

    readonly property bool available: typeof app !== "undefined" && app
                                      && !!app.settings

    AppComboBox {
        id: combo
        objectName: "imageMetadataCombo"
        storm: true
        Layout.fillWidth: true
        textRole: "label"
        valueRole: "value"
        Accessible.name: qsTr("Photo details removed when sending")
        model: [
            { label: qsTr("Remove location (recommended)"), value: "location" },
            { label: qsTr("Remove all details"),            value: "all" },
            { label: qsTr("Keep everything"),               value: "keep" },
        ]
        function syncFromSettings() {
            if (section.available)
                syncToValue(app.settings.imageMetadataMode)
        }
        Component.onCompleted: syncFromSettings()
        Connections {
            target: section.available ? app.settings : null
            function onImageMetadataModeChanged() { combo.syncFromSettings() }
        }
        onActivated: if (section.available)
                         app.settings.imageMetadataMode = currentValue
    }
    Label {
        objectName: "imageMetadataHint"
        Layout.fillWidth: true
        Layout.leftMargin: AppTheme.spacing4
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        lineHeight: AppTheme.lineHeightBody
        lineHeightMode: Text.ProportionalHeight
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        text: {
            var mode = section.available ? app.settings.imageMetadataMode
                                         : "location"
            if (mode === "all")
                return qsTr("Removes location, camera, time and every other "
                            + "detail stored inside JPEG, PNG, WebP and HEIC "
                            + "photos. The picture itself is not re-encoded "
                            + "and keeps its rotation.")
            if (mode === "keep")
                return qsTr("Photos are sent exactly as they are, including "
                            + "where and when they were taken.")
            return qsTr("Removes where a JPEG, PNG, WebP or HEIC photo was "
                        + "taken before it is sent: GPS position and any "
                        + "named place (city, country, location) in its "
                        + "details. Camera and time details stay. The "
                        + "picture itself is not re-encoded. TIFF, raw and "
                        + "JPEG XL files cannot be cleaned and are not "
                        + "sent. Your own file is never changed.")
        }
    }
}
