import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings -> Updates (UPDATE-SPEC.md v1), bound to UpdateManager through
// app.updateManager. UpdateManager is account-agnostic. `state` is its real
// Q_ENUM (compare against UpdateManager.Idle etc., never strings);
// lastCheckTime is a QDateTime (a JS Date). A ColumnLayout toggled by
// visibility, never a Loader, so local state such as failureDismissed survives
// switching categories. Trust-chain rules:
//   - a hash/signature failure is terminal: only Retry and dismissing the
//     banner text are offered;
//   - Flatpak/Snap installs (packageManaged) never get download/install, only a
//     disclosure and help;
//   - development/unknown installs never get install either;
//   - restarting is always one explicit click, never automatic.
ColumnLayout {
    id: root
    objectName: "updatesSettingsSection"

    Layout.fillWidth: true
    spacing: AppTheme.spacing12

    readonly property var um: (typeof app !== "undefined" && app)
                               ? app.updateManager : null
    readonly property bool packageManaged:
        root.um ? (root.um.packageManaged === true) : false
    readonly property bool canInstallAutomatically:
        root.um ? (root.um.canInstallAutomatically === true) : false
    readonly property string installType: root.um ? root.um.installType : ""
    readonly property bool isDevOrUnknown:
        root.installType === "development" || root.installType === "unknown"
    // One definition of "a check has ever run", shared by the "Last checked"
    // row and the Idle status so they agree. Idle is also the state after a
    // restart until a check runs, while lastCheckTime persists; an unset
    // QDateTime arrives as Invalid Date (getTime() is NaN).
    readonly property bool everChecked:
        !!(root.um && root.um.lastCheckTime
           && !isNaN(root.um.lastCheckTime.getTime()))

    // Local-only banner dismissal: hides this card's banner, never the state; a
    // new Failed state shows it again.
    property bool failureDismissed: false
    // Set only from UpdateManager::managedUpdateHelpRequested; nothing here
    // constructs a command.
    property string managedHelpCommand: ""
    property string managedHelpExplanation: ""
    property bool managedHelpRevealed: false
    property bool managedHelpCopied: false
    // installRefused can fire independently of state (managed install,
    // development build, stale override); shown rather than dropped.
    property string installRefusedReason: ""
    Connections {
        target: root.um
        function onStateChanged() {
            if (root.um.state === UpdateManager.Failed)
                root.failureDismissed = false
        }
        function onManagedUpdateHelpRequested(command, explanation) {
            root.managedHelpCommand = command
            root.managedHelpExplanation = explanation
            root.managedHelpRevealed = true
        }
        function onInstallRefused(reason) {
            root.installRefusedReason = reason
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

    // Mirrors SettingsScreen.qml's formatBytes(), duplicated by that file's
    // convention.
    function formatBytes(n) {
        if (!n || n <= 0) return "0 B"
        if (n < 1024) return n + " B"
        if (n < 1024 * 1024) return Math.round(n / 1024) + " KB"
        return (n / (1024 * 1024)).toFixed(1) + " MB"
    }

    // SettingsScreen's SettingsCard by another name, so these cards sit on the
    // same plane (stormPanel) as every other settings page; see SettingsCard.
    component UpdateCard: Pane {
        Layout.fillWidth: true
        background: Rectangle {
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            radius: AppTheme.radiusMd
        }
    }

    Label {
        text: qsTr("Updates")
        color: AppTheme.stormText
        font.pixelSize: AppTheme.textTitle
        // Matches the other settings section headings.
        font.weight: AppTheme.weightBold
    }
    Label {
        Layout.fillWidth: true
        Layout.topMargin: -AppTheme.spacing8
        wrapMode: Text.WordWrap
        lineHeight: AppTheme.lineHeightBody
        lineHeightMode: Text.ProportionalHeight
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textBody
        text: qsTr("Check for and install new Lightning releases. Every "
                   + "download is verified against a signed manifest "
                   + "before it is ever installed.")
    }

    // This installation
    UpdateCard {
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8
            Label {
                text: qsTr("This installation")
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightStrong
            }
            Label {
                objectName: "updateCurrentVersionLabel"
                color: AppTheme.stormText
                text: qsTr("Version %1")
                      .arg(root.um ? root.um.currentVersion : "")
            }
            Label {
                objectName: "updateInstallTypeLabel"
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: qsTr("Installation type: %1")
                      .arg(root.um ? root.um.installTypeLabel : "")
            }
            Label {
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: qsTr("Update channel: Stable")
            }
        }
    }

    // Automatic checks
    UpdateCard {
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8
            Label {
                text: qsTr("Automatic checks")
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightStrong
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing12
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    color: AppTheme.stormText
                    text: qsTr("Automatically check for updates")
                }
                AppSwitch {
                    id: automaticChecksSwitch
                    objectName: "updateAutomaticChecksSwitch"
                    checked: root.um ? root.um.automaticChecksEnabled : false
                    Accessible.name: qsTr("Automatically check for updates")
                    onToggled: {
                        if (root.um)
                            root.um.automaticChecksEnabled =
                                !root.um.automaticChecksEnabled
                    }
                }
            }
            Label {
                objectName: "updateAutomaticChecksCaption"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                // States the real default (on). Only the release server is
                // contacted, and nothing about the account, device or Matrix
                // data is sent.
                text: qsTr("On by default; turn it off here at any time. "
                           + "Lightning periodically checks our release "
                           + "server for a newer version. The request "
                           + "never includes any account, device, or Matrix "
                           + "information, and creates no tracking "
                           + "identifier; Lightning sends only its own "
                           + "version number.")
            }
            Label {
                objectName: "updateLastCheckedLabel"
                // An unset lastCheckTime is Invalid Date; same idiom as the
                // Sessions "last seen" row.
                visible: root.everChecked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: qsTr("Last checked: %1").arg(
                    root.everChecked
                        ? Qt.formatDateTime(root.um.lastCheckTime,
                                            "d MMM yyyy hh:mm")
                        : "")
            }
            AppButton {
                storm: true
                objectName: "updateCheckNowButton"
                text: root.um && root.um.state === UpdateManager.Checking
                      ? qsTr("Checking…") : qsTr("Check for updates")
                enabled: root.um
                         && root.um.state !== UpdateManager.Checking
                         && root.um.state !== UpdateManager.Downloading
                onClicked: if (root.um) root.um.checkForUpdates()
            }
            Label {
                objectName: "updateDevBuildNotice"
                visible: root.isDevOrUnknown
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: qsTr("Automatic installation is disabled for this "
                           + "build. A manual check may still report "
                           + "whether a newer version exists.")
            }
        }
    }

    // Status
    UpdateCard {
        objectName: "updateStatusCard"
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8

            Label {
                text: qsTr("Status")
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightStrong
            }

            // Non-error diagnostic context ("not downgrading", "prerelease
            // ignored", "not yet published by your package manager"); never a
            // substitute for errorMessage.
            Label {
                objectName: "updateStatusDetailLabel"
                // startInstall() copies its handoff summary into statusDetail,
                // and the restart block binds that summary, so hide it here in
                // RestartRequired.
                visible: root.um && root.um.statusDetail
                         && root.um.statusDetail.length > 0
                         && root.um.statusDetail !== root.um.handoffSummary
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                // statusDetail can carry a channel note from the signed
                // manifest: remote text, never markup.
                textFormat: Text.PlainText
                text: root.um ? root.um.statusDetail : ""
            }

            // A policy refusal can happen even when this pane hides the
            // triggering button; shown and dismissible.
            RowLayout {
                objectName: "updateInstallRefusedBlock"
                visible: root.installRefusedReason.length > 0
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    color: AppTheme.stormDanger
                    font.pixelSize: AppTheme.textMeta
                    text: root.installRefusedReason
                }
                AppButton {
                    storm: true
                    objectName: "updateInstallRefusedDismissButton"
                    text: qsTr("Dismiss")
                    onClicked: root.installRefusedReason = ""
                }
            }

            Label {
                objectName: "updateIdleLabel"
                visible: root.um && root.um.state === UpdateManager.Idle
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormTextMuted
                //: Shown under Status when no update check has run since the
                //: application started, but one has run before -- the date is
                //: on the "Last checked" row above.
                text: root.everChecked
                      ? qsTr("No check has run since Lightning started.")
                      : qsTr("Updates haven't been checked yet.")
            }

            RowLayout {
                objectName: "updateCheckingBlock"
                visible: root.um && root.um.state === UpdateManager.Checking
                spacing: AppTheme.spacing8
                AppProgressBar {
                    indeterminate: true
                    Layout.preferredWidth: 120
                }
                Label {
                    color: AppTheme.stormTextSecondary
                    text: qsTr("Checking for updates…")
                }
            }

            Label {
                objectName: "updateUpToDateLabel"
                visible: root.um && root.um.state === UpdateManager.UpToDate
                color: AppTheme.stormSuccess
                font.weight: AppTheme.weightStrong
                text: qsTr("Lightning is up to date.")
            }

            // Update available
            ColumnLayout {
                id: availableBlock
                objectName: "updateAvailableBlock"
                visible: root.um && root.um.state === UpdateManager.UpdateAvailable
                Layout.fillWidth: true
                spacing: AppTheme.spacing8

                Label {
                    objectName: "updateAvailableVersionLabel"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    color: AppTheme.stormText
                    font.weight: AppTheme.weightStrong
                    text: qsTr("Lightning %1 is available (you have %2).")
                          .arg(root.um ? root.um.latestVersion : "")
                          .arg(root.um ? root.um.currentVersion : "")
                }
                Label {
                    visible: root.um && root.um.totalBytes > 0
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textMeta
                    text: qsTr("Download size: %1")
                          .arg(root.formatBytes(
                              root.um ? root.um.totalBytes : 0))
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textMeta
                    text: qsTr("Update method: %1")
                          .arg(root.um ? root.um.installTypeLabel : "")
                }

                // Package-managed (Flatpak/Snap): no download/install, only the
                // disclosure and a help action.
                ColumnLayout {
                    objectName: "updateManagedBlock"
                    visible: root.packageManaged
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing8
                    Label {
                        objectName: "updateManagedMessage"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                        color: AppTheme.stormTextSecondary
                        text: root.installType === "linux-flatpak"
                            ? qsTr("Updates for this installation are managed by Flatpak.")
                            : root.installType === "linux-snap"
                                ? qsTr("Updates for this installation are managed by Snap.")
                                : ""
                    }
                    AppButton {
                        storm: true
                        objectName: "updateManagedHelpButton"
                        text: qsTr("Get update instructions")
                        onClicked: if (root.um) root.um.openManagedUpdateHelp()
                    }
                    // The exact command and explanation from
                    // managedUpdateHelpRequested.
                    ColumnLayout {
                        objectName: "updateManagedCommandBlock"
                        visible: root.managedHelpRevealed
                                 && root.managedHelpCommand.length > 0
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing4
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing8
                            Label {
                                objectName: "updateManagedCommandText"
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
                                objectName: "updateManagedCopyCommandButton"
                                text: root.managedHelpCopied
                                      ? qsTr("Copied") : qsTr("Copy")
                                onClicked: {
                                    managedCommandClipboard.text =
                                        root.managedHelpCommand
                                    managedCommandClipboard.selectAll()
                                    managedCommandClipboard.copy()
                                    managedCommandClipboard.text = ""
                                    root.managedHelpCopied = true
                                    managedHelpCopiedTimer.restart()
                                }
                            }
                        }
                    }
                }

                // Self-updating installs only.
                RowLayout {
                    objectName: "updateActionsRow"
                    visible: !root.packageManaged && root.canInstallAutomatically
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing8
                    AppButton {
                        storm: true
                        kind: "primary"
                        objectName: "updateNowButton"
                        text: qsTr("Update now")
                        onClicked: if (root.um) root.um.downloadUpdate()
                    }
                    AppButton {
                        storm: true
                        objectName: "updateLaterButton"
                        text: qsTr("Later")
                        onClicked: if (root.um) root.um.dismissVersion()
                    }
                }
            }

            // Downloading
            ColumnLayout {
                objectName: "updateDownloadingBlock"
                visible: root.um && root.um.state === UpdateManager.Downloading
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                Label {
                    color: AppTheme.stormTextSecondary
                    text: qsTr("Downloading update…")
                }
                AppProgressBar {
                    objectName: "updateDownloadProgressBar"
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    value: root.um ? root.um.downloadProgress : 0
                }
                Label {
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textMeta
                    text: root.um && root.um.totalBytes > 0
                        ? qsTr("%1 of %2")
                            .arg(root.formatBytes(root.um.downloadedBytes))
                            .arg(root.formatBytes(root.um.totalBytes))
                        : root.formatBytes(root.um ? root.um.downloadedBytes : 0)
                }
                AppButton {
                    storm: true
                    objectName: "updateCancelDownloadButton"
                    text: qsTr("Cancel")
                    onClicked: if (root.um) root.um.cancelDownload()
                }
            }

            // Verifying (indeterminate)
            RowLayout {
                objectName: "updateVerifyingBlock"
                visible: root.um && root.um.state === UpdateManager.Verifying
                spacing: AppTheme.spacing8
                AppProgressBar {
                    indeterminate: true
                    Layout.preferredWidth: 120
                }
                Label {
                    color: AppTheme.stormTextSecondary
                    text: qsTr("Verifying download…")
                }
            }

            // Ready to install. installAndRestart()/installUpdate() are only
            // valid here (startInstall guards on ReadyToInstall) and are only
            // called from here. installAndRestart() also requests quit so the
            // updater helper can finish; installUpdate() applies on the user's
            // own next quit.
            ColumnLayout {
                objectName: "updateReadyToInstallBlock"
                visible: root.um && root.um.state === UpdateManager.ReadyToInstall
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    color: AppTheme.stormTextSecondary
                    text: qsTr("Update %1 downloaded and verified.")
                          .arg(root.um ? root.um.latestVersion : "")
                }
                RowLayout {
                    objectName: "updateReadyToInstallActionsRow"
                    visible: root.canInstallAutomatically
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing8
                    AppButton {
                        storm: true
                        kind: "primary"
                        objectName: "updateInstallAndRestartButton"
                        text: qsTr("Install and restart")
                        onClicked: if (root.um) root.um.installAndRestart()
                    }
                    AppButton {
                        storm: true
                        objectName: "updateInstallButton"
                        text: qsTr("Install without restarting")
                        onClicked: if (root.um) root.um.installUpdate()
                    }
                }
            }

            // Installing (indeterminate)
            RowLayout {
                objectName: "updateInstallingBlock"
                visible: root.um && root.um.state === UpdateManager.Installing
                spacing: AppTheme.spacing8
                AppProgressBar {
                    indeterminate: true
                    Layout.preferredWidth: 120
                }
                Label {
                    color: AppTheme.stormTextSecondary
                    text: qsTr("Installing…")
                }
            }

            // Restart required: informational only. Both install calls guard on
            // ReadyToInstall, so a "Restart now" button here would do nothing.
            // After "Install and restart" the app is already quitting; after
            // "Install without restarting" the update applies on the user's
            // next quit.
            ColumnLayout {
                objectName: "updateRestartRequiredBlock"
                visible: root.um && root.um.state === UpdateManager.RestartRequired
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    color: AppTheme.stormText
                    font.weight: AppTheme.weightStrong
                    // Not "is installed": the verified artifact has been handed
                    // to the updater helper, which waits for Lightning to exit,
                    // so the install can still fail.
                    text: qsTr("Lightning %1 is ready to install.")
                          .arg(root.um ? root.um.latestVersion : "")
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textMeta
                    // The manager words this per entry point; bind rather than
                    // restate it.
                    text: root.um ? root.um.handoffSummary : ""
                }
            }

            // Outcome of the previous run's update. The helper works after
            // Lightning exits, so its result (e.g. installer-exit-100,
            // layout-invalid, rollback-failed) can only be shown on the next
            // start. Shown once, then dismissed.
            ColumnLayout {
                objectName: "updateLastResultBlock"
                visible: root.um
                         && root.um.lastUpdateResult !== UpdateManager.NoResult
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                Label {
                    objectName: "updateLastResultLabel"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    // lastUpdateError comes from a file on disk: plain text
                    // only.
                    textFormat: Text.PlainText
                    color: root.um
                           && root.um.lastUpdateResult === UpdateManager.InstallFailed
                           ? AppTheme.stormDanger : AppTheme.stormText
                    // lastUpdateError is a short sanitized token from the
                    // helper's enum (never a path, command or process output).
                    // Lead with an explanatory sentence; the token follows on
                    // its own line for bug reports.
                    text: {
                        if (!root.um)
                            return ""
                        if (root.um.lastUpdateResult !== UpdateManager.InstallFailed)
                            return qsTr("The last update was installed successfully.")
                        // Called on the instance: a Q_INVOKABLE static on an
                        // uncreatable non-singleton is not reliably reachable
                        // through the type name.
                        var explained = root.um.explainInstallError(
                            root.um.lastUpdateError)
                        return explained.length > 0
                            ? explained
                            : qsTr("The last update could not be installed, and "
                                   + "nothing was changed.")
                    }
                }
                Label {
                    objectName: "updateLastResultCode"
                    visible: root.um
                             && root.um.lastUpdateResult === UpdateManager.InstallFailed
                             && root.um.lastUpdateError.length > 0
                    Layout.fillWidth: true
                    text: qsTr("Code: %1").arg(root.um ? root.um.lastUpdateError : "")
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textMeta
                    wrapMode: Text.WordWrap
                }
                AppButton {
                    objectName: "updateLastResultDismissButton"
                    text: qsTr("Dismiss")
                    Accessible.name: text
                    onClicked: if (root.um) root.um.clearLastUpdateResult()
                }
            }

            // Failed: terminal, no bypass of any kind
            ColumnLayout {
                objectName: "updateFailedBlock"
                visible: root.um && root.um.state === UpdateManager.Failed
                         && !root.failureDismissed
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                Label {
                    objectName: "updateErrorMessageLabel"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    color: AppTheme.stormDanger
                    font.weight: AppTheme.weightStrong
                    // Error text embeds manifest-derived detail; plain text
                    // only.
                    textFormat: Text.PlainText
                    text: root.um ? root.um.errorMessage : ""
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textMeta
                    text: qsTr("Lightning never installs an update that "
                               + "fails verification. There is no way "
                               + "to bypass this check.")
                }
                RowLayout {
                    spacing: AppTheme.spacing8
                    AppButton {
                        storm: true
                        objectName: "updateRetryButton"
                        text: qsTr("Retry")
                        onClicked: if (root.um) root.um.checkForUpdates()
                    }
                    AppButton {
                        storm: true
                        objectName: "updateDismissFailureButton"
                        text: qsTr("Dismiss")
                        onClicked: root.failureDismissed = true
                    }
                }
            }
        }
    }
}
