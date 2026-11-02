import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Dialog {
    id: root
    objectName: "updateAvailableDialog"
    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    width: Math.min(460, parent ? parent.width - AppTheme.spacing24 * 2 : 460)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    readonly property var um: (typeof app !== "undefined" && app)
                               ? app.updateManager : null
    readonly property bool packageManaged:
        root.um ? (root.um.packageManaged === true) : false
    readonly property bool canInstallAutomatically:
        root.um ? (root.um.canInstallAutomatically === true) : false
    readonly property string installType: root.um ? root.um.installType : ""

    readonly property bool shouldBeOpen:
        root.um !== null && root.um !== undefined
        && root.um.state === UpdateManager.UpdateAvailable
        && root.um.latestVersion !== root.um.dismissedVersion
    onShouldBeOpenChanged: {
        if (!shouldBeOpen && opened)
            close()
    }
    onOpenedChanged: if (!opened) root.managedHelpRevealed = false

    property string managedHelpCommand: ""
    property string managedHelpExplanation: ""
    property bool managedHelpRevealed: false
    property bool managedHelpCopied: false
    Connections {
        target: root.um
        function onManagedUpdateHelpRequested(command, explanation) {
            root.managedHelpCommand = command
            root.managedHelpExplanation = explanation
            root.managedHelpRevealed = true
        }
    }
    TextEdit {
        id: managedCommandClipboard
        visible: false
        width: 0
        height: 0
    }
    Timer {
        id: managedHelpCopiedTimer
        interval: 1500
        onTriggered: root.managedHelpCopied = false
    }

    function formatBytes(n) {
        if (!n || n <= 0) return "0 B"
        if (n < 1024) return n + " B"
        if (n < 1024 * 1024) return Math.round(n / 1024) + " KB"
        return Number(n / (1024 * 1024)).toLocaleString(Qt.locale(AppLocale.name), "f", 1) + " MB"
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
            text: qsTr("Update available")
            color: AppTheme.stormText
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightBold
        }
        Label {
            objectName: "updateDialogVersionLabel"
            Layout.fillWidth: true
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.Wrap
            color: AppTheme.stormTextSecondary
            text: qsTr("Lightning %1 is available (you have %2).")
                  .arg(root.um ? root.um.latestVersion : "")
                  .arg(root.um ? root.um.currentVersion : "")
        }
        Label {
            visible: root.um && root.um.totalBytes > 0
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            text: qsTr("Download size: %1")
                  .arg(root.formatBytes(root.um ? root.um.totalBytes : 0))
        }
        Label {
            Layout.fillWidth: true
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.Wrap
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            text: qsTr("Update method: %1")
                  .arg(root.um ? root.um.installTypeLabel : "")
        }

        Label {
            visible: root.um && root.um.releaseNotes.length > 0
            text: qsTr("What's new")
            color: AppTheme.stormTextSecondary
            font.pixelSize: AppTheme.textMeta
            font.weight: AppTheme.weightStrong
        }
        ScrollView {
            visible: root.um && root.um.releaseNotes.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(160, notesText.implicitHeight + 16)
            clip: true
            TextArea {
                id: notesText
                objectName: "updateReleaseNotesText"
                readOnly: true
                wrapMode: TextArea.Wrap
                textFormat: TextEdit.PlainText
                text: root.um ? root.um.releaseNotes : ""
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textMeta
                selectByMouse: true
                background: Rectangle {
                    color: AppTheme.stormInset
                    radius: AppTheme.radiusMd
                }
                Accessible.name: qsTr("Release notes")
            }
        }
        Label {
            objectName: "updateReleaseNotesUrlLabel"
            visible: root.um && root.um.releaseNotesUrl
                     && root.um.releaseNotesUrl.toString().length > 0
            Layout.fillWidth: true
            wrapMode: Text.WrapAnywhere
            textFormat: Text.PlainText
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            font.family: AppTheme.monoFont
            text: root.um && root.um.releaseNotesUrl
                  ? root.um.releaseNotesUrl.toString() : ""
        }

        Label {
            objectName: "updateDialogManagedMessage"
            visible: root.packageManaged
            Layout.fillWidth: true
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.Wrap
            color: AppTheme.stormTextSecondary
            text: root.installType === "linux-flatpak"
                ? qsTr("Updates for this installation are managed by Flatpak.")
                : root.installType === "linux-snap"
                    ? qsTr("Updates for this installation are managed by Snap.")
                    : root.installType === "linux-rpm-repo" && root.um
                          && root.um.packageManagerName === "zypper"
                        ? qsTr("Updates for this installation are managed by zypper.")
                        : root.installType === "linux-rpm-repo"
                            ? qsTr("Updates for this installation are managed by dnf.")
                            : ""
        }
        ColumnLayout {
            objectName: "updateDialogManagedCommandBlock"
            visible: root.packageManaged && root.managedHelpRevealed
                     && root.managedHelpCommand.length > 0
            Layout.fillWidth: true
            spacing: AppTheme.spacing4
            Label {
                Layout.fillWidth: true
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                wrapMode: Text.Wrap
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: root.managedHelpExplanation
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                Label {
                    objectName: "updateDialogManagedCommandText"
                    Layout.fillWidth: true
                    wrapMode: Text.WrapAnywhere
                    textFormat: Text.PlainText
                    color: AppTheme.stormText
                    font.family: AppTheme.monoFont
                    font.pixelSize: AppTheme.textMeta
                    text: root.managedHelpCommand
                }
                AppButton {
                    storm: true
                    objectName: "updateDialogCopyCommandButton"
                    text: root.managedHelpCopied ? qsTr("Copied") : qsTr("Copy")
                    onClicked: {
                        managedCommandClipboard.text = root.managedHelpCommand
                        managedCommandClipboard.selectAll()
                        managedCommandClipboard.copy()
                        managedCommandClipboard.text = ""
                        root.managedHelpCopied = true
                        managedHelpCopiedTimer.restart()
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Item { Layout.fillWidth: true }
            AppButton {
                storm: true
                objectName: "updateDialogLaterButton"
                text: qsTr("Later")
                onClicked: {
                    if (root.um) root.um.dismissVersion()
                    root.close()
                }
            }
            AppButton {
                storm: true
                objectName: "updateDialogManagedHelpButton"
                visible: root.packageManaged
                text: qsTr("Get update instructions")
                onClicked: if (root.um) root.um.openManagedUpdateHelp()
            }
            AppButton {
                storm: true
                kind: "primary"
                objectName: "updateDialogUpdateNowButton"
                visible: !root.packageManaged && root.canInstallAutomatically
                text: qsTr("Update now")
                onClicked: {
                    // Then go to Settings -> Updates, where progress is shown
                    // and the install buttons live
                    // (installUpdate()/installAndRestart() are only called
                    // there). Closing without navigating left a download
                    // running with nothing on screen.
                    if (root.um) root.um.downloadUpdate()
                    root.close()
                    app.showSettingsSection("updates")
                }
            }
        }
    }
}
