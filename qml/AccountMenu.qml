import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

// v0.6.5 (SPEC 1h, modified — vertical identity cards): the account
// switcher popover, opened from the rail avatar. A vertical stack of
// IdentityCard rows (active account first, real presence/space/E2EE data
// only) replaces the old single-header + list layout; everything else —
// live-active-account guard, account-switching lockout, both destructive
// confirmations, and the deep links under "Manage" — is preserved exactly.
// No access token, device secret, or local path is ever displayed.
Popup {
    id: root
    objectName: "accountSwitcherPopover"
    modal: true
    width: 320
    padding: AppTheme.spacing12
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    readonly property bool connected: app.connectionStatus === qsTr("Connected")

    // Real presence + real space count for the ACTIVE card only — never
    // fabricated, and the space-count portion is omitted entirely when
    // there are no real Spaces (SpaceManager::spaceCount counts only real
    // joined Spaces, never the pseudo Home/orphans rows).
    function activeMetaText() {
        var parts = [app.connectionStatus]
        if (app.spaces && app.spaces.spaceCount > 0) {
            parts.push(app.spaces.spaceCount === 1
                       ? qsTr("1 space")
                       : qsTr("%1 spaces").arg(app.spaces.spaceCount))
        }
        return parts.join(" · ")
    }

    // Active account first, regardless of the underlying storage order —
    // matches the old layout's dedicated active-account header, just
    // expressed as a stack ordering instead of a separate block.
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

    // Sanctioned shadow — overlay-anchored popovers stay on the design's
    // shadow budget (R3). The effect and its source Rectangle must be
    // SIBLINGS (MultiEffect cannot anchor across the Popup background
    // boundary), so both live inside one background Item.
    background: Item {
        Rectangle {
            id: popoverBackground
            anchors.fill: parent
            color: AppTheme.cardElevated
            border.color: AppTheme.borderStrong
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

    // Shared footer action button: icon + 12px/700 label, 34px tall,
    // radiusTile. Add/Settings are neutral (hover bg); Sign out carries a
    // permanent danger tint.
    component FooterAction: AbstractButton {
        id: footerBtn
        property string iconName: ""
        property color inkColor: AppTheme.textSecondary
        property color restColor: "transparent"
        property color hoverColor: AppTheme.hover

        Layout.fillWidth: true
        implicitHeight: 34
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        Accessible.role: Accessible.Button
        Accessible.name: footerBtn.text

        // Spacer-centred: a control stretches its contentItem to the full
        // button width, so anchors.centerIn on the layout is a no-op and the
        // icon+label would sit hard against the pill's left edge.
        contentItem: RowLayout {
            spacing: AppTheme.spacing4
            Item { Layout.fillWidth: true }
            Icon { name: footerBtn.iconName; size: 16; color: footerBtn.inkColor }
            Label {
                text: footerBtn.text
                color: footerBtn.inkColor
                font.pixelSize: 12
                font.weight: Font.Bold
            }
            Item { Layout.fillWidth: true }
        }
        background: Rectangle {
            radius: AppTheme.radiusTile
            color: footerBtn.enabled && (footerBtn.down || footerBtn.hovered)
                   ? footerBtn.hoverColor : footerBtn.restColor
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: AppTheme.radiusTile + 3
            color: "transparent"
            border.width: 2
            border.color: AppTheme.focusRing
            visible: footerBtn.visualFocus
        }
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing10

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Label {
                text: qsTr("Accounts")
                color: AppTheme.textPrimary
                font.pixelSize: AppTheme.fontBody
                font.weight: Font.ExtraBold
            }
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
                    color: AppTheme.accent
                    font.pixelSize: AppTheme.fontMonoXS
                    font.weight: Font.DemiBold
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
                    border.color: AppTheme.focusRing
                    border.width: 2
                    visible: manageLabel.visualFocus
                }
            }
        }

        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.separator }

        // Off-screen probe: real IdentityCard geometry drives the "cap the
        // stack at ~3 rows" height, instead of a guessed magic number. It
        // needs a real width (invisible items get none from the layout) so
        // its implicit height is measured at the width the cards render at.
        IdentityCard {
            id: heightProbe
            objectName: "identityCardHeightProbe"
            visible: false
            width: root.availableWidth
            displayName: "Probe"
            userId: "@probe:example.org"
        }
        // The ACTIVE card is taller by exactly its meta row — measure that
        // variant too, or the 3-account case clips the last card by that
        // difference (active-first ordering guarantees rows = 1 tall +
        // rest short).
        IdentityCard {
            id: tallHeightProbe
            objectName: "identityCardTallHeightProbe"
            visible: false
            width: root.availableWidth
            displayName: "Probe"
            userId: "@probe:example.org"
            metaText: "probe"
            connected: true
        }

        ListView {
            id: cardList
            objectName: "identityCardList"
            Layout.fillWidth: true
            clip: true
            spacing: AppTheme.spacing10
            model: root.sortedAccounts
            // Height from the MODEL COUNT and the measured probe — never
            // from contentHeight, which stays 0 until delegates exist while
            // delegates only instantiate inside a nonzero viewport (a
            // permanently-empty-list deadlock).
            Layout.preferredHeight: {
                var rows = Math.min(cardList.count, 3)
                if (rows <= 0)
                    return 0
                return tallHeightProbe.implicitHeight
                       + (rows - 1) * heightProbe.implicitHeight
                       + (rows - 1) * AppTheme.spacing10
            }
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: IdentityCard {
                id: card
                required property var modelData
                objectName: "identityCard_" + (modelData.userId || "")
                width: cardList.width
                enabled: !app.accountSwitching

                active: modelData.isActive === true
                displayName: modelData.displayName || ""
                userId: modelData.userId || ""
                avatarMxc: modelData.avatarUrl || ""
                needsSignIn: modelData.needsSignIn === true
                // healthWarning / e2eeReady: live crypto state is only ever
                // available for the ACTIVE row — the SDK only reports it
                // for whichever account the running client is attached to
                // (unchanged from the previous per-row rule).
                healthWarning: modelData.isActive === true
                              && app.backendName === "rust"
                              && app.cryptoHealth
                              && app.cryptoHealth.cryptoError === true
                e2eeReady: modelData.isActive === true
                          && app.backendName === "rust"
                          && app.cryptoHealth
                          && app.cryptoHealth.cryptoReady === true
                connected: root.connected
                metaText: modelData.isActive === true ? root.activeMetaText() : ""

                onActivated: {
                    // Compare against the LIVE active account, never the
                    // row snapshot: a stale delegate must not be able to
                    // swallow a legitimate switch request.
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

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            FooterAction {
                objectName: "accountFooterAdd"
                text: qsTr("Add")
                iconName: "person_add"
                restColor: AppTheme.hover
                enabled: !app.accountSwitching
                onClicked: { root.close(); app.showLogin() }
            }
            FooterAction {
                objectName: "accountFooterSettings"
                text: qsTr("Settings")
                iconName: "settings"
                restColor: AppTheme.hover
                enabled: !app.accountSwitching
                onClicked: { root.close(); app.showSettingsSection("general") }
            }
            FooterAction {
                objectName: "accountFooterSignOut"
                text: qsTr("Sign out")
                iconName: "logout"
                inkColor: AppTheme.dangerInk
                restColor: AppTheme.dangerSoft
                hoverColor: Qt.alpha(AppTheme.mentionBadge, 0.16)
                enabled: !app.accountSwitching
                onClicked: { root.close(); signOutConfirm.open() }
            }
        }
    }

    // "Manage" deep links — exact existing aliases (SettingsScreen.qml
    // mapLegacySection remaps "general" -> appearance, "security" -> privacy).
    AppMenu {
        id: manageMenu
        objectName: "accountManageMenu"
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

    // Remove-account confirmation — names exactly which account is removed;
    // Cancel is focused and the default safe action. Only that account's
    // local session, store, and token are deleted.
    Dialog {
        id: removeConfirm
        objectName: "removeAccountConfirmDialog"
        property string targetUserId: ""
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.max(240, Math.min(420, parent ? parent.width - 32 : 420))
        modal: true
        title: qsTr("Remove account?")
        standardButtons: Dialog.NoButton
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            color: AppTheme.surface
            border.color: AppTheme.border
            radius: AppTheme.radiusLg
        }

        contentItem: ColumnLayout {
            spacing: AppTheme.spacing12
            Label {
                Layout.fillWidth: true
                text: qsTr("Remove %1 from this device? Its local Lightning "
                           + "data, encryption store, and sign-in are deleted "
                           + "from this computer only. Messages stay on the "
                           + "server, and other accounts are not affected.")
                      .arg(removeConfirm.targetUserId)
                wrapMode: Text.WordWrap
                color: AppTheme.textPrimary
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button {
                    text: qsTr("Cancel")
                    focus: true
                    onClicked: removeConfirm.close()
                }
                Button {
                    text: qsTr("Remove")
                    Accessible.name: qsTr("Confirm account removal")
                    contentItem: Label {
                        text: parent.text
                        color: AppTheme.dangerText
                        horizontalAlignment: Text.AlignHCenter
                    }
                    background: Rectangle {
                        color: parent.down ? Qt.darker(AppTheme.danger, 1.2)
                                           : AppTheme.danger
                        radius: AppTheme.radiusSm
                    }
                    onClicked: {
                        var target = removeConfirm.targetUserId
                        removeConfirm.close()
                        app.removeAccount(target)
                    }
                }
            }
        }
    }

    // Confirmation — Cancel is focused and the default safe action.
    Dialog {
        id: signOutConfirm
        objectName: "signOutConfirmDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        // Bound to the overlay, not to content implicitWidth: the content
        // can wrap without feeding its preferred size back into the Dialog.
        width: Math.max(240, Math.min(420, parent ? parent.width - 32 : 420))
        modal: true
        title: qsTr("Sign out?")
        standardButtons: Dialog.NoButton
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            color: AppTheme.surface
            border.color: AppTheme.border
            radius: AppTheme.radiusLg
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
                wrapMode: Text.WordWrap
                color: AppTheme.textPrimary
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button {
                    text: qsTr("Cancel")
                    focus: true
                    onClicked: signOutConfirm.close()
                }
                Button {
                    text: qsTr("Sign out")
                    Accessible.name: qsTr("Confirm sign out")
                    contentItem: Label {
                        text: parent.text
                        color: AppTheme.dangerText
                        horizontalAlignment: Text.AlignHCenter
                    }
                    background: Rectangle {
                        color: parent.down ? Qt.darker(AppTheme.danger, 1.2)
                                           : AppTheme.danger
                        radius: AppTheme.radiusSm
                    }
                    onClicked: {
                        signOutConfirm.close()
                        app.auth.logout()
                    }
                }
            }
        }
    }
}
