import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// One member of a Space on Space Home: avatar and name in a pill, sized to
// its content up to 240 px. A tap asks for the member's profile card.
Rectangle {
    id: spaceMemberChip
    required property var modelData
    signal profileRequested(var member)

    radius: AppTheme.radiusPill
    color: chipHover.hovered ? AppTheme.hover : AppTheme.surface
    border.color: AppTheme.border
    border.width: 1
    // The row's own side margins, so the name gets all of its width; a
    // fractional text width is rounded up, never down into an elision.
    implicitWidth: Math.min(Math.ceil(chipRow.implicitWidth)
                            + chipRow.anchors.leftMargin
                            + chipRow.anchors.rightMargin, 240)
    implicitHeight: 34
    HoverHandler {
        id: chipHover
        cursorShape: Qt.PointingHandCursor
    }
    TapHandler {
        onTapped: spaceMemberChip.profileRequested(spaceMemberChip.modelData)
    }
    RowLayout {
        id: chipRow
        anchors.fill: parent
        anchors.leftMargin: AppTheme.spacing4
        anchors.rightMargin: AppTheme.spacing10
        spacing: AppTheme.spacing6
        Avatar {
            size: 26
            name: spaceMemberChip.modelData.displayName
                  || spaceMemberChip.modelData.userId
            mxc: spaceMemberChip.modelData.avatarUrl || ""
            colorKey: spaceMemberChip.modelData.userId
            circle: true
        }
        Label {
            objectName: "spaceMemberChipName"
            Layout.fillWidth: true
            text: spaceMemberChip.modelData.displayName
                  || spaceMemberChip.modelData.userId
            textFormat: Text.PlainText
            color: AppTheme.textPrimary
            font.pixelSize: AppTheme.textMeta
            elide: Label.ElideRight
        }
    }
}
