import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.9 room upgrade (phase 8): the confirmation flow. Opened from the room's
// Access block and from Space settings; drives app.roomUpgrade, whose
// room is the one currently inspected (app.roomInfo.roomId). The version
// list is the homeserver's own (requestRoomVersions on open), the default
// is the recommendation, and the explanation is deliberately blunt: an
// upgrade is irreversible and tombstones the old room.
Dialog {
    id: root
    objectName: "roomUpgradeDialog"
    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    width: Math.min(480, parent ? parent.width - AppTheme.spacing24 * 2 : 480)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    // "room" or "space": copy only.
    property string kind: "room"
    readonly property bool isSpace: kind === "space"
    property string chosenVersion: ""

    function openFor() {
        chosenVersion = ""
        addToSpaces.checked = !root.isSpace
        app.roomUpgrade.requestRoomVersions()
        open()
    }
    onOpened: Qt.callLater(function () { versionCombo.forceActiveFocus() })

    // Navigate-on-success closes the dialog: the controller emits
    // navigateRequested, and the room changes under us.
    Connections {
        target: app.roomUpgrade
        function onUpgradeStateChanged() {
            if (root.opened && !app.roomUpgrade.upgradeBusy
                    && app.roomUpgrade.lastReplacementRoomId.length > 0
                    && app.roomUpgrade.upgradeError.length === 0)
                root.close()
        }
        function onVersionsChanged() {
            if (root.chosenVersion === "")
                root.chosenVersion = app.roomUpgrade.defaultVersion
            versionCombo.refresh()
        }
    }

    background: Rectangle {
        radius: AppTheme.radiusLg
        color: AppTheme.stormPanel
        border.color: AppTheme.stormBorder
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        Label {
            text: root.isSpace ? qsTr("Upgrade this space")
                               : qsTr("Upgrade this room")
            color: AppTheme.stormText
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightBold
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: AppTheme.stormTextSecondary
            font.pixelSize: AppTheme.textBody
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            text: root.isSpace
                  ? qsTr("Upgrading creates a NEW space on a newer room "
                         + "version and marks this one as replaced. Members "
                         + "have to join the new space; the old one stays "
                         + "readable but closed. The server carries the "
                         + "settings, permissions and addresses across. "
                         + "This cannot be undone.")
                  : qsTr("Upgrading creates a NEW room on a newer room "
                         + "version and marks this one as replaced. Members "
                         + "have to join the new room; the old one stays "
                         + "readable but closed. The server carries the "
                         + "settings, permissions and addresses across. "
                         + "This cannot be undone.")
        }
        Label {
            text: qsTr("Current version: %1").arg(app.roomInfo.roomVersion)
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
        }
        Label {
            text: qsTr("New version")
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
        }
        AppComboBox {
            id: versionCombo
            objectName: "roomUpgradeVersionCombo"
            Layout.fillWidth: true
            enabled: app.roomUpgrade.versionsKnown && !app.roomUpgrade.upgradeBusy
            readonly property var rows: app.roomUpgrade.availableVersions
            model: {
                var labels = []
                for (var i = 0; i < rows.length; ++i) {
                    var r = rows[i]
                    var label = r.version
                    if (r.version === app.roomUpgrade.defaultVersion)
                        label += " · " + qsTr("recommended")
                    else if (!r.stable)
                        label += " · " + qsTr("unstable")
                    labels.push(label)
                }
                return labels
            }
            function refresh() {
                for (var i = 0; i < rows.length; ++i) {
                    if (rows[i].version === root.chosenVersion) {
                        currentIndex = i
                        return
                    }
                }
                currentIndex = rows.length > 0 ? 0 : -1
            }
            onActivated: (index) => {
                if (index >= 0 && index < rows.length)
                    root.chosenVersion = rows[index].version
            }
        }
        Label {
            Layout.fillWidth: true
            visible: !app.roomUpgrade.versionsKnown
            wrapMode: Text.Wrap
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            text: qsTr("Asking the server which room versions it supports…")
        }
        RowLayout {
            Layout.fillWidth: true
            visible: !root.isSpace
            spacing: AppTheme.spacing8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textBody
                text: qsTr("Also add the new room to the spaces this one is in")
            }
            AppSwitch {
                id: addToSpaces
                objectName: "roomUpgradeAddToSpaces"
                checked: true
                onToggled: checked = !checked
            }
        }
        Label {
            Layout.fillWidth: true
            visible: app.roomUpgrade.upgradeError.length > 0
            wrapMode: Text.Wrap
            color: AppTheme.danger
            font.pixelSize: AppTheme.textMeta
            text: app.roomUpgrade.upgradeError
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Cancel")
                kind: "ghost"
                enabled: !app.roomUpgrade.upgradeBusy
                onClicked: root.close()
            }
            AppButton {
                objectName: "roomUpgradeConfirm"
                text: app.roomUpgrade.upgradeBusy ? qsTr("Upgrading…")
                                                  : qsTr("Upgrade")
                kind: "dangerPrimary"
                enabled: app.roomUpgrade.versionsKnown
                         && !app.roomUpgrade.upgradeBusy
                         && root.chosenVersion.length > 0
                onClicked: app.roomUpgrade.upgradeRoom(root.chosenVersion,
                                                       addToSpaces.checked)
            }
        }
    }
}
