import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: root

    property bool blocked: false

    readonly property bool shouldShow:
        app.messageSearch.indexAllOffered
        && !blocked
        && !app.verificationActive
        && app.verificationState === ""
        && app.currentScreen === 1

    objectName: "indexAllPrompt"
    visible: opacity > 0
    opacity: shouldShow ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 140 } }

    width: 316
    implicitHeight: promptColumn.implicitHeight + AppTheme.spacing16 * 2
    height: implicitHeight
    radius: AppTheme.radiusLg
    color: AppTheme.stormPanel
    border.color: AppTheme.stormBorder
    border.width: 1

    Accessible.role: Accessible.AlertMessage
    Accessible.name: qsTr("Index encrypted messages now?")

    ColumnLayout {
        id: promptColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: AppTheme.spacing16
        anchors.rightMargin: AppTheme.spacing16
        spacing: AppTheme.spacing8

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Icon {
                name: "search"
                size: 18
                color: AppTheme.stormTextMuted
                Layout.alignment: Qt.AlignVCenter
            }
            Label {
                objectName: "indexAllPromptTitle"
                Layout.fillWidth: true
                text: qsTr("Index encrypted messages now?")
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightBold
                elide: Label.ElideRight
            }
        }

        Label {
            objectName: "indexAllPromptBody"
            Layout.fillWidth: true
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            text: qsTr("So search can find older messages in encrypted "
                       + "rooms later. It can take a while, uses "
                       + "bandwidth, and stores the decrypted text on this "
                       + "device. You can also do it later in Settings.")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            AppButton {
                objectName: "indexAllPromptYes"
                storm: true
                kind: "primary"
                Layout.fillWidth: true
                text: qsTr("Index now")
                onClicked: app.messageSearch.answerIndexAllOffer("yes")
            }
            AppButton {
                objectName: "indexAllPromptNo"
                storm: true
                Layout.fillWidth: true
                text: qsTr("Not now")
                onClicked: app.messageSearch.answerIndexAllOffer("no")
            }
        }
    }
}
