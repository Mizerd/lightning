import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The ONE update-available dialog (UPDATE-SPEC.md v1). Opens itself off
// UpdateManager's own state — the ReportMessageDialog / VerificationDialog /
// UiaPromptDialog idiom (see Main.qml): every place that can discover an
// update (an automatic check, a manual check) surfaces through the same one
// presentation, never a per-caller popup. It intentionally does very little:
// "Update now" only STARTS the download and gets out of the way — the
// persistent Settings -> Updates card (UpdatesSettingsSection.qml) is the
// one place that shows Downloading/Verifying/ReadyToInstall/Installing/
// RestartRequired/Failed, so progress is never duplicated or allowed to
// drift between two surfaces.
//
// Release notes are UNTRUSTED remote text (fetched from the update
// manifest's `release_notes` field, over a validated HTTPS channel, but
// still content the release host controls). They are rendered in a
// read-only TextArea with textFormat FORCED to TextEdit.PlainText — never
// AutoText, StyledText, RichText, or MarkdownText — so nothing the release
// server sends can ever be interpreted as HTML/markup or invoke an
// application command. UpdateManager also exposes releaseNotesUrl (the
// manifest's `release_notes_url`); it is shown as plain, NON-interactive,
// explicitly-constructed text underneath the notes — inert, never clickable
// and never wired to any link-activation handler — so nothing the manifest
// supplies can trigger navigation or an application command by itself.
Dialog {
    id: root
    objectName: "updateAvailableDialog"
    modal: true
    // The shared navy modal scrim (QuickSwitcher convention) —
    // never the Basic style default dim (2026-08-19 audit).
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

    // v0.7.3: this dialog no longer opens ITSELF. The automatic announcement
    // is the corner card (UpdateAvailablePrompt), which does not block the
    // application for something that is never urgent; this dialog is what the
    // card's "Update" opens, and it remains the ONE place release notes and
    // "Update now" live. Two self-opening surfaces for one event was exactly
    // the duplication this file's header warns about.
    //
    // It still closes ITSELF, because the reasons to close are the manager's
    // own: the download started, or the version was dismissed. Leaving a
    // stale dialog open over a state it no longer describes would be worse
    // than never having opened it.
    readonly property bool shouldBeOpen:
        root.um !== null && root.um !== undefined
        && root.um.state === UpdateManager.UpdateAvailable
        && root.um.latestVersion !== root.um.dismissedVersion
    onShouldBeOpenChanged: {
        if (!shouldBeOpen && opened)
            close()
    }
    onOpenedChanged: if (!opened) root.managedHelpRevealed = false

    // Set from UpdateManager::managedUpdateHelpRequested(command, explanation)
    // — the ONLY source of this text; nothing here constructs a command
    // itself. Revealed inline rather than closing the dialog, so the user
    // can actually read/copy it before "Later" dismisses this version.
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
        return (n / (1024 * 1024)).toFixed(1) + " MB"
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
            font.pixelSize: 16
            font.weight: Font.Bold
        }
        Label {
            objectName: "updateDialogVersionLabel"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: AppTheme.stormTextSecondary
            text: qsTr("Lightning %1 is available (you have %2).")
                  .arg(root.um ? root.um.latestVersion : "")
                  .arg(root.um ? root.um.currentVersion : "")
        }
        Label {
            visible: root.um && root.um.totalBytes > 0
            color: AppTheme.stormTextMuted
            font.pixelSize: 12
            text: qsTr("Download size: %1")
                  .arg(root.formatBytes(root.um ? root.um.totalBytes : 0))
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: AppTheme.stormTextMuted
            font.pixelSize: 12
            text: qsTr("Update method: %1")
                  .arg(root.um ? root.um.installTypeLabel : "")
        }

        Label {
            visible: root.um && root.um.releaseNotes.length > 0
            text: qsTr("What's new")
            color: AppTheme.stormTextSecondary
            font.pixelSize: 12
            font.weight: Font.DemiBold
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
                // Forced plain text — see the file header. Never
                // AutoText/StyledText/RichText/MarkdownText on this
                // untrusted remote string.
                textFormat: TextEdit.PlainText
                text: root.um ? root.um.releaseNotes : ""
                color: AppTheme.stormTextSecondary
                font.pixelSize: 12
                selectByMouse: true
                background: Rectangle {
                    color: AppTheme.stormInset
                    radius: AppTheme.radiusMd
                }
                Accessible.name: qsTr("Release notes")
            }
        }
        // The manifest's release_notes_url, shown as a separate,
        // explicitly-constructed, NON-interactive line of plain text —
        // never a clickable link (see the file header). Selectable so a
        // user who wants to open it can copy/paste it themselves.
        Label {
            objectName: "updateReleaseNotesUrlLabel"
            visible: root.um && root.um.releaseNotesUrl
                     && root.um.releaseNotesUrl.toString().length > 0
            Layout.fillWidth: true
            wrapMode: Text.WrapAnywhere
            textFormat: Text.PlainText
            color: AppTheme.stormTextMuted
            font.pixelSize: 11
            font.family: AppTheme.monoFont
            text: root.um && root.um.releaseNotesUrl
                  ? root.um.releaseNotesUrl.toString() : ""
        }

        // Package-managed installs (Flatpak/Snap): no self-download here
        // either — same policy as Settings -> Updates.
        Label {
            objectName: "updateDialogManagedMessage"
            visible: root.packageManaged
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: AppTheme.stormTextSecondary
            text: root.installType === "linux-flatpak"
                ? qsTr("Updates for this installation are managed by Flatpak.")
                : root.installType === "linux-snap"
                    ? qsTr("Updates for this installation are managed by Snap.")
                    : ""
        }
        // Revealed after "Get update instructions" — the exact command and
        // explanation UpdateManager itself emitted, nothing constructed
        // here.
        ColumnLayout {
            objectName: "updateDialogManagedCommandBlock"
            visible: root.packageManaged && root.managedHelpRevealed
                     && root.managedHelpCommand.length > 0
            Layout.fillWidth: true
            spacing: AppTheme.spacing4
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: AppTheme.stormTextMuted
                font.pixelSize: 11
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
                    font.pixelSize: 12
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
                // Deliberately does NOT close the dialog: the command is
                // revealed inline above so there is something to read/copy
                // before the user dismisses with Later.
                onClicked: if (root.um) root.um.openManagedUpdateHelp()
            }
            AppButton {
                storm: true
                kind: "primary"
                objectName: "updateDialogUpdateNowButton"
                visible: !root.packageManaged && root.canInstallAutomatically
                text: qsTr("Update now")
                onClicked: {
                    if (root.um) root.um.downloadUpdate()
                    root.close()
                }
            }
        }
    }
}
