import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: root

    property bool suppressed: false

    signal detailsRequested()

    readonly property var um: app.updateManager

    readonly property bool shouldShow:
        um && um.updateAvailableWarning
        && !suppressed
        && app.currentScreen === 1

    // Re-arm for a different version, so dismissing one never silences the
    // next.
    readonly property string _version: um ? um.latestVersion : ""
    on_VersionChanged: suppressed = false

    visible: opacity > 0
    opacity: shouldShow ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 140 } }

    implicitWidth: 320
    implicitHeight: promptColumn.implicitHeight + AppTheme.spacing16 * 2
    height: implicitHeight
    width: implicitWidth

    color: AppTheme.stormPanel
    radius: AppTheme.radiusLg
    border.width: 1
    border.color: AppTheme.stormBorder

    objectName: "updateAvailablePrompt"

    ColumnLayout {
        id: promptColumn
        anchors.fill: parent
        anchors.margins: AppTheme.spacing16
        spacing: AppTheme.spacing8

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Icon {
                Layout.alignment: Qt.AlignVCenter
                name: "download"
                size: 18
                color: AppTheme.warning
            }
            Label {
                Layout.fillWidth: true
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                wrapMode: Text.WordWrap
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightBold
                text: qsTr("Lightning %1 is available")
                      .arg(root.um ? root.um.latestVersion : "")
            }
        }

        Label {
            Layout.fillWidth: true
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextSecondary
            font.pixelSize: AppTheme.textMeta
            text: qsTr("You have %1.").arg(root.um ? root.um.currentVersion : "")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Item { Layout.fillWidth: true }
            AppButton {
                storm: true
                objectName: "updatePromptDismissButton"
                text: qsTr("Not now")
                onClicked: {
                    root.suppressed = true;
                    if (root.um)
                        root.um.dismissVersion();
                }
            }
            AppButton {
                storm: true
                kind: "primary"
                objectName: "updatePromptOpenButton"
                text: qsTr("Update")
                onClicked: {
                    root.suppressed = true;
                    root.detailsRequested();
                }
            }
        }
    }
}
