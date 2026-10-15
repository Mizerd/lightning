import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Why a video surface is empty. With Qt Quick's software renderer there is no
// video node, so a VideoOutput draws nothing while its player believes it is
// playing: the same reason the call stage gives in its own strip
// (CallStage `callSoftwareRendererNotice`). Place it over the VideoOutput;
// `shown` is the host's own "a picture should be here now" condition. Nothing
// at all on a machine with a usable GPU. Below kMinTextHeight only the icon
// is drawn, with the sentence as its accessible name and tooltip.
Item {
    id: root

    property bool shown: true
    readonly property bool active: root.shown && typeof app !== "undefined"
                                   && !!app && app.softwareRenderer === true
    readonly property int kMinTextHeight: 64
    readonly property string message:
        qsTr("Video can't be shown on this computer: there is no working graphics acceleration. Sound still plays.")

    visible: root.active
    Accessible.role: Accessible.StaticText
    Accessible.name: root.message

    Rectangle {
        id: plate
        objectName: "softwareVideoNoticePlate"
        anchors.centerIn: parent
        readonly property bool roomy: root.height >= root.kMinTextHeight
                                      && root.width >= 160
        width: roomy ? Math.min(root.width - AppTheme.spacing8 * 2, 360)
                     : noticeIcon.width + AppTheme.spacing8 * 2
        height: roomy ? noticeRow.implicitHeight + AppTheme.spacing8 * 2
                      : noticeIcon.height + AppTheme.spacing8 * 2
        color: AppTheme.warningFill
        radius: AppTheme.radiusSm

        RowLayout {
            id: noticeRow
            anchors.fill: parent
            anchors.margins: AppTheme.spacing8
            spacing: AppTheme.spacing8

            Icon {
                id: noticeIcon
                name: "warning"
                size: 16
                color: AppTheme.warning
                Layout.alignment: Qt.AlignVCenter
            }
            Text {
                objectName: "softwareVideoNoticeText"
                visible: plate.roomy
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textMeta
                textFormat: Text.PlainText
                text: root.message
            }
        }

        HoverHandler { id: hover }
        ToolTip.visible: !plate.roomy && hover.hovered
        ToolTip.text: root.message
    }
}
