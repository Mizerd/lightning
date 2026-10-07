import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings -> Appearance: chat backgrounds and surface depth.
//
//   * Your own background for every room (ChatBackgroundEditor, "default").
//   * Where the other kinds live: one room's own picture, and a picture shared
//     with everyone in a room or Space, are set from Room information.
//   * "Show backgrounds set by others" (app.backdrops.showShared, default on).
//     Off hides every room's and Space's shared picture; your own still show.
//   * Depth: flat surfaces, or a gentle light-from-above gradient on the
//     window's large grounds that only ever raises text contrast; and the way
//     to a gradient of your own, which is a custom theme's (Fill in the theme
//     editor), so the two kinds of gradient point at each other.
ColumnLayout {
    id: section
    objectName: "chatBackgroundSettings"
    spacing: AppTheme.spacing8

    // "Make your own gradient": the host opens the theme editor on a surface.
    signal makeGradientRequested()

    readonly property bool available: typeof app !== "undefined" && app
                                      && !!app.backdrops

    component GroupLabel: Label {
        Layout.topMargin: AppTheme.spacing8
        color: AppTheme.stormTextMuted
        font.family: AppTheme.menuSectionFont
        font.pixelSize: AppTheme.menuSectionSize
        font.weight: AppTheme.menuSectionWeight
        font.letterSpacing: AppTheme.menuSectionTracking
    }
    component Hint: Label {
        Layout.fillWidth: true
        Layout.leftMargin: AppTheme.spacing4
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        lineHeight: AppTheme.lineHeightBody
        lineHeightMode: Text.ProportionalHeight
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
    }

    GroupLabel {
        objectName: "chatBackgroundHeading"
        text: qsTr("Chat background")
    }
    Hint {
        text: qsTr("A picture behind every conversation. Only you see it.")
    }
    ChatBackgroundEditor {
        objectName: "chatBackgroundDefaultEditor"
        Layout.fillWidth: true
        Layout.maximumWidth: 520
        scopeKind: "default"
        showTitle: false
        storm: true
        visible: section.available
    }
    Hint {
        objectName: "chatBackgroundPerRoomHint"
        text: qsTr("For one room only, or a picture everyone in a room or space "
                   + "sees: choose Chat background… from the room's ⋮ menu or "
                   + "right-click the room in the list.")
    }

    // Your own backgrounds (this one and every room's "Only me" picture) on
    // the homeserver: account data plus an encrypted upload, so they follow
    // the account to other devices and come back after a sign-out, which
    // deletes every copy on this device. app.backdrops.syncEnabled, per
    // account, default on. Turning it off asks about the server copies.
    readonly property bool syncAvailable: available && app.backdrops.syncAvailable
    readonly property var syncStatus: syncAvailable ? app.backdrops.syncStatus : ({})
    readonly property string syncState: syncStatus.state !== undefined
                                        ? syncStatus.state : "unavailable"

    // On asks nothing; off asks what happens to the server copies.
    function toggleSync() {
        if (!syncAvailable)
            return
        if (app.backdrops.syncEnabled)
            syncOffDialog.open()
        else
            app.backdrops.setSyncEnabled(true, false)
    }

    function syncErrorText(category) {
        switch (category) {
        case "": return ""
        case "rate_limited":
            return qsTr("the server is limiting requests")
        case "network": case "timeout":
            return qsTr("the server could not be reached")
        case "upload_too_large": case "too_large":
            return qsTr("a picture is too large for the server")
        case "forbidden": case "signed_out":
            return qsTr("the server refused it")
        case "newer_schema":
            return qsTr("a newer version of Lightning saved it")
        case "sync_disabled":
            return qsTr("it was turned off on another device")
        case "too_many":
            return qsTr("you have your own background in too many rooms")
        case "read_failed":
            return qsTr("your homeserver could not be read")
        case "undecodable": case "unsupported_image": case "invalid":
            return qsTr("a saved picture could not be used")
        default:
            return qsTr("error: %1").arg(category)
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: AppTheme.spacing4
        spacing: AppTheme.spacing12
        visible: section.syncAvailable
        AppSwitch {
            id: syncSwitch
            objectName: "backgroundSyncSwitch"
            checked: section.syncAvailable && app.backdrops.syncEnabled
            // AppSwitch does not flip itself: `checked` is still the current
            // state here, and the controller's answer moves it.
            onToggled: section.toggleSync()
            Accessible.name: syncLabel.text
        }
        Label {
            id: syncLabel
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            color: AppTheme.stormText
            font.pixelSize: AppTheme.textBody
            text: qsTr("Keep my backgrounds on my homeserver")
            TapHandler { onTapped: section.toggleSync() }
        }
    }
    Hint {
        objectName: "backgroundSyncPrivacyHint"
        visible: section.syncAvailable
        text: qsTr("Your own backgrounds then follow you to your other devices "
                   + "and come back when you sign in again. The server stores "
                   + "each picture encrypted, but the key is kept in your "
                   + "account data, which your homeserver's administrator can "
                   + "read.")
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: AppTheme.spacing8
        visible: section.syncAvailable && backgroundSyncStatus.text.length > 0
        Label {
            id: backgroundSyncStatus
            objectName: "backgroundSyncStatus"
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing4
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            font.pixelSize: AppTheme.textMeta
            color: section.syncState === "failed"
                   || section.syncStatus.removal === "failed"
                   || section.syncStatus.removal === "partial"
                   ? AppTheme.stormDanger : AppTheme.stormTextMuted
            text: {
                const s = section.syncStatus
                switch (section.syncState) {
                case "working":
                    if (s.uploading > 0)
                        return qsTr("Saving %n background(s) on your "
                                    + "homeserver…", "", s.uploading)
                    if (s.downloading > 0)
                        return qsTr("Fetching %n background(s) from your "
                                    + "homeserver…", "", s.downloading)
                    return qsTr("Checking your homeserver…")
                case "removing":
                    return qsTr("Removing your backgrounds from your homeserver…")
                case "failed":
                    return qsTr("%n background(s) could not be saved on or "
                                + "fetched from your homeserver (%1).", "",
                                Math.max(1, s.failed))
                           .arg(section.syncErrorText(s.error))
                case "off":
                    if (s.removal === "ok")
                        return qsTr("Removed from your homeserver. Your other "
                                    + "devices keep their own copies until you "
                                    + "remove them there. The encrypted "
                                    + "pictures already uploaded stay in its "
                                    + "media storage, which offers no way to "
                                    + "delete them; without the key they "
                                    + "cannot be viewed.")
                    // Removed and not removed are both counted: a room you
                    // left or a newer version's copy is not removed either.
                    if (s.removal === "partial")
                        return qsTr("%1 removed from your homeserver, %2 could "
                                    + "not be (a room you left, or saved by a "
                                    + "newer Lightning). Your other devices "
                                    + "keep their own copies.")
                               .arg(s.removed).arg(s.removeFailed)
                    if (s.removal === "failed")
                        return qsTr("Nothing could be removed from your "
                                    + "homeserver.")
                    if (s.switchError)
                        return qsTr("Off on this device, but your homeserver "
                                    + "could not be told yet (%1); Lightning "
                                    + "keeps trying.")
                               .arg(section.syncErrorText(s.switchError))
                    return qsTr("Your backgrounds stay on this device only, "
                                + "and are deleted when you sign out. This "
                                + "applies to all your devices. Turning it on "
                                + "again puts the pictures this device has "
                                + "back on your homeserver.")
                default:
                    return ""
                }
            }
        }
        AppButton {
            objectName: "backgroundSyncRetry"
            visible: section.syncState === "failed"
            storm: true
            size: "sm"
            text: qsTr("Retry")
            onClicked: app.backdrops.retrySync()
        }
        // A removal that did not finish is retried as a removal: turning
        // sync on and off would upload this device's pictures first.
        AppButton {
            objectName: "backgroundSyncRetryRemoval"
            visible: section.syncState === "off"
                     && (section.syncStatus.removal === "partial"
                         || section.syncStatus.removal === "failed")
            storm: true
            size: "sm"
            text: qsTr("Retry removal")
            onClicked: app.backdrops.retryRemoval()
        }
    }

    AppDialog {
        id: syncOffDialog
        objectName: "backgroundSyncOffDialog"
        parent: Overlay.overlay
        width: Math.min(520, parent ? parent.width - 64 : 520)
        title: qsTr("Stop keeping backgrounds on your homeserver?")
        // Closed without a choice (Escape, outside): nothing changes.
        standardButtons: Dialog.NoButton

        contentItem: ColumnLayout {
            spacing: AppTheme.spacing16
            Label {
                objectName: "backgroundSyncOffText"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textBody
                text: qsTr("This turns it off on all your devices. Your "
                           + "backgrounds stay on this device until you sign "
                           + "out. You can also remove the copies on your "
                           + "homeserver; your other devices keep the pictures "
                           + "they already have until you remove them there. "
                           + "The encrypted pictures themselves cannot be "
                           + "deleted from the server's media storage, but "
                           + "without the key they cannot be viewed. Turning "
                           + "it on again later puts each device's pictures "
                           + "back on your homeserver.")
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: AppTheme.spacing8
                AppButton {
                    objectName: "backgroundSyncOffCancel"
                    storm: true
                    kind: "secondary"
                    text: qsTr("Cancel")
                    onClicked: syncOffDialog.close()
                }
                AppButton {
                    objectName: "backgroundSyncOffKeep"
                    storm: true
                    kind: "secondary"
                    text: qsTr("Keep server copies")
                    onClicked: {
                        app.backdrops.setSyncEnabled(false, false)
                        syncOffDialog.close()
                    }
                }
                AppButton {
                    objectName: "backgroundSyncOffRemove"
                    storm: true
                    kind: "dangerPrimary"
                    text: qsTr("Remove server copies")
                    onClicked: {
                        app.backdrops.setSyncEnabled(false, true)
                        syncOffDialog.close()
                    }
                }
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: AppTheme.spacing4
        spacing: AppTheme.spacing12
        visible: section.available
        AppSwitch {
            id: showSharedSwitch
            objectName: "showSharedBackgroundsSwitch"
            checked: section.available && app.backdrops.showShared
            onToggled: app.backdrops.showShared = !checked
            Accessible.name: showSharedLabel.text
        }
        Label {
            id: showSharedLabel
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            color: AppTheme.stormText
            font.pixelSize: AppTheme.textBody
            text: qsTr("Show backgrounds set by others")
            TapHandler { onTapped: app.backdrops.showShared = !showSharedSwitch.checked }
        }
    }
    Hint {
        text: qsTr("Rooms and spaces can share a picture with everyone in them. "
                   + "Turn this off to see only your own.")
    }

    GroupLabel {
        objectName: "surfaceDepthHeading"
        text: qsTr("Depth")
    }
    SegmentedControl {
        objectName: "surfaceDepthControl"
        storm: true
        visible: section.available
        model: [
            { label: qsTr("Flat"), value: 0 },
            { label: qsTr("Depth"), value: 1 },
        ]
        current: section.available ? app.backdrops.surfaceDepth : 0
        onActivated: (value) => app.backdrops.surfaceDepth = value
    }
    Hint {
        text: qsTr("Depth gives the conversation, the room list and the spaces "
                   + "rail a soft light-from-above shading. Text never gets "
                   + "harder to read: the shading always moves away from it.")
    }
    // Depth is the automatic gradient; this is the way to one you choose.
    ColumnLayout {
        Layout.fillWidth: true
        spacing: AppTheme.spacing6
        Hint {
            objectName: "surfaceDepthGradientHint"
            Layout.fillWidth: true
            text: qsTr("Want your own colours? A custom theme can give each of "
                       + "these areas a gradient.")
        }
        AppButton {
            objectName: "surfaceDepthMakeGradientButton"
            Layout.leftMargin: AppTheme.spacing4
            storm: true
            size: "sm"
            text: qsTr("Make a gradient…")
            onClicked: section.makeGradientRequested()
        }
    }
}
