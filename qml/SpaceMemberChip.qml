import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: spaceMemberChip
    required property var modelData
    property string spaceId: ""
    signal profileRequested(var member)
    signal moderationRequested(var member, string action)

    function openActions(x, y) {
        if (!app.roomInfo || app.roomInfo.roomId !== spaceId)
            return
        actions.canKick = app.roomInfo.canModerate(modelData.userId, "kick")
        actions.canBan = app.roomInfo.canModerate(modelData.userId, "ban")
        if (actions.canKick || actions.canBan)
            actions.popup(spaceMemberChip, x, y)
    }

    radius: AppTheme.radiusPill
    color: chipHover.hovered ? AppTheme.hover : AppTheme.surface
    border.color: spaceMemberChip.activeFocus ? AppTheme.focusRing
                                              : AppTheme.border
    border.width: spaceMemberChip.activeFocus ? 2 : 1
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: spaceMemberChip.modelData.displayName
                     || spaceMemberChip.modelData.userId
    Accessible.onPressAction: spaceMemberChip.profileRequested(
                                  spaceMemberChip.modelData)
    Keys.onReturnPressed: spaceMemberChip.profileRequested(
                              spaceMemberChip.modelData)
    Keys.onEnterPressed: spaceMemberChip.profileRequested(
                             spaceMemberChip.modelData)
    Keys.onSpacePressed: spaceMemberChip.profileRequested(
                             spaceMemberChip.modelData)
    Keys.onMenuPressed: spaceMemberChip.openActions(0, height)
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier)) {
            spaceMemberChip.openActions(0, height)
            event.accepted = true
        }
    }
    implicitWidth: Math.min(Math.ceil(chipRow.implicitWidth)
                            + chipRow.anchors.leftMargin
                            + chipRow.anchors.rightMargin, 240)
    implicitHeight: 34
    HoverHandler {
        id: chipHover
        cursorShape: Qt.PointingHandCursor
    }
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: spaceMemberChip.profileRequested(spaceMemberChip.modelData)
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onTapped: (point) => spaceMemberChip.openActions(point.position.x,
                                                         point.position.y)
    }
    AppMenu {
        id: actions
        objectName: "spaceMemberChipMenu"
        property bool canKick: false
        property bool canBan: false
        contextLabel: spaceMemberChip.modelData.displayName
                      || spaceMemberChip.modelData.userId
        AppMenuItem {
            objectName: "spaceMemberChipKick"
            visible: actions.canKick
            enabled: visible
            danger: true
            iconName: "person_remove"
            text: qsTr("Kick from space…")
            onTriggered: spaceMemberChip.moderationRequested(
                             spaceMemberChip.modelData, "kick")
        }
        AppMenuItem {
            objectName: "spaceMemberChipBan"
            visible: actions.canBan
            enabled: visible
            danger: true
            iconName: "block"
            text: qsTr("Ban from space…")
            onTriggered: spaceMemberChip.moderationRequested(
                             spaceMemberChip.modelData, "ban")
        }
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
