import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings -> Account: a settings file (export / import) and "Sync settings
// across your devices". Both move the same allow-listed preference set
// (src/settings/SettingsPortable.*); neither ever carries a sign-in, a key, a
// device, a file location or anything about an account. Bound to
// app.settingsSync.
ColumnLayout {
    id: backup
    objectName: "settingsBackupSection"
    Layout.fillWidth: true
    spacing: AppTheme.spacing12

    readonly property var sync: (typeof app !== "undefined" && app)
                                ? app.settingsSync : null
    readonly property bool syncAvailable: !!sync && sync.supported
    property var preview: ({})
    property string notice: ""
    property bool noticeIsError: false

    function groupLabel(group) {
        switch (group) {
        case "Appearance": return qsTr("Appearance")
        case "Layout": return qsTr("Layout")
        case "Window": return qsTr("Window")
        case "Notifications": return qsTr("Notifications and sounds")
        case "Privacy": return qsTr("Privacy")
        case "Media": return qsTr("Media and previews")
        case "Calls": return qsTr("Calls")
        case "Timeline": return qsTr("Timeline")
        case "Composer": return qsTr("Composer")
        }
        return group
    }
    function valueText(v) {
        if (v === true) return qsTr("on")
        if (v === false) return qsTr("off")
        if (v === undefined || v === null) return qsTr("not set")
        if (Array.isArray(v)) return v.length > 0 ? v.join(", ") : qsTr("none")
        var s = String(v)
        return s.length > 0 ? s : qsTr("none")
    }

    component CardTitle: Label {
        color: AppTheme.stormTextSecondary
        font.pixelSize: AppTheme.textBody
        font.weight: AppTheme.weightStrong
    }
    component Hint: Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        lineHeight: AppTheme.lineHeightBody
        lineHeightMode: Text.ProportionalHeight
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
    }
    component Card: Pane {
        Layout.fillWidth: true
        background: Rectangle {
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            radius: AppTheme.radiusMd
        }
    }

    // ---- the settings file -------------------------------------------------
    Card {
        objectName: "settingsFileCard"
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8
            CardTitle { text: qsTr("Settings file") }
            Hint {
                text: qsTr("Save your preferences to a file, or load them from one. "
                           + "It holds how Lightning looks and behaves: appearance, "
                           + "layout, notifications, sounds, shortcuts and your custom "
                           + "themes. It never holds your sign-in, keys, devices, "
                           + "accounts or where files are kept on this computer.")
            }
            RowLayout {
                spacing: AppTheme.spacing8
                AppButton {
                    storm: true
                    objectName: "exportSettingsButton"
                    text: qsTr("Export settings…")
                    enabled: !!backup.sync
                    onClicked: exportDialog.open()
                }
                AppButton {
                    storm: true
                    objectName: "importSettingsButton"
                    text: qsTr("Import settings…")
                    enabled: !!backup.sync
                    onClicked: importFileDialog.open()
                }
            }
            Label {
                objectName: "settingsFileNotice"
                Layout.fillWidth: true
                visible: backup.notice.length > 0
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                font.pixelSize: AppTheme.textMeta
                color: backup.noticeIsError ? AppTheme.stormDanger
                                             : AppTheme.stormTextMuted
                text: backup.notice
            }
        }
    }

    // ---- sync ----------------------------------------------------------------
    Card {
        objectName: "settingsSyncCard"
        visible: backup.syncAvailable
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8
            CardTitle { text: qsTr("Sync across your devices") }
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                AppSwitch {
                    id: syncSwitch
                    objectName: "settingsSyncSwitch"
                    checked: !!backup.sync && backup.sync.enabled
                    enabled: !!backup.sync && backup.sync.state !== "busy"
                    // The controller's answer moves `checked`, not the click.
                    onToggled: backup.sync.setSyncEnabled(!backup.sync.enabled)
                    Accessible.name: syncLabel.text
                }
                Label {
                    id: syncLabel
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textBody
                    text: qsTr("Sync settings across your devices")
                    TapHandler {
                        onTapped: if (syncSwitch.enabled)
                                      backup.sync.setSyncEnabled(!backup.sync.enabled)
                    }
                }
            }
            Hint {
                text: qsTr("Keeps the same preferences on every device signed in to "
                           + "this account, and brings them back when you sign in on "
                           + "a new one. They are stored in your account data on your "
                           + "homeserver, which its administrator can read; they hold "
                           + "no keys or passwords. Things that belong to one computer "
                           + "(window size, zoom, volumes, microphone and camera, "
                           + "privacy and security switches, custom themes) stay on "
                           + "that computer.")
            }
            Label {
                objectName: "settingsSyncStatus"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                font.pixelSize: AppTheme.textMeta
                color: backup.sync && backup.sync.state === "error"
                       ? AppTheme.stormDanger : AppTheme.stormTextMuted
                visible: text.length > 0
                text: {
                    if (!backup.sync) return ""
                    switch (backup.sync.state) {
                    case "checking": return qsTr("Checking your homeserver…")
                    case "busy": return qsTr("Syncing…")
                    case "readOnly":
                        return qsTr("Your account's settings were saved by a newer "
                                    + "Lightning. This version reads what it knows "
                                    + "and writes nothing.")
                    case "error":
                        return qsTr("Settings could not be synced (%1). Trying again "
                                    + "shortly.").arg(backup.sync.error)
                    case "idle":
                        if (backup.sync.pendingCount > 0)
                            return qsTr("%n change(s) waiting to sync.", "",
                                        backup.sync.pendingCount)
                        if (backup.sync.lastSyncedMs > 0)
                            return qsTr("Up to date. Last synced %1.")
                                   .arg(new Date(backup.sync.lastSyncedMs)
                                        .toLocaleString(Qt.locale(AppLocale.name), Locale.ShortFormat))
                        return qsTr("Up to date.")
                    }
                    return ""
                }
            }

            // First enable with settings on both sides that disagree.
            ColumnLayout {
                objectName: "settingsSyncChoice"
                Layout.fillWidth: true
                visible: !!backup.sync && backup.sync.state === "choice"
                spacing: AppTheme.spacing8
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textBody
                    text: backup.sync
                          ? qsTr("Your account already has synced settings, and %n "
                                 + "setting(s) you chose on this device differ. "
                                 + "Nothing has been changed yet.", "",
                                 backup.sync.choiceDiffering)
                          : ""
                }
                RowLayout {
                    spacing: AppTheme.spacing8
                    AppButton {
                        storm: true
                        kind: "primary"
                        objectName: "settingsSyncAdoptButton"
                        text: qsTr("Use my account's settings")
                        onClicked: backup.sync.chooseAdoptServer()
                    }
                    AppButton {
                        storm: true
                        objectName: "settingsSyncUploadButton"
                        text: qsTr("Upload this device's settings")
                        onClicked: backup.sync.chooseUploadMine()
                    }
                }
            }

            // A later change elsewhere replaced an unsynced one here.
            ColumnLayout {
                objectName: "settingsSyncConflicts"
                Layout.fillWidth: true
                visible: !!backup.sync && backup.sync.conflicts.length > 0
                spacing: AppTheme.spacing4
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textMeta
                    text: backup.sync
                          ? qsTr("Changed on another device since you edited them here, "
                                 + "so the newer value was kept: %1.")
                            .arg(backup.sync.conflicts.join(", "))
                          : ""
                }
                AppButton {
                    storm: true
                    objectName: "settingsSyncConflictsOk"
                    text: qsTr("OK")
                    onClicked: backup.sync.acknowledgeConflicts()
                }
            }

            AppButton {
                storm: true
                kind: "danger"
                objectName: "settingsSyncRemoveButton"
                visible: !!backup.sync && backup.sync.enabled
                text: qsTr("Turn off and remove from my homeserver")
                onClicked: removeConfirm.open()
            }
        }
    }

    // ---- dialogs -------------------------------------------------------------
    NativeFileDialog {
        id: exportDialog
        purpose: "settings"
        objectName: "exportSettingsDialog"
        fileMode: "save"
        currentFolder: app.defaultFileDialogFolder()
        currentName: "lightning-settings.json"
        title: qsTr("Export settings")
        nameFilters: [ qsTr("Settings (*.json)"), qsTr("All files (*)") ]
        onAccepted: {
            var problem = app.settingsSync.exportSettings(selectedFile)
            backup.noticeIsError = problem.length > 0
            backup.notice = problem.length > 0
                             ? problem : qsTr("Settings exported.")
        }
    }
    NativeFileDialog {
        id: importFileDialog
        purpose: "settings"
        objectName: "importSettingsDialog"
        fileMode: "open"
        currentFolder: app.defaultFileDialogFolder()
        title: qsTr("Import settings")
        nameFilters: [ qsTr("Settings (*.json)"), qsTr("All files (*)") ]
        onAccepted: {
            var p = app.settingsSync.previewImport(selectedFile)
            if (!p.ok) {
                backup.preview = ({})
                backup.noticeIsError = true
                backup.notice = p.error
                return
            }
            backup.notice = ""
            backup.preview = p
            importPreview.open()
        }
    }

    AppDialog {
        id: importPreview
        objectName: "importSettingsPreview"
        title: qsTr("Import settings")
        parent: Overlay.overlay
        width: Math.min(560, parent ? parent.width - AppTheme.spacing24 * 2 : 560)
        onClosed: if (!importApplying) app.settingsSync.cancelImport()
        property bool importApplying: false

        contentItem: ColumnLayout {
            spacing: AppTheme.spacing8
            Label {
                objectName: "importPreviewSummary"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textBody
                text: {
                    var n = backup.preview.changes ? backup.preview.changes.length : 0
                    var extra = (backup.preview.shortcutChanges || 0)
                                + (backup.preview.themesNew || 0)
                    if (n === 0 && extra === 0)
                        return qsTr("This file changes nothing: every setting in it "
                                    + "already has that value.")
                    return qsTr("Importing will change %n setting(s).", "", n)
                }
            }
            Label {
                Layout.fillWidth: true
                visible: !!backup.preview.newerVersion
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: qsTr("This file was made by a newer Lightning. The settings "
                           + "this version knows are read; the rest are ignored.")
            }
            ListView {
                objectName: "importPreviewList"
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(contentHeight, 280)
                clip: true
                model: backup.preview.changes || []
                section.property: "group"
                section.delegate: Label {
                    required property string section
                    width: ListView.view.width
                    topPadding: AppTheme.spacing8
                    text: backup.groupLabel(section)
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textMeta
                    font.weight: AppTheme.weightStrong
                }
                delegate: Label {
                    width: ListView.view.width
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textMeta
                    text: qsTr("%1: %2 → %3").arg(modelData.label)
                          .arg(backup.valueText(modelData.from))
                          .arg(backup.valueText(modelData.to))
                }
            }
            Label {
                Layout.fillWidth: true
                visible: (backup.preview.shortcutChanges || 0) > 0
                         || (backup.preview.themesNew || 0) > 0
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textMeta
                text: {
                    var parts = []
                    if ((backup.preview.shortcutChanges || 0) > 0)
                        parts.push(qsTr("%n shortcut(s)", "", backup.preview.shortcutChanges))
                    if ((backup.preview.themesNew || 0) > 0)
                        parts.push(qsTr("%n custom theme(s) added", "", backup.preview.themesNew))
                    return parts.join(", ")
                }
            }
            Label {
                objectName: "importPreviewRejected"
                Layout.fillWidth: true
                visible: !!backup.preview.rejected && backup.preview.rejected.length > 0
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                color: AppTheme.stormDanger
                font.pixelSize: AppTheme.textMeta
                text: {
                    var r = backup.preview.rejected || []
                    var items = []
                    for (var i = 0; i < r.length; ++i)
                        items.push(r[i].label + " (" + r[i].reason + ")")
                    return qsTr("Skipped because the value is not valid: %1.")
                           .arg(items.join(", "))
                }
            }
            Label {
                Layout.fillWidth: true
                visible: (backup.preview.unknownCount || 0) > 0
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: qsTr("%n entry(ies) this version does not know were ignored.",
                           "", backup.preview.unknownCount || 0)
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: AppTheme.spacing8
                AppButton {
                    storm: true
                    objectName: "importCancelButton"
                    text: qsTr("Cancel")
                    onClicked: importPreview.close()
                }
                AppButton {
                    storm: true
                    kind: "primary"
                    objectName: "importApplyButton"
                    text: qsTr("Import")
                    onClicked: {
                        importPreview.importApplying = true
                        var res = app.settingsSync.applyImport()
                        importPreview.importApplying = false
                        backup.noticeIsError = !res.ok
                        if (res.ok) {
                            var msg = qsTr("Imported %n setting(s).", "", res.applied)
                            if (res.shortcutsSkipped > 0)
                                msg += " " + qsTr("%n shortcut(s) could not be used and were skipped.",
                                                  "", res.shortcutsSkipped)
                            backup.notice = msg
                        } else {
                            backup.notice = res.error
                        }
                        importPreview.close()
                    }
                }
            }
        }
    }

    AppDialog {
        id: removeConfirm
        objectName: "settingsSyncRemoveConfirm"
        title: qsTr("Remove synced settings?")
        parent: Overlay.overlay
        width: Math.min(480, parent ? parent.width - AppTheme.spacing24 * 2 : 480)
        contentItem: ColumnLayout {
            spacing: AppTheme.spacing8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textBody
                text: qsTr("This turns settings sync off for every device on this "
                           + "account and empties the copy on your homeserver. "
                           + "Every device keeps the settings it has now.")
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: AppTheme.spacing8
                AppButton {
                    storm: true
                    text: qsTr("Cancel")
                    onClicked: removeConfirm.close()
                }
                AppButton {
                    storm: true
                    kind: "dangerPrimary"
                    objectName: "settingsSyncRemoveConfirmButton"
                    text: qsTr("Turn off and remove")
                    onClicked: {
                        backup.sync.removeFromServer()
                        removeConfirm.close()
                    }
                }
            }
        }
    }
}
