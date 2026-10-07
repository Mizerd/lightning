import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts
import MatrixClient

// The applications a screen share's sound can come from, live: one tickable
// row per application that is playing (or was chosen and is not playing now),
// updated as applications start and stop. Ticking one switches the share to
// "only the chosen apps"; Lightning itself is never listed. Used by the
// share picker and by ShareAudioAppsDialog (the call bar's route, which is
// the only one on Wayland).
ColumnLayout {
    id: root

    spacing: AppTheme.spacing4

    readonly property bool canChoose:
        !!app.groupCall && app.groupCall.shareAudioCanChooseApps
    readonly property var rows:
        app.groupCall ? app.groupCall.shareAudioApplications : []

    // Live only while shown: the controller polls while anything watches.
    // Counted, so the false must always follow the true.
    property bool watching: false
    function syncWatch() {
        var want = root.visible && root.canChoose && !!app.groupCall
        if (want === root.watching)
            return
        root.watching = want
        app.groupCall.watchShareAudioApplications(want)
    }
    onVisibleChanged: syncWatch()
    Component.onCompleted: syncWatch()
    Component.onDestruction: {
        if (root.watching && app.groupCall)
            app.groupCall.watchShareAudioApplications(false)
        root.watching = false
    }

    Label {
        objectName: "shareAudioAppsUnavailable"
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        visible: !root.canChoose
        wrapMode: Text.WordWrap
        text: qsTr("Single apps can't be chosen on this system. It needs "
                   + "PipeWire on Linux, or Windows 10 version 2004 or newer.")
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
    }

    Label {
        objectName: "shareAudioAppsEmpty"
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        visible: root.canChoose && root.rows.length === 0
        wrapMode: Text.WordWrap
        text: qsTr("No app is playing sound right now. Start one and it "
                   + "appears here.")
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
    }

    Repeater {
        model: root.canChoose ? root.rows : []
        delegate: RowLayout {
            id: rowItem
            required property var modelData
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            CheckBox {
                id: tick
                objectName: "shareAudioAppCheck"
                property string appKey: rowItem.modelData.key
                // Lightning runs inside it (Windows: explorer.exe, a launcher):
                // its process tree includes this call, so it is never captured.
                enabled: rowItem.modelData.containsUs !== true
                checked: rowItem.modelData.chosen === true
                Accessible.name: rowItem.modelData.label
                onToggled: {
                    if (app.groupCall) {
                        app.groupCall.setShareAudioAppChosen(
                            rowItem.modelData.key, rowItem.modelData.label,
                            checked)
                    }
                    // A click writes `checked` and destroys the binding; the
                    // controller's answer is the truth (it may refuse).
                    checked = Qt.binding(function () {
                        return rowItem.modelData.chosen === true
                    })
                }
            }

            // The application's own icon where the audio server names one
            // (an XDG icon name), else its initial.
            Item {
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
                IconImage {
                    id: appIcon
                    anchors.fill: parent
                    visible: status === Image.Ready
                    name: rowItem.modelData.iconName || ""
                    sourceSize.width: 20
                    sourceSize.height: 20
                }
                Rectangle {
                    anchors.fill: parent
                    visible: !appIcon.visible
                    radius: width / 2
                    color: AppTheme.stormInset
                    Label {
                        anchors.centerIn: parent
                        text: (rowItem.modelData.label || "?")
                              .charAt(0).toUpperCase()
                        textFormat: Text.PlainText
                        color: AppTheme.stormTextSecondary
                        font.pixelSize: AppTheme.textMeta
                        font.weight: AppTheme.weightStrong
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                // Application names are remote-ish text: never markup.
                textFormat: Text.PlainText
                text: rowItem.modelData.label
                elide: Text.ElideRight
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textBody
                MouseArea {
                    anchors.fill: parent
                    // The name is part of the target, as a CheckBox's own
                    // text would be.
                    onClicked: {
                        if (!tick.enabled)
                            return
                        tick.toggle()
                        tick.toggled()
                    }
                }
            }

            Label {
                objectName: "shareAudioAppState"
                // Whether the running share is actually carrying it, so a
                // ticked app that is not heard says why.
                textFormat: Text.PlainText
                text: rowItem.modelData.containsUs === true
                      ? qsTr("Includes Lightning")
                      : rowItem.modelData.failed === true
                        ? qsTr("Couldn't capture")
                        : rowItem.modelData.carried === true
                          ? qsTr("In the share")
                          : rowItem.modelData.playing === true
                            ? qsTr("Playing")
                            : qsTr("Not playing")
                color: rowItem.modelData.failed === true
                       ? AppTheme.stormDanger
                       : rowItem.modelData.carried === true
                         ? AppTheme.stormSuccess : AppTheme.stormTextMuted
                ToolTip.visible: rowItem.modelData.containsUs === true
                                 && stateHover.hovered
                ToolTip.text: qsTr("Lightning runs inside this app, so "
                                   + "capturing it would send the call back "
                                   + "to everyone.")
                HoverHandler { id: stateHover }
                font.pixelSize: AppTheme.textMeta
            }
        }
    }
}
