import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import MatrixClient

// Covers spoiler media. The picture behind it is decoded at a few pixels
// only, so no setting of the blur can show more than a smear.
Item {
    id: root
    objectName: "spoilerCover"

    property bool covered: false
    property string source: ""
    property string reason: ""
    property bool focusable: true
    property bool interactive: true

    signal revealRequested()

    visible: covered
    enabled: covered

    readonly property int strength: typeof app !== "undefined" && app && app.settings
                                    ? app.settings.spoilerBlurStrength : 70
    readonly property int decodeEdge: Math.round(32 - 24 * strength / 100)

    Rectangle {
        anchors.fill: parent
        radius: AppTheme.radiusSm
        color: AppTheme.embedSurface
        border.color: AppTheme.border
        border.width: 1
    }

    Image {
        id: smear
        objectName: "spoilerSmear"
        anchors.fill: parent
        visible: false
        source: root.covered ? root.source : ""
        sourceSize.width: root.decodeEdge
        sourceSize.height: root.decodeEdge
        fillMode: Image.PreserveAspectCrop
        smooth: true
        asynchronous: true
        cache: false
    }
    MultiEffect {
        objectName: "spoilerBlur"
        anchors.fill: parent
        visible: root.covered && smear.status === Image.Ready
        source: smear
        blurEnabled: true
        blur: 1.0
        blurMax: 32 + Math.round(root.strength * 0.32)
        autoPaddingEnabled: false
        brightness: -0.12
    }

    Column {
        anchors.centerIn: parent
        width: Math.min(parent.width - 16, implicitWidth)
        spacing: AppTheme.spacing4

        Rectangle {
            objectName: "spoilerChip"
            anchors.horizontalCenter: parent.horizontalCenter
            radius: height / 2
            color: AppTheme.overlayScrim
            width: chipRow.implicitWidth + 16
            height: chipRow.implicitHeight + 8
            Row {
                id: chipRow
                anchors.centerIn: parent
                spacing: AppTheme.spacing6
                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: "visibility_off"
                    size: 15
                    color: AppTheme.scrimInk
                }
                Label {
                    objectName: "spoilerLabel"
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Spoiler")
                    color: AppTheme.scrimInk
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    font.weight: AppTheme.weightStrong
                }
            }
        }
        Rectangle {
            objectName: "spoilerReason"
            visible: root.reason.length > 0
            anchors.horizontalCenter: parent.horizontalCenter
            radius: 3
            color: AppTheme.overlayScrim
            width: Math.min(reasonLabel.implicitWidth + 12, root.width - 16)
            height: reasonLabel.implicitHeight + 4
            Label {
                id: reasonLabel
                anchors.centerIn: parent
                width: Math.min(implicitWidth, parent.width - 12)
                text: root.reason
                textFormat: Text.PlainText
                elide: Label.ElideRight
                color: AppTheme.scrimInk
                font.pixelSize: AppTheme.fontMicro
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: root.interactive
        cursorShape: Qt.PointingHandCursor
        onClicked: root.revealRequested()
    }

    activeFocusOnTab: root.covered && root.focusable
    Accessible.role: Accessible.Button
    Accessible.name: qsTr("Spoiler, press to reveal")
    Accessible.description: root.reason
    Accessible.onPressAction: root.revealRequested()
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
            || event.key === Qt.Key_Enter) {
            root.revealRequested()
            event.accepted = true
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: root.activeFocus
        radius: AppTheme.radiusSm
        color: "transparent"
        border.width: 2
        border.color: AppTheme.focusRing
    }
}
