import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings and the account menu, for when the Spaces rail is hidden. Both live
// on the rail's bottom cluster, so hiding it ("Show the Spaces rail" off)
// left no pointer route to either. This sits in the room list's workspace
// header and is shown ONLY while the rail is hidden, so the two are never on
// screen twice.
RowLayout {
    id: root
    objectName: "railHiddenActions"
    spacing: AppTheme.spacing4

    visible: app.loggedIn && app.settings
             && !app.settings.spacesRailVisible

    // Same attention rule as the rail's cog: one badge, verification first.
    readonly property bool updateBadge:
        app.updateManager && app.updateManager.updateAvailable
    readonly property string settingsText:
        app.sessionVerificationWarning
            ? qsTr("Settings — this session is not verified")
            : (updateBadge
               ? qsTr("Settings — a Lightning update is available")
               : qsTr("Settings"))

    IconButton {
        id: settingsBtn
        objectName: "headerSettingsButton"
        implicitWidth: 30; implicitHeight: 30
        radius: AppTheme.radiusMd
        iconName: "settings"
        iconSize: 18
        // Accent while in-shell Settings is open; clicking again returns to
        // chat, like the rail's cog.
        active: app.currentScreen === 2
        Accessible.name: root.settingsText
        ToolTip.text: root.settingsText
        ToolTip.visible: hovered
        ToolTip.delay: 500
        onClicked: app.currentScreen === 2 ? app.showMain()
                                           : app.showSettings()

        Rectangle {
            objectName: "headerSettingsAlertBadge"
            visible: app.sessionVerificationWarning || root.updateBadge
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: AppTheme.spacing4
            width: AppTheme.scaled(8)
            height: AppTheme.scaled(8)
            radius: height / 2
            color: app.sessionVerificationWarning ? AppTheme.dangerFill
                                                  : AppTheme.warningFill
            border.color: AppTheme.roomList
            border.width: 1
        }
    }

    // The account tile: the same switcher popover the rail opens.
    Item {
        id: accountTile
        objectName: "headerAccountTile"
        implicitWidth: 30; implicitHeight: 30
        // Keyboard: a Tab stop with a ring, opening the menu on Return, Enter,
        // Space or the Menu key (it was a TapHandler only).
        activeFocusOnTab: true
        Keys.onPressed: (event) => {
            var k = event.key
            if (k === Qt.Key_Return || k === Qt.Key_Enter
                    || k === Qt.Key_Space || k === Qt.Key_Menu) {
                accountMenu.open()
                event.accepted = true
            }
        }
        Rectangle {
            objectName: "headerAccountFocusRing"
            anchors.fill: parent
            anchors.margins: -2
            radius: AppTheme.radiusPill
            color: "transparent"
            border.color: AppTheme.focusRing
            border.width: 2
            visible: accountTile.activeFocus
        }
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("Account menu for %1")
                         .arg(app.accounts ? app.accounts.activeUserId : "")

        // Invokable results do not re-evaluate on signals; refresh on registry
        // or selection changes (see SpacesRail's railAccount).
        property var activeAccount: ({})
        function refreshAccount() {
            activeAccount = app.accounts
                ? app.accounts.account(app.accounts.activeUserId) : ({})
        }
        Component.onCompleted: refreshAccount()
        Connections {
            target: app.accounts
            function onAccountsChanged() { accountTile.refreshAccount() }
            function onActiveUserIdChanged() { accountTile.refreshAccount() }
        }

        Avatar {
            anchors.fill: parent
            size: 30
            circle: true
            name: accountTile.activeAccount.displayName
                  || (app.accounts ? (app.accounts.activeUserId || "") : "")
            mxc: accountTile.activeAccount.avatarUrl || ""
            colorKey: app.accounts ? app.accounts.activeUserId : ""
        }
        HoverHandler { id: accountHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: accountMenu.open() }
        AccountWheelSwitch {
            objectName: "hiddenRailAccountWheel"
            accounts: app.accounts ? app.accounts.accounts : []
            activeUserId: app.accounts ? (app.accounts.activeUserId || "") : ""
            busy: app.accountSwitching
                  || !!(app.auth && app.auth.logoutPending)
            onSwitchRequested: (userId) => app.switchToAccount(userId)
        }
        ToolTip.visible: accountHover.hovered
        ToolTip.text: app.accounts ? (app.accounts.activeUserId || "") : ""
        ToolTip.delay: 500

        AccountMenu {
            id: accountMenu
            // Opens down and to the left of the header's right edge.
            x: accountTile.width - width
            y: accountTile.height + AppTheme.spacing4
            margins: AppTheme.spacing12
        }
    }
}
