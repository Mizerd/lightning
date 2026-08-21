import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings -> Updates (UPDATE-SPEC.md v1). Binds directly to UpdateManager,
// reached the exact same way every other settings sub-object is reached from
// QML: a QObject* CONSTANT property on AppController (app.settings, app.gif,
// app.crypto, ...) — here
//     Q_PROPERTY(UpdateManager* updateManager READ updateManager CONSTANT)
// exposed as `app.updateManager`. UpdateManager itself is
// deliberately account-agnostic (§8: no AppController/SettingsManager
// account scoping for its own state); this file only reaches it through the
// existing `app` root context property the same way every other pane does.
// `state` is UpdateManager's real Q_ENUM (QML_ELEMENT + Q_ENUM(State), see
// src/update/UpdateManager.h) — compared here against the enum, e.g.
// `root.um.state === UpdateManager.Idle`, never against a string literal.
// `lastCheckTime` is a QDateTime (a JS Date in QML), not a string.
//
// Root type mirrors every other in-file settings pane in SettingsScreen.qml
// (a ColumnLayout, visibility-toggled by the caller — never a Loader, so
// this pane's own local state, e.g. failureDismissed below, survives
// switching to another settings category and back).
//
// Trust-chain honesty (§0/§9 of the spec) drives every branch here:
//   - a hash/signature failure is TERMINAL — nothing in this file offers a
//     way past it, only Retry (a clean re-check) and a local dismiss of
//     the banner text;
//   - Flatpak/Snap installs (packageManaged) NEVER get a download/install
//     action — only the disclosure sentence + a help action;
//   - development/unknown installs (!canInstallAutomatically) never get an
//     install action either, though a manual check may still run;
//   - restarting is always one explicit click (installAndRestart()), never
//     automatic, never silent.
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

    // Local-only failure-banner dismissal. There is no backend "dismiss
    // error" call — this hides only this one card's banner, never the fact
    // itself (errorMessage/state stay exactly what UpdateManager reports),
    // and a freshly-arriving Failed state always re-shows it.
    property bool failureDismissed: false
    // Set from UpdateManager::managedUpdateHelpRequested(command,
    // explanation) — the ONLY source of this text; nothing here constructs
    // a command itself.
    property string managedHelpCommand: ""
    property string managedHelpExplanation: ""
    property bool managedHelpRevealed: false
    property bool managedHelpCopied: false
    // installRefused fires when a policy refusal happens independently of
    // state (managed install, development build, a stale diagnostic
    // override) — surfaced honestly rather than silently dropped.
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

    // Mirrors SettingsScreen.qml's own formatBytes() (Starred-GIF summary
    // row) — small, presentation-only, duplicated per that file's own
    // documented convention rather than shared.
    function formatBytes(n) {
        if (!n || n <= 0) return "0 B"
        if (n < 1024) return n + " B"
        if (n < 1024 * 1024) return Math.round(n / 1024) + " KB"
        return (n / (1024 * 1024)).toFixed(1) + " MB"
    }

    // Progress bar, styled once.
    //
    // main.cpp sets QQuickStyle "Basic", whose ProgressBar fills in
    // palette.dark on a palette.midlight track — Main.qml maps those to
    // AppTheme.textSecondary and AppTheme.border, so the download bar (the
    // most-watched progress affordance in the app) rendered as body-text
    // GREY on a hairline, square-ended, directly under a text-size slider
    // that fills bolt on stormInset. Same geometry as that slider's track:
    // 4px, pill ends, stormInset behind, bolt in front.
    component UpdateCard: Pane {
        Layout.fillWidth: true
        background: Rectangle {
            color: AppTheme.stormCanvas
            border.color: AppTheme.stormBorder
            radius: AppTheme.radiusMd
        }
    }

    Label {
        text: qsTr("Updates")
        color: AppTheme.stormText
        font.pixelSize: AppTheme.textTitle
        // Matches the other settings section headings, which were
        // ExtraBold(800) while this one was DemiBold(600) for no reason.
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

    // ── This installation ───────────────────────────────────────────────
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

    // ── Automatic checks ────────────────────────────────────────────────
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
                // ON by default since v0.7.3 — the copy must state the
                // real default, and the privacy guarantees below are
                // unchanged: Lightning contacts only the release server,
                // and nothing about this account, device, or any Matrix
                // data is ever sent with the request.
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
                // lastCheckTime is a QDateTime (marshaled to a JS Date), not
                // a string — an unset one arrives as Invalid Date, whose
                // getTime() is NaN. Same validity idiom as the Sessions
                // "last seen" row above in SettingsScreen.qml.
                visible: root.um && root.um.lastCheckTime
                         && !isNaN(root.um.lastCheckTime.getTime())
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: qsTr("Last checked: %1").arg(
                    root.um && root.um.lastCheckTime
                    && !isNaN(root.um.lastCheckTime.getTime())
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

    // ── Status ───────────────────────────────────────────────────────────
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
            // ignored", "your package manager has not published this
            // version yet") — informational, never a substitute for
            // errorMessage below.
            Label {
                objectName: "updateStatusDetailLabel"
                // Never the same sentence twice. startInstall() copies its
                // handoff summary into statusDetail, and the restart block
                // below binds that summary directly — so in RestartRequired
                // the identical text rendered once above the install buttons
                // and once below them.
                visible: root.um && root.um.statusDetail
                         && root.um.statusDetail.length > 0
                         && root.um.statusDetail !== root.um.handoffSummary
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: root.um ? root.um.statusDetail : ""
            }

            // A policy refusal (installRefused) can happen even when this
            // pane's own gating already hides the triggering button — kept
            // honest and dismissible rather than silently discarded.
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
                text: qsTr("Updates haven't been checked yet.")
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

            // ── Update available ────────────────────────────────────────
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

                // Package-managed (Flatpak/Snap): NO download/install
                // action is ever offered — only the disclosure and a
                // help action.
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
                    // Revealed after the button above — the exact command
                    // and explanation UpdateManager itself emitted via
                    // managedUpdateHelpRequested, nothing constructed here.
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

            // ── Downloading ──────────────────────────────────────────────
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

            // ── Verifying (indeterminate) ─────────────────────────────────
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

            // ── Ready to install ────────────────────────────────────────
            // installAndRestart()/installUpdate() are ONLY valid from this
            // state (UpdateManager::startInstall guards on
            // m_state == ReadyToInstall and both calls leave RestartRequired
            // behind them) — this is deliberately the ONLY place either is
            // ever called from. installAndRestart() also requests the
            // application quit (UpdateManager::quitRequested) so the
            // updater helper can finish; installUpdate() installs and
            // leaves restarting for later, entirely on the user's own next
            // quit.
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

            // ── Installing (indeterminate) ────────────────────────────────
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

            // ── Restart required ────────────────────────────────────────
            // Purely informational, deliberately with NO button that calls
            // back into UpdateManager: installAndRestart()/installUpdate()
            // both guard on state === ReadyToInstall (see above), which is
            // already behind this point — a "Restart now" button here would
            // silently no-op. Reaching this state already required one
            // explicit click at Ready to install (either action); if that
            // was "Install and restart", the application is already in the
            // process of quitting (quitRequested); if it was "Install
            // without restarting", the update takes effect on the user's
            // OWN next quit — still explicit, still never automatic, never
            // silent, just not forced by Lightning.
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
                    // NOT "is installed". This state means the verified
                    // artifact has been handed to the updater helper, which
                    // waits for Lightning to exit before touching anything —
                    // so at this moment nothing has been installed yet, and
                    // the install can still fail. Saying otherwise was the
                    // review's M2 finding: it made a failed install look like
                    // a success, since the outcome only appears next start.
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
                    // The manager words this differently for the two entry
                    // points (quitting now vs. applying on your own next
                    // quit), so bind it rather than restating it here.
                    text: root.um ? root.um.handoffSummary : ""
                }
            }

            // ── Outcome of the PREVIOUS run's update ──────────────────────
            // The helper does its work after Lightning has exited, so the only
            // moment its result can be shown is the next start. Without this
            // the typed failures it records (installer-exit-100,
            // layout-invalid, rollback-failed) were written to disk and thrown
            // away, and a failed update was indistinguishable from a silent
            // one. Shown once, then dismissed for good.
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
                    // lastUpdateError comes from a file on disk. Plain text
                    // explicitly, never the AutoText rich-text heuristic.
                    textFormat: Text.PlainText
                    color: root.um
                           && root.um.lastUpdateResult === UpdateManager.InstallFailed
                           ? AppTheme.stormDanger : AppTheme.stormText
                    // lastUpdateError is a short sanitized token from the
                    // helper's own enum — never a path, never a command, never
                    // process output.
                    text: root.um && root.um.lastUpdateResult === UpdateManager.InstallFailed
                          ? qsTr("The last update could not be installed (%1).")
                            .arg(root.um.lastUpdateError)
                          : qsTr("The last update was installed successfully.")
                }
                AppButton {
                    objectName: "updateLastResultDismissButton"
                    text: qsTr("Dismiss")
                    Accessible.name: text
                    onClicked: if (root.um) root.um.clearLastUpdateResult()
                }
            }

            // ── Failed — terminal; no bypass of any kind ──────────────────
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
