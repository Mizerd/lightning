import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

// The account switcher popover, opened from the rail avatar: a header, a
// dense list of account rows (active first), one status strip for the active
// account, and a three-button footer. No access token, device secret or local
// path is ever displayed.
//
// Rows have a fixed height owned here (`rowH`), so the list's content height
// and viewport are the same arithmetic and it only scrolls when there are
// more accounts than fit (as in MentionPopup). Crypto state (trust meter,
// E2EE badge) is only known for the active account, so it lives in the
// status strip rather than on each row.
Popup {
    id: root
    objectName: "accountSwitcherPopover"
    modal: true
    width: 320
    padding: AppTheme.spacing12
    // `CloseOnEscape` needs `focus: true`: QQuickPopup only handles Escape with
    // active focus, and a Popup's focus defaults to false. The same applies to
    // the two Dialogs below, whose Cancel buttons can't hold active focus
    // otherwise.
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    readonly property bool connected: app.connectionStatus === qsTr("Connected")

    // One row height for the whole surface; the delegate, viewport and content
    // height all derive from it.
    readonly property int rowH: AppTheme.scaled(44)
    // Six rows before the list scrolls, as in MentionPopup.
    readonly property int maxVisibleRows: 6

    // Crypto state for the active account, read once: the SDK only reports it
    // for the attached account, so it must never be shown for an inactive one.
    readonly property bool cryptoKnown: app.backendName === "rust"
                                        && !!app.cryptoHealth
    readonly property bool activeE2eeReady: root.cryptoKnown
                                            && app.cryptoHealth.cryptoReady === true
    readonly property bool activeHealthWarning: root.cryptoKnown
                                                && app.cryptoHealth.cryptoError === true
    // The trust meter uses real crypto state only; -1 means no trustworthy
    // answer, and the chip is then hidden.
    readonly property int activeTrustCompleted: {
        if (!root.cryptoKnown || !app.cryptoHealth.cryptoSupported)
            return -1
        var n = 0
        if (app.cryptoHealth.ownIdentityVerified === CryptoHealthModel.Yes) n++
        if (app.cryptoHealth.currentDeviceVerified === CryptoHealthModel.Yes) n++
        if (app.cryptoHealth.crossSigningReady === true) n++
        return n
    }
    readonly property int activeTrustTotal: 3

    // The strip describes the active account; with none attached it is hidden.
    readonly property bool hasActiveAccount:
        !!(app.accounts && app.accounts.activeUserId
           && app.accounts.activeUserId.length > 0)

    // ── The strip only speaks when something is wrong ──
    // The connection state appears only when unhealthy ("Idle" means
    // disconnected while logged in): a line that always says "Connected" gets
    // ignored. The user's own status text leads when set.
    readonly property string healthyConnectionText: qsTr("Connected")
    function activeMetaText() {
        var parts = []
        if (app.presence && app.presence.ownStatusText.length > 0)
            parts.push(app.presence.ownStatusText)
        var conn = app.connectionStatus || ""
        if (conn.length > 0 && conn !== root.healthyConnectionText)
            parts.push(conn)
        return parts.join(" · ")
    }

    // Active account first, regardless of storage order.
    readonly property var sortedAccounts: {
        var list = app.accounts ? app.accounts.accounts : []
        var activeIndex = -1
        for (var i = 0; i < list.length; i++) {
            if (list[i].isActive === true) { activeIndex = i; break }
        }
        if (activeIndex > 0)
            return [list[activeIndex]].concat(list.slice(0, activeIndex),
                                              list.slice(activeIndex + 1))
        return list
    }

    // Popover shadow. MultiEffect and its source must be siblings, so both
    // live in one background Item.
    background: Item {
        Rectangle {
            id: popoverBackground
            // Named so a test can assert rows derive their inks against the
            // colour this actually paints (IdentityCard's `hostSurface`).
            objectName: "accountPopoverBackground"
            anchors.fill: parent
            // The deep canvas, so the identity rows read as raised.
            color: AppTheme.stormCanvas
            border.color: AppTheme.stormBorder
            border.width: 1
            radius: AppTheme.radiusLg
        }
        MultiEffect {
            source: popoverBackground
            anchors.fill: popoverBackground
            z: -1
            shadowEnabled: true
            shadowColor: AppTheme.shadow
            shadowBlur: 0.6
            shadowVerticalOffset: 2
            shadowHorizontalOffset: 0
        }
    }

    // Footer action: an outline button. Add/Settings use the secondary ink;
    // Sign out uses the danger ink and outline.
    component FooterAction: AbstractButton {
        id: footerBtn
        property string iconName: ""
        property bool dangerAction: false

        readonly property color _ink:
            dangerAction ? AppTheme.stormDanger : AppTheme.stormTextSecondary

        Layout.fillWidth: true
        implicitHeight: 33
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        Accessible.role: Accessible.Button
        Accessible.name: footerBtn.text

        // Spacer-centred: the contentItem is stretched to the button width, so
        // centerIn would be a no-op.
        contentItem: RowLayout {
            spacing: AppTheme.spacing4
            Item { Layout.fillWidth: true }
            Icon { name: footerBtn.iconName; size: 15; color: footerBtn._ink }
            Label {
                text: footerBtn.text
                color: footerBtn._ink
                font.family: AppTheme.menuFont
                font.pixelSize: AppTheme.textMeta
                font.weight: AppTheme.weightStrong
            }
            Item { Layout.fillWidth: true }
        }
        background: Rectangle {
            radius: AppTheme.radiusTile
            color: footerBtn.enabled && (footerBtn.down || footerBtn.hovered)
                   ? (footerBtn.dangerAction ? AppTheme.stormDangerSoft
                                             : AppTheme.stormSelection)
                   : "transparent"
            border.width: 1
            border.color: footerBtn.dangerAction ? AppTheme.stormDangerBorder
                                                 : AppTheme.stormBorderStrong
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: AppTheme.radiusTile + 3
            color: "transparent"
            border.width: 2
            border.color: AppTheme.bolt
            visible: footerBtn.visualFocus
        }
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing10

        // Header: bolt icon, "Accounts" label and the Manage link.
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Icon {
                name: "bolt"
                size: 14
                color: AppTheme.bolt
            }
            // Reuse MenuSectionLabel so section headers restyle together.
            MenuSectionLabel { text: qsTr("Accounts") }
            Item { Layout.fillWidth: true }
            AbstractButton {
                id: manageLabel
                objectName: "accountManageLink"
                text: qsTr("Manage")
                implicitWidth: manageInk.implicitWidth + AppTheme.spacing4
                implicitHeight: manageInk.implicitHeight + AppTheme.spacing4
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Manage account settings")
                onClicked: manageMenu.popup(manageLabel, 0,
                                            manageLabel.height
                                            + AppTheme.spacing4)
                contentItem: Label {
                    id: manageInk
                    text: manageLabel.text
                    // stormLink rather than bolt: this popover already uses
                    // bolt for the active chip, avatar ring and presence dot.
                    color: AppTheme.stormLink
                    font.family: AppTheme.menuFont
                    // Meta size, matching the labels beside it.
                    font.pixelSize: AppTheme.textMeta
                    font.weight: AppTheme.weightStrong
                    font.underline: manageLabel.hovered
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Item { }
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -2
                    radius: AppTheme.radiusSm
                    color: "transparent"
                    border.color: AppTheme.bolt
                    border.width: 2
                    visible: manageLabel.visualFocus
                }
            }
        }

        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.stormBorder }

        ListView {
            id: cardList
            objectName: "identityCardList"
            Layout.fillWidth: true
            clip: true
            // A contiguous band; each row's selection chip separates them.
            spacing: 0
            model: root.sortedAccounts
            // From the count, not contentHeight (0 until delegates exist, and
            // delegates need a nonzero viewport). Rows are exactly `rowH`.
            Layout.preferredHeight: {
                var n = cardList.count
                if (n <= 0)
                    return 0
                if (n <= root.maxVisibleRows)
                    return n * root.rowH
                // Past the cap, half a row stays visible: the AsNeeded bar is
                // invisible at rest, so the cut row is the cue that it scrolls.
                return Math.round((root.maxVisibleRows + 0.5) * root.rowH)
            }
            ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }

            delegate: IdentityCard {
                id: card
                required property var modelData
                objectName: "identityCard_" + (modelData.userId || "")
                width: cardList.width
                rowHeight: root.rowH
                enabled: !app.accountSwitching

                active: modelData.isActive === true
                displayName: modelData.displayName || ""
                userId: modelData.userId || ""
                avatarMxc: modelData.avatarUrl || ""
                needsSignIn: modelData.needsSignIn === true
                // Crypto state only for the active row.
                healthWarning: modelData.isActive === true
                               && root.activeHealthWarning

                onActivated: {
                    // Compare against the live active account, not the row
                    // snapshot, so a stale delegate can't swallow a real
                    // switch.
                    if ((modelData.userId || "")
                            === (app.accounts ? app.accounts.activeUserId : ""))
                        return
                    root.close()
                    app.switchToAccount(modelData.userId)
                }
                onRemoveRequested: {
                    removeConfirm.targetUserId = modelData.userId
                    root.close()
                    removeConfirm.open()
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: AppTheme.stormBorder
            visible: root.hasActiveAccount
        }

        // ── The active account's status strip ──
        // Presence on the left, crypto state on the right. Whatever the backend
        // can't answer is absent, never fabricated.
        RowLayout {
            id: statusStrip
            objectName: "accountStatusStrip"
            Layout.fillWidth: true
            spacing: AppTheme.spacing6
            // Gated on the account, not a child's `visible` (which folds in the
            // parent's and would loop).
            visible: root.hasActiveAccount

            Rectangle {
                Layout.alignment: Qt.AlignVCenter
                visible: metaLabel.text.length > 0
                width: 6
                height: 6
                radius: 3
                // Bolt when connected, muted otherwise.
                color: root.connected ? AppTheme.bolt : AppTheme.stormTextMuted
            }
            Label {
                id: metaLabel
                objectName: "accountStatusMeta"
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                // Remote or externally chosen text: never markup.
                textFormat: Text.PlainText
                text: root.activeMetaText()
                color: AppTheme.stormTextMuted
                // Prose, not an identifier.
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                font.weight: AppTheme.weightMedium
                elide: Label.ElideRight
            }

            // Trust as a chip that opens Security & Recovery.
            AbstractButton {
                id: trustChip
                objectName: "accountTrustChip"
                Layout.alignment: Qt.AlignVCenter
                visible: root.activeE2eeReady || root.activeTrustCompleted >= 0
                readonly property bool full:
                    root.activeTrustCompleted >= root.activeTrustTotal
                readonly property color ink:
                    full && root.activeE2eeReady ? AppTheme.stormSuccess
                                                 : AppTheme.stormTextMuted
                implicitWidth: trustInk.implicitWidth + AppTheme.spacing8
                implicitHeight: trustInk.implicitHeight + AppTheme.spacing4
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                Accessible.role: Accessible.Button
                Accessible.name: root.activeTrustCompleted >= 0
                    ? qsTr("Encryption trust %1 of %2. Open Security and Recovery.")
                      .arg(root.activeTrustCompleted).arg(root.activeTrustTotal)
                    : qsTr("Encryption ready. Open Security and Recovery.")
                onClicked: { root.close(); app.showSettingsSection("security") }

                background: Rectangle {
                    radius: AppTheme.radiusSm
                    color: trustChip.hovered ? AppTheme.stormSelection
                                             : "transparent"
                }
                contentItem: RowLayout {
                    id: trustInk
                    spacing: 3
                    Icon {
                        name: trustChip.full ? "verified_user" : "shield"
                        size: AppTheme.scaled(AppTheme.textMeta)
                        color: trustChip.ink
                    }
                    Label {
                        text: qsTr("E2EE")
                        color: trustChip.ink
                        font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                        font.weight: AppTheme.weightBold
                    }
                    Label {
                        objectName: "accountTrustCount"
                        visible: root.activeTrustCompleted >= 0
                        text: "%1/%2".arg(root.activeTrustCompleted)
                                     .arg(root.activeTrustTotal)
                        color: trustChip.ink
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                        font.weight: AppTheme.weightBold
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -2
                    radius: AppTheme.radiusSm + 2
                    color: "transparent"
                    border.color: AppTheme.bolt
                    border.width: 2
                    visible: trustChip.visualFocus
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            FooterAction {
                objectName: "accountFooterAdd"
                text: qsTr("Add")
                iconName: "person_add"
                enabled: !app.accountSwitching
                onClicked: { root.close(); app.showLogin() }
            }
            FooterAction {
                objectName: "accountFooterSettings"
                text: qsTr("Settings")
                iconName: "settings"
                enabled: !app.accountSwitching
                onClicked: { root.close(); app.showSettingsSection("general") }
            }
            FooterAction {
                objectName: "accountFooterSignOut"
                text: qsTr("Sign out")
                iconName: "logout"
                dangerAction: true
                enabled: !app.accountSwitching
                onClicked: { root.close(); signOutConfirm.open() }
            }
        }
    }

    // "Manage" deep links, using SettingsScreen.qml's section aliases
    // (mapLegacySection). The status editor is hosted here so every entry
    // point opens the same instance.
    StatusDialog { id: statusDialog }
    AppMenu {
        id: manageMenu
        objectName: "accountManageMenu"
        AppMenuItem {
            objectName: "accountSetStatusItem"
            text: app.presence && app.presence.ownStatusText.length > 0
                  ? qsTr("Edit status…") : qsTr("Set a status…")
            iconName: "mood"
            enabled: app.presence && app.presence.supported
            onTriggered: { root.close(); statusDialog.openForEdit() }
        }
        AppMenuItem {
            text: qsTr("Settings")
            iconName: "settings"
            onTriggered: { root.close(); app.showSettingsSection("general") }
        }
        AppMenuItem {
            text: qsTr("Security & Recovery")
            iconName: "verified_user"
            onTriggered: { root.close(); app.showSettingsSection("security") }
        }
        AppMenuItem {
            text: qsTr("About Lightning")
            iconName: "info"
            onTriggered: { root.close(); app.showSettingsSection("about") }
        }
    }

    // Remove-account confirmation: names the exact account, Cancel is the
    // focused default. Only that account's local session, store and token are
    // deleted.
    Dialog {
        id: removeConfirm
        objectName: "removeAccountConfirmDialog"
        property string targetUserId: ""
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.max(240, Math.min(420, parent ? parent.width - 32 : 420))
        modal: true
        // An explicit scrim: Basic's Overlay.modal uses palette.shadow, which
        // the app doesn't set, so `modal: true` alone draws nothing.
        Overlay.modal: Rectangle { color: AppTheme.modalScrim }
        title: qsTr("Remove account?")
        standardButtons: Dialog.NoButton
        // `focus` for CloseOnEscape and Cancel's default focus (see the popover
        // above).
        focus: true
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorderStrong
            radius: AppTheme.radiusLg
        }

        // Replace Basic's stock title bar. `title` stays for accessibility.
        header: Label {
            text: removeConfirm.title
            color: AppTheme.stormText
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightBold
            elide: Label.ElideRight
            leftPadding: AppTheme.spacing16
            rightPadding: AppTheme.spacing16
            topPadding: AppTheme.spacing16
            bottomPadding: AppTheme.spacing8
        }

        contentItem: ColumnLayout {
            spacing: AppTheme.spacing12
            Label {
                // Remote or externally chosen text: never markup.
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: qsTr("Remove %1 from this device? Its local Lightning "
                           + "data, encryption store, and sign-in are deleted "
                           + "from this computer only. Messages stay on the "
                           + "server, and other accounts are not affected.")
                      .arg(removeConfirm.targetUserId)
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                wrapMode: Text.WordWrap
                color: AppTheme.stormText
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                AppButton {
                    storm: true
                    text: qsTr("Cancel")
                    focus: true
                    onClicked: removeConfirm.close()
                }
                AppButton {
                    storm: true
                    kind: "danger"
                    text: qsTr("Remove")
                    Accessible.name: qsTr("Confirm account removal")
                    onClicked: {
                        var target = removeConfirm.targetUserId
                        removeConfirm.close()
                        app.removeAccount(target)
                    }
                }
            }
        }
    }

    // Sign-out confirmation; Cancel is the focused default.
    Dialog {
        id: signOutConfirm
        objectName: "signOutConfirmDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        // Bound to the overlay, not content implicitWidth, so wrapping content
        // doesn't feed back into the Dialog width.
        width: Math.max(240, Math.min(420, parent ? parent.width - 32 : 420))
        modal: true
        // An explicit scrim (see removeConfirm).
        Overlay.modal: Rectangle { color: AppTheme.modalScrim }
        title: qsTr("Sign out?")
        standardButtons: Dialog.NoButton
        // See removeConfirm: `focus` for CloseOnEscape and Cancel's focus.
        focus: true
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorderStrong
            radius: AppTheme.radiusLg
        }

        // Replace Basic's stock title bar. `title` stays for accessibility.
        header: Label {
            text: signOutConfirm.title
            color: AppTheme.stormText
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightBold
            elide: Label.ElideRight
            leftPadding: AppTheme.spacing16
            rightPadding: AppTheme.spacing16
            topPadding: AppTheme.spacing16
            bottomPadding: AppTheme.spacing8
        }

        contentItem: ColumnLayout {
            spacing: AppTheme.spacing12
            Label {
                Layout.fillWidth: true
                text: qsTr("You will be signed out of this session. "
                           + "Lightning's local data for this account is "
                           + "removed from this computer; your messages stay "
                           + "on the server, and encrypted history may need "
                           + "your recovery key after the next sign-in.")
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                wrapMode: Text.WordWrap
                color: AppTheme.stormText
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                AppButton {
                    storm: true
                    text: qsTr("Cancel")
                    focus: true
                    onClicked: signOutConfirm.close()
                }
                AppButton {
                    storm: true
                    kind: "danger"
                    text: qsTr("Sign out")
                    Accessible.name: qsTr("Confirm sign out")
                    onClicked: {
                        signOutConfirm.close()
                        app.auth.logout()
                    }
                }
            }
        }
    }
}
