import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// "Choose apps…" from the call bar's share options: which applications the
// screen share's sound comes from. Opened from CallShareOptionsMenu, the one
// share surface every route shows (on Wayland the desktop's own picker
// replaces Lightning's), so choosing applications never depends on which
// picker a desktop draws. Changes apply to a running share at once.
AppDialog {
    id: root

    objectName: "shareAudioAppsDialog"
    title: qsTr("Share sound from apps")
    parent: Overlay.overlay
    anchors.centerIn: parent
    standardButtons: Dialog.NoButton
    width: Math.min(440, parent ? parent.width - 64 : 440)

    ColumnLayout {
        anchors.fill: parent
        spacing: AppTheme.spacing12

        Label {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            wrapMode: Text.WordWrap
            text: qsTr("Only the apps you tick are heard in your screen "
                       + "share. Lightning itself is never included, so "
                       + "nobody hears the call back.")
            color: AppTheme.stormTextSecondary
            font.pixelSize: AppTheme.textMeta
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(appList.implicitHeight, 280)
            clip: true
            contentWidth: availableWidth
            ShareAudioAppList {
                id: appList
                width: parent ? parent.width : implicitWidth
                visible: root.visible
            }
        }

        Label {
            objectName: "shareAudioAppsStatus"
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            wrapMode: Text.WordWrap
            // Application names reach this sentence: never markup.
            textFormat: Text.PlainText
            text: app.groupCall ? app.groupCall.shareAudioStatus : ""
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
        }

        RowLayout {
            Layout.fillWidth: true
            AppButton {
                objectName: "shareAudioAppsWholeSystem"
                storm: root.storm
                text: qsTr("Use the entire system")
                visible: !!app.groupCall && app.groupCall.shareAudioSupported
                onClicked: {
                    if (app.groupCall)
                        app.groupCall.shareAudioMode = 1
                    root.close()
                }
            }
            Item { Layout.fillWidth: true }
            AppButton {
                objectName: "shareAudioAppsDone"
                storm: root.storm
                kind: "primary"
                text: qsTr("Done")
                onClicked: root.close()
            }
        }
    }
}
