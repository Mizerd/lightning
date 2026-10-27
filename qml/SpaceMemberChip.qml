import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// One member of a Space on Space Home: avatar and name in a pill, sized to
// its content up to 240 px. A tap asks for the member's profile card.
Rectangle {
    id: spaceMemberChip
    required property var modelData
    // Empty for isolated previews; real Space Home supplies its room id.
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
    // A chip opens a profile card, so it is a Tab stop with a ring and a
    // name a screen reader can speak.
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
