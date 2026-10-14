import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// How the colour open in the theme editor is tied to the rest of the theme, as
// something to act on rather than a sentence to read:
//
//   linked   "Linked to Panels and cards"   [Edit Panels and cards] [Unlink]
//   own      "Own colour, part of Panels"   [Reset to group] [Apply to all ...]
//
// and, for a group, "Also changes N" with a Show toggle and "N keep their own
// colour" with Include them. It holds no theme state: the editor sets the
// properties and answers the signals, so the same row serves every role.
//
// Painted in the invariant editor tokens (see `editorCanvas` in AppTheme.qml).
ColumnLayout {
    id: root

    // The group this role belongs to ("" when it is a top-level colour).
    property string groupLabel: ""
    // The role it is riding on right now ("" when it has a colour of its own
    // or follows nothing).
    property string parentLabel: ""
    // True while the role has a colour or gradient of its own to drop.
    property bool hasOwn: false
    // Unlink copies the resolved colour; refused for a see-through one, whose
    // alpha a stored colour cannot carry.
    property bool canUnlink: true
    // At the Simple and Groups levels the way to a wider change is the more
    // prominent button.
    property bool emphasizeParent: false
    // Roles that change with this one, and how many of its group keep a
    // colour of their own and so would not.
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

    // A small self-contained button.
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

    // The state, in one line.
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
            // The mark an own colour carries in the list, so the two read as
            // the same fact.
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

    // A group: what it carries with it.
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
