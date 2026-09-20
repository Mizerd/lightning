import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

// The account switcher popover, opened from the rail avatar: a header, a
// dense list of account ROWS, one status strip for the active account, and
// the three-button footer. Active account first. Everything load-bearing —
// live-active-account guard, account-switching lockout, both destructive
// confirmations, and the deep links under "Manage" — is preserved exactly.
// No access token, device secret, or local path is ever displayed.
//
// 2026-09-19, reported: "scrolling in it feels kind of dumb since you can
// even scroll about with one account since it doesn't fit". Two things were
// wrong and only one of them was the layout.
//
// THE SCROLL WAS A MEASUREMENT DEFECT. The list was sized from an off-layout
// PROBE of IdentityCard times the model count, because contentHeight stays 0
// until delegates exist while delegates only instantiate inside a nonzero
// viewport. The reasoning was sound and the probe was not: the ACTIVE card
// carries a trust meter and an E2EE badge that NEITHER probe declared, so
// the viewport came out exactly 23 px short of its content at every account
// count — measured at the 296 px content width: short card 113, tall probe
// 136, real active card 159. One account scrolled by 23 px; three clipped
// the last id by 23 px. A probe is only as good as its resemblance.
//
// The fix is not a better probe. Rows are a FIXED height this file owns
// (`rowH`), handed to every delegate, so `contentHeight == count * rowH`
// and the viewport is a multiple of the same number — they cannot disagree,
// and no property added to a row later can make them disagree.
// This is MentionPopup's arrangement, which has always sized itself this
// way for the same reason.
//
// WHAT MOVED, AND WHY IT IS NOT AN INFORMATION LOSS. The meta line, the
// E2EE badge and the trust meter were rendered per row but are only ever
// available for the ACTIVE account (the SDK reports crypto state for the
// attached session alone), so they were a property of one row pretending to
// be a column. They are now one status strip below the list, at constant
// cost instead of per-account cost, and the trust chip is a real affordance
// into the Security & Recovery section it describes.
Popup {
    id: root
    objectName: "accountSwitcherPopover"
    modal: true
    width: 320
    padding: AppTheme.spacing12
    // ── `CloseOnEscape` IS HALF OF A TWO-PART DECLARATION ────────────────
    //
    // `QQuickPopup::keyPressEvent` gates its Escape branch on
    // `hasActiveFocus()`, and a Popup only takes active focus when `focus` is
    // true — which defaults to FALSE. So this popover declared
    // `CloseOnEscape`, looked correct in review, and Escape did nothing:
    // measured on Windows against the published 0.9.8, four presses, zero
    // differing pixels, with a hover/park control proving the screen was
    // live. Press-outside kept working the whole time because the overlay
    // handles that with the mouse, independently of focus, and that asymmetry
    // is exactly what disguised it.
    //
    // Every other root popup in this tree that means it declares both. The
    // two confirmations below are Dialogs, and inherit the same rule: a
    // `focus: true` on their Cancel BUTTON sets focus within the popup's own
    // focus scope, which cannot become ACTIVE focus while the scope itself
    // has none — so Cancel was not focused either, and neither dialog closed
    // on Escape.
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    readonly property bool connected: app.connectionStatus === qsTr("Connected")

    // ONE row height for the whole surface. The delegate takes it, the
    // viewport is a multiple of it, and `contentHeight` is a multiple of it
    // — so the list cannot scroll unless there are genuinely more accounts
    // than fit. Density is the product's person-row ladder (MentionPopup).
    readonly property int rowH: AppTheme.scaled(44)
    // Six rows before the list scrolls, matching MentionPopup's cap. At the
    // common 1-4 accounts nothing scrolls at all.
    readonly property int maxVisibleRows: 6

    // Live crypto state for the ACTIVE account, read ONCE here instead of
    // once per delegate: the SDK only reports it for the account the
    // running client is attached to, so it is not per-row data and must
    // never be fabricated for an inactive account.
    readonly property bool cryptoKnown: app.backendName === "rust"
                                        && !!app.cryptoHealth
    readonly property bool activeE2eeReady: root.cryptoKnown
                                            && app.cryptoHealth.cryptoReady === true
    readonly property bool activeHealthWarning: root.cryptoKnown
                                                && app.cryptoHealth.cryptoError === true
    // Storm §3.4: the trust meter rides ONLY on real crypto state. -1 means
    // "we have no trustworthy answer", and the chip is then absent rather
    // than showing a zero.
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

    // The strip below the list describes the ACTIVE account; with none
    // attached there is nothing it could honestly say.
    readonly property bool hasActiveAccount:
        !!(app.accounts && app.accounts.activeUserId
           && app.accounts.activeUserId.length > 0)

    // ── THE STRIP SPEAKS WHEN SOMETHING IS WRONG, AND OTHERWISE NOT ─────
    //
    // It used to read "Connected · 1 space(s)", and the maintainer's report
    // was that he could not tell what it was for. Both halves earned that:
    //
    // The SPACE COUNT is gone. A switcher answers "which account am I and
    // switch me"; how many Spaces the account is in is not part of either
    // question, and it is on screen in the rail beside it anyway.
    //
    // The CONNECTION STATE stays, but only when it is not healthy. On a
    // working client the string is "Connected", which is the same noise as
    // a readability badge that fires on a stock theme: a line that always
    // says "fine" teaches people not to read it, and then it cannot say
    // "not fine". Worth knowing what the report actually caught — the
    // screenshot read "Idle", which is AppController's word for
    // disconnected-while-logged-in, so the strip WAS reporting a real fault
    // in a word that reads as benign.
    //
    // The own status text still leads when the user set one: that is theirs
    // and they chose to show it.
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
            // Named so a test can assert that what this paints and what an
            // IdentityCard derives its inks against are the SAME colour. The
            // row composites its selection chip onto `hostSurface`, so a
            // popover that changed ground without telling the rows would
            // hand them inks derived for a surface nobody paints.
            objectName: "accountPopoverBackground"
            anchors.fill: parent
            // Storm §4 2c: the switcher sits on the deep canvas so the
            // identity cards (stormPanel / stormInset) read as raised.
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

    // Shared footer action button — Storm §4 2c: three OUTLINE buttons,
    // 33px, radiusTile; Add/Settings ink stormTextSecondary on a
    // stormBorderStrong outline, Sign out carries the danger ink and the
    // 30%-alpha danger outline.
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

        // Spacer-centred: a control stretches its contentItem to the full
        // button width, so anchors.centerIn on the layout is a no-op and the
        // icon+label would sit hard against the pill's left edge.
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

        // Storm §4 2c header: outline bolt + mono ACCOUNTS + bolt Manage
        // link (the surface's one yellow action affordance).
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Icon {
                name: "bolt"
                size: 14
                color: AppTheme.bolt
            }
            // The section-label recipe lives in MenuSectionLabel; this used
            // to be a hand-typed copy of it, which is how the treatment
            // drifts. Reuse the component so restyling it moves every
            // section header in the app at once.
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
                    // stormLink per §1 (inline links) — the 2c mock draws
                    // Manage in bolt, but §1's yellow reserve wins: this
                    // popover already carries the ACTIVE chip, the avatar
                    // ring and the presence dot in bolt.
                    color: AppTheme.stormLink
                    font.family: AppTheme.menuFont
                    // fontMonoXS is the MONO identity-string size; on a UI
                    // face it just rendered this link a pixel smaller than
                    // every other meta label beside it.
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
            // Rows are a contiguous band, like the room list and the
            // mention popup; the rounded selection chip inside each row is
            // what separates them, not a gap.
            spacing: 0
            model: root.sortedAccounts
            // No probe, and deliberately not `contentHeight` either (which
            // stays 0 until delegates exist, while delegates only
            // instantiate inside a nonzero viewport — a permanently-empty
            // list deadlock). Every row is exactly `rowH`, so the count and
            // the row height are all this needs, and content and viewport
            // are the same arithmetic.
            Layout.preferredHeight: {
                var n = cardList.count
                if (n <= 0)
                    return 0
                if (n <= root.maxVisibleRows)
                    return n * root.rowH
                // Past the cap, HALF a row stays visible. AppScrollBar keeps
                // the Basic style's fade contract, so an AsNeeded bar is
                // invisible at rest — the cut row is then the only resting
                // cue that the list continues, and a list that silently
                // ends on a whole row reads as the whole list.
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
                // Live crypto state is only ever available for the ACTIVE
                // row — the SDK only reports it for whichever account the
                // running client is attached to (unchanged rule, read from
                // one place now).
                healthWarning: modelData.isActive === true
                               && root.activeHealthWarning

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

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: AppTheme.stormBorder
            visible: root.hasActiveAccount
        }

        // ── The active account's status strip ────────────────────────────
        // One line, one cost, whatever the account count. It carries what
        // used to be stamped on every card and was only ever true of this
        // one: real presence + real space count on the left, real crypto
        // state on the right. Nothing here is fabricated — when the backend
        // cannot answer, the half that cannot be answered is absent.
        RowLayout {
            id: statusStrip
            objectName: "accountStatusStrip"
            Layout.fillWidth: true
            spacing: AppTheme.spacing6
            // Gated on the ACCOUNT, never on a child's effective `visible`
            // — `Item.visible` folds in the parent's, so a parent reading a
            // child's would be a binding loop.
            visible: root.hasActiveAccount

            Rectangle {
                Layout.alignment: Qt.AlignVCenter
                visible: metaLabel.text.length > 0
                width: 6
                height: 6
                radius: 3
                // Storm: the connected dot is bolt (§4 2h presence idiom);
                // disconnected falls to the muted ink.
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
                // "Connected · 4 spaces" is prose, not an identifier.
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                font.weight: AppTheme.weightMedium
                elide: Label.ElideRight
            }

            // The trust meter used to be a 3 px progress track on every
            // active card. As a chip it costs no height and, unlike the
            // track, it does something: it opens the section that fixes it.
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

    // "Manage" deep links — exact existing aliases (SettingsScreen.qml
    // mapLegacySection remaps "general" -> appearance, "security" -> privacy).
    // v0.9 (phase 10): the personal status editor, hosted here so every
    // entry point (menu item, meta line) opens the same instance.
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
        // THE SCRIM IS NOT THE DEFAULT, AND `modal: true` DOES NOT DRAW ONE
        // HERE. Measured 2026-09-20: three background pixels sampled before
        // and after this dialog opened were byte-identical, because the
        // Basic style's Overlay.modal is a Rectangle tinted from
        // `palette.shadow` and nothing in this application sets that
        // palette role — so a destructive confirmation appeared over a
        // background that still looked live and clickable. The four other
        // dialogs in the tree that got this right (AddWidgetDialog,
        // EventSourceDialog, ExportRoomDialog, DiscoverJoinDialog) all
        // name the token explicitly; these two now do too.
        Overlay.modal: Rectangle { color: AppTheme.modalScrim }
        title: qsTr("Remove account?")
        standardButtons: Dialog.NoButton
        // Both halves, for the reason on the popover above: without `focus`
        // the Dialog never takes active focus, `CloseOnEscape` cannot fire,
        // and the `focus: true` on Cancel below never becomes ACTIVE focus
        // either — so the safe default action was not the focused one.
        focus: true
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorderStrong
            radius: AppTheme.radiusLg
        }

        // Qt Quick Controls Basic renders `title` as a bold default-font
        // Label on a palette.light bar. The background below was themed but
        // the header never was, so this popover's two confirmations carried
        // the one piece of stock chrome left in the file. `title` is kept —
        // it is what the accessibility tree reads.
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
        // THE SCRIM IS NOT THE DEFAULT, AND `modal: true` DOES NOT DRAW ONE
        // HERE. Measured 2026-09-20: three background pixels sampled before
        // and after this dialog opened were byte-identical, because the
        // Basic style's Overlay.modal is a Rectangle tinted from
        // `palette.shadow` and nothing in this application sets that
        // palette role — so a destructive confirmation appeared over a
        // background that still looked live and clickable. The four other
        // dialogs in the tree that got this right (AddWidgetDialog,
        // EventSourceDialog, ExportRoomDialog, DiscoverJoinDialog) all
        // name the token explicitly; these two now do too.
        Overlay.modal: Rectangle { color: AppTheme.modalScrim }
        title: qsTr("Sign out?")
        standardButtons: Dialog.NoButton
        // See removeConfirm above: `CloseOnEscape` needs `focus` to fire, and
        // Cancel cannot hold active focus until this scope does.
        focus: true
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorderStrong
            radius: AppTheme.radiusLg
        }

        // Qt Quick Controls Basic renders `title` as a bold default-font
        // Label on a palette.light bar. The background below was themed but
        // the header never was, so this popover's two confirmations carried
        // the one piece of stock chrome left in the file. `title` is kept —
        // it is what the accessibility tree reads.
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
