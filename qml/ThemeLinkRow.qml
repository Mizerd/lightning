import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

ColumnLayout {
    id: root

    property string groupLabel: ""
    property string parentLabel: ""
    property bool hasOwn: false
    property bool canUnlink: true
    property bool emphasizeParent: false
    property int alsoChanges: 0
    property int keepOwn: 0
    property bool showingDependents: false

    signal parentRequested()
    signal unlinkRequested()
    signal resetRequested()
    signal applyRequested()
    signal showDependentsToggled()

    objectName: "themeRoleLinks"
    spacing: AppTheme.spacing6

    component LinkButton: AbstractButton {
        id: btn
        property bool primary: false
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        implicitHeight: 28
        implicitWidth: Math.min(btnLabel.implicitWidth + AppTheme.spacing12 * 2,
                                root.width)
        Accessible.role: Accessible.Button
        Accessible.name: text
        // AbstractButton answers Space only; the sibling controls in the
        // editor all answer Return and Enter too.
        Keys.onReturnPressed: (e) => { btn.clicked(); e.accepted = true }
        Keys.onEnterPressed: (e) => { btn.clicked(); e.accepted = true }
        background: Rectangle {
            radius: AppTheme.radiusMd
            color: btn.primary
                   ? (btn.down || btn.hovered ? Qt.lighter(AppTheme.editorAccent, 1.08)
                                              : AppTheme.editorAccent)
                   : btn.down ? AppTheme.editorSelection
                   : btn.hovered ? AppTheme.editorInset : "transparent"
            border.width: btn.primary ? 0 : 1
            border.color: AppTheme.editorBorderStrong
            Rectangle {
                anchors.fill: parent
                anchors.margins: 2
                visible: btn.visualFocus
                radius: AppTheme.radiusSm
                color: "transparent"
                border.width: 2
                border.color: btn.primary ? AppTheme.editorAccentInk
                                          : AppTheme.editorAccent
            }
        }
        contentItem: Label {
            id: btnLabel
            text: btn.text
            textFormat: Text.PlainText
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: btn.primary ? AppTheme.editorAccentInk : AppTheme.editorText
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
            font.weight: AppTheme.weightStrong
        }
    }

    RowLayout {
        Layout.fillWidth: true
        visible: root.parentLabel.length > 0 || root.groupLabel.length > 0
        spacing: AppTheme.spacing8

        Item {
            implicitWidth: 16
            implicitHeight: 16
            Icon {
                anchors.centerIn: parent
                visible: root.parentLabel.length > 0
                name: "link"
                size: 16
                color: AppTheme.editorTextSecondary
            }
            Rectangle {
                anchors.centerIn: parent
                visible: root.parentLabel.length === 0
                width: 8
                height: 8
                radius: 4
                color: AppTheme.editorAccent
            }
        }
        Label {
            objectName: "themeRoleLinkState"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: root.parentLabel.length > 0
                  ? qsTr("Linked to %1. It changes when that does.")
                    .arg(root.parentLabel)
                  : qsTr("Own colour. Part of %1.").arg(root.groupLabel)
            color: AppTheme.editorTextSecondary
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }
    }

    Flow {
        Layout.fillWidth: true
        visible: root.parentLabel.length > 0 || root.groupLabel.length > 0
        spacing: AppTheme.spacing6

        LinkButton {
            objectName: "themeLinkParent"
            visible: root.parentLabel.length > 0
            primary: root.emphasizeParent
            text: qsTr("Edit %1").arg(root.parentLabel)
            Accessible.description:
                qsTr("Open %1, so a change recolours everything linked to it")
                .arg(root.parentLabel)
            onClicked: root.parentRequested()
        }
        LinkButton {
            objectName: "themeLinkUnlink"
            visible: root.parentLabel.length > 0 && root.canUnlink
            text: qsTr("Unlink")
            Accessible.description:
                qsTr("Keep the colour it has now, and stop it following %1")
                .arg(root.parentLabel)
            onClicked: root.unlinkRequested()
        }
        LinkButton {
            objectName: "themeResetToGroup"
            visible: root.parentLabel.length === 0 && root.hasOwn
                     && root.groupLabel.length > 0
            text: qsTr("Reset to group")
            Accessible.description: qsTr("Take the colour of %1 again")
                                    .arg(root.groupLabel)
            onClicked: root.resetRequested()
        }
        LinkButton {
            objectName: "themeApplyToGroup"
            visible: root.parentLabel.length === 0 && root.groupLabel.length > 0
            text: qsTr("Apply to all %1").arg(root.groupLabel)
            Accessible.description:
                qsTr("Give this colour to %1 and everything in it")
                .arg(root.groupLabel)
            onClicked: root.applyRequested()
        }
    }

    RowLayout {
        Layout.fillWidth: true
        visible: root.alsoChanges > 0
        spacing: AppTheme.spacing8
        Label {
            objectName: "themeRoleAlsoChanges"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: qsTr("Also changes %n element(s)",
                       "custom theme, count of linked elements", root.alsoChanges)
            color: AppTheme.editorTextSecondary
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }
        LinkButton {
            objectName: "themeLinkShow"
            implicitHeight: 24
            text: root.showingDependents ? qsTr("Hide") : qsTr("Show")
            Accessible.description: qsTr("List the elements that follow this colour")
            onClicked: root.showDependentsToggled()
        }
    }
    RowLayout {
        Layout.fillWidth: true
        visible: root.keepOwn > 0
        spacing: AppTheme.spacing8
        Label {
            objectName: "themeRoleKeepOwn"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: qsTr("%n keep their own colour",
                       "custom theme, count of elements that do not follow",
                       root.keepOwn)
            color: AppTheme.editorTextSecondary
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }
        LinkButton {
            objectName: "themeLinkInclude"
            implicitHeight: 24
            text: qsTr("Include them")
            Accessible.description:
                qsTr("Give them this colour too, dropping their own")
            onClicked: root.applyRequested()
        }
    }
}
