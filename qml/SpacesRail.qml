import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.7 design shell: the far-left rail (68 px). Top-to-bottom: Home ("all
// rooms"), Space avatars (40×40, radius 12, active = accent outline), then a
// bottom cluster with Settings and the account avatar that opens the account
// switcher popover. The rail is always visible — it is the primary
// navigation column, not a Spaces-only affordance.
Rectangle {
    id: root
    color: AppTheme.rail

    // Emitted by the Add Space tile below the Space list; MainScreen routes
    // it into the creation dialog's Space mode.
    signal createSpaceRequested()

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: AppTheme.spacing12 + 2
        anchors.bottomMargin: AppTheme.spacing12
        spacing: 0

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: app.spaces
            clip: true
            spacing: AppTheme.spacing4

            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Item {
                id: spaceItem
                width: list.width
                // 48 = 40px tile + 4px on each side so the active accent
                // outline (drawn at -4px margins) is never clipped by the
                // list bounds — this was the Home-icon clipping defect.
                // Home carries the handoff divider (32×2) below its tile.
                height: isHome ? 58 : 48

                property bool isActive: app.spaces
                                        && app.spaces.activeSpaceId === model.spaceId
                property bool isPseudo: model.spaceId === ""
                                        || model.spaceId === "@orphans"
                property bool isHome: model.spaceId === ""

                Accessible.role: Accessible.Button
                Accessible.name: isHome ? qsTr("All rooms")
                                 : model.spaceId === "@orphans"
                                   ? qsTr("Other rooms") : (model.name || "")

                // Active outline: 2 px accent ring offset from the tile.
                Rectangle {
                    anchors.fill: spaceTile
                    anchors.margins: -4
                    radius: AppTheme.radiusLg + 3
                    color: "transparent"
                    border.color: AppTheme.accent
                    border.width: 2
                    visible: spaceItem.isActive
                }

                // Handoff divider between Home and the Space tiles (32×2).
                Rectangle {
                    visible: spaceItem.isHome
                    width: 32; height: 2; radius: 2
                    color: AppTheme.border
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 2
                }

                // Nested sub-space: a subtle indent + connector notch so
                // the rail mirrors the space hierarchy ("Land of the
                // Insane" layout, 2026-08-18) — level was computed by
                // SpaceManager all along and rendered nowhere.
                Rectangle {
                    visible: !spaceItem.isPseudo && (model.level || 0) > 0
                    width: 6; height: 2; radius: 1
                    color: AppTheme.border
                    anchors.verticalCenter: spaceTile.verticalCenter
                    anchors.right: spaceTile.left
                    anchors.rightMargin: 1
                }
                Rectangle {
                    id: spaceTile
                    width: 40; height: 40
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.horizontalCenterOffset:
                        !spaceItem.isPseudo && (model.level || 0) > 0 ? 5 : 0
                    y: 4
                    radius: AppTheme.radiusLg
                    // Home is a persistent accent tile per the handoff; the
                    // pseudo "other rooms" tile stays neutral; real Spaces
                    // paint through the shared Avatar below.
                    color: spaceItem.isHome ? AppTheme.accent
                           : spaceItem.isPseudo ? AppTheme.cardElevated
                                                : "transparent"

                    Behavior on color { ColorAnimation { duration: 120 } }

                    // Pseudo rows use monochrome icons; real Spaces show
                    // palette initials until the avatar loads.
                    Icon {
                        anchors.centerIn: parent
                        visible: spaceItem.isPseudo
                        name: spaceItem.isHome ? "home" : "workspaces"
                        size: spaceItem.isHome ? 22 : 20
                        color: spaceItem.isHome ? AppTheme.accentText
                                                : AppTheme.textSecondary
                    }

                    // Real Space avatar (palette initials fallback) via the
                    // shared Avatar element — mediaCached wiring and the
                    // baked "|shape:" mask both live there.
                    Avatar {
                        anchors.fill: parent
                        visible: !spaceItem.isPseudo
                        size: 40
                        circle: false
                        squareRadius: AppTheme.radiusLg
                        labelSize: 15
                        name: spaceItem.isPseudo ? "" : (model.name || "")
                        colorKey: spaceItem.isPseudo ? "" : (model.spaceId || "")
                        mxc: spaceItem.isPseudo ? "" : (model.avatarUrl || "")
                    }

                    // Unread count badge (rail-coloured ring per design).
                    Rectangle {
                        visible: model.unreadTotal > 0 && !spaceItem.isActive
                        width: Math.max(18, badgeLabel.implicitWidth + 6)
                        height: 18
                        radius: 9
                        color: model.highlightTotal > 0 ? AppTheme.mentionBadge
                                                        : AppTheme.unreadBadge
                        border.color: AppTheme.rail
                        border.width: 2
                        anchors.top: parent.top
                        anchors.right: parent.right
                        anchors.topMargin: -5
                        anchors.rightMargin: -5

                        Label {
                            id: badgeLabel
                            anchors.centerIn: parent
                            text: model.unreadTotal > 99
                                  ? "99+" : model.unreadTotal.toString()
                            font.pixelSize: 10
                            font.weight: Font.ExtraBold
                            color: model.highlightTotal > 0 ? AppTheme.dangerText
                                                            : AppTheme.accentText
                        }
                    }
                }

                HoverHandler { id: spaceHover }
                Rectangle {
                    anchors.fill: spaceTile
                    anchors.margins: -3
                    radius: spaceTile.radius + 3
                    color: AppTheme.hover
                    visible: spaceHover.hovered && !spaceItem.isActive
                    z: -1
                }

                TapHandler {
                    // Single tap filters the room list to the Space;
                    // double tap additionally opens the Space Home
                    // overview (the sub-room front page). Only REAL
                    // Spaces have an overview — a double-tap on the
                    // Home/"Other rooms" pseudo tiles must not tear
                    // down the open room for a surface that does not
                    // exist (review L3).
                    onTapped: if (app.spaces) app.spaces.activeSpaceId = model.spaceId
                    onDoubleTapped: {
                        if (model.spaceId.length > 0
                                && model.spaceId.charAt(0) === "!")
                            app.openSpaceHome(model.spaceId)
                    }
                }

                ToolTip {
                    visible: spaceHover.hovered
                    text: spaceItem.Accessible.name
                    delay: 500
                }
            }

            // Add Space: part of the list CONTENT, so it sits directly
            // below the last Space tile, scrolls with the tiles, and never
            // overlaps the pinned Settings/account cluster.
            footer: Item {
                width: list.width
                height: railAddSpaceButton.visible ? 48 : 0
                IconButton {
                    id: railAddSpaceButton
                    objectName: "railAddSpaceButton"
                    y: 4
                    anchors.horizontalCenter: parent.horizontalCenter
                    implicitWidth: 40; implicitHeight: 40
                    radius: AppTheme.radiusLg
                    iconName: "add"
                    iconSize: 22
                    visible: app.loggedIn && app.conversations
                             && app.conversations.supported
                    Accessible.name: qsTr("Create a Space")
                    ToolTip.text: qsTr("Create a Space")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: root.createSpaceRequested()
                    // Soft dashed-affordance treatment: a quiet outline
                    // distinguishes "add" from real Space tiles.
                    Rectangle {
                        anchors.fill: parent
                        z: -1
                        radius: AppTheme.radiusLg
                        color: "transparent"
                        border.width: 1
                        border.color: AppTheme.borderStrong
                    }
                }
            }
        }

        // ── Bottom cluster: settings + account ─────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing12
            Layout.rightMargin: AppTheme.spacing12
            implicitHeight: 1
            color: AppTheme.separator
            visible: app.loggedIn
        }

        Item { implicitHeight: AppTheme.spacing12; visible: app.loggedIn }

        IconButton {
            id: railSettingsButton
            objectName: "railSettingsButton"
            Layout.alignment: Qt.AlignHCenter
            implicitWidth: 40; implicitHeight: 40
            radius: AppTheme.radiusLg
            iconName: "settings"
            iconSize: 22
            // Accent chip while the in-shell Settings view is open;
            // clicking again returns to chat.
            active: app.currentScreen === 2
            visible: app.loggedIn
            // v0.7.x: the badge names WHY the cog wants attention, so a
            // screen reader is not left with a bare "Settings" while a red
            // dot sits on it.
            // ONE badge, never two dots on 68px of chrome. Verification
            // outranks an update: a security state the user must act on is
            // not the same class of thing as a release being available, and
            // colouring them alike would devalue the red one.
            // The PERSISTENT fact, not the dismissible card: dismissing the
            // corner prompt stops the interruption, and the badge is what is
            // left to say an update is still waiting.
            readonly property bool _updateBadge:
                app.updateManager && app.updateManager.updateAvailable
            readonly property string _attentionText:
                app.sessionVerificationWarning
                    ? qsTr("Settings — this session is not verified")
                    : (_updateBadge
                       ? qsTr("Settings — a Lightning update is available")
                       : qsTr("Settings"))
            Accessible.name: _attentionText
            ToolTip.text: _attentionText
            ToolTip.visible: hovered
            ToolTip.delay: 500
            onClicked: app.currentScreen === 2 ? app.showMain()
                                               : app.showSettings()

            // Attention badge: this session is not verified AND the user
            // has not dismissed the reminder. A dot, not a full "!" glyph —
            // the rail is 68px of chrome and the tooltip carries the words.
            // Ringed in the rail colour so it reads as a badge sitting ON
            // the cog rather than part of the glyph.
            Rectangle {
                objectName: "railSettingsAlertBadge"
                visible: app.sessionVerificationWarning || parent._updateBadge
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 6
                width: 10
                height: 10
                radius: 5
                color: app.sessionVerificationWarning ? AppTheme.danger
                                                      : AppTheme.warning
                border.color: AppTheme.rail
                border.width: 2
            }
        }

        Item { implicitHeight: AppTheme.spacing8; visible: app.loggedIn }

        // Account avatar (40 px circle) with presence dot; opens the
        // account switcher popover.
        Item {
            id: railAccount
            Layout.alignment: Qt.AlignHCenter
            width: 40; height: 40
            visible: app.loggedIn

            // Invokable results do not re-evaluate on signals; refresh the
            // record whenever the registry or selection changes.
            property var activeAccount: ({})
            function refreshAccount() {
                activeAccount = app.accounts
                    ? app.accounts.account(app.accounts.activeUserId) : ({})
            }
            Component.onCompleted: refreshAccount()
            Connections {
                target: app.accounts
                function onAccountsChanged() { railAccount.refreshAccount() }
                function onActiveUserIdChanged() { railAccount.refreshAccount() }
            }
            readonly property string localpart: {
                var uid = app.accounts ? (app.accounts.activeUserId || "") : ""
                if (uid.startsWith("@")) uid = uid.slice(1)
                var colon = uid.indexOf(":")
                return colon > 0 ? uid.slice(0, colon) : uid
            }

            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Account menu for %1")
                             .arg(app.accounts ? app.accounts.activeUserId : "")

            Avatar {
                id: railAvatar
                anchors.fill: parent
                size: 40
                circle: true
                name: railAccount.activeAccount.displayName
                      || railAccount.localpart
                mxc: railAccount.activeAccount.avatarUrl || ""
                // Key by the MXID like every other self-avatar surface —
                // a display-name key gave the rail its own colour and
                // recoloured on rename.
                colorKey: app.accounts ? app.accounts.activeUserId : ""
            }
            // LOCAL CONNECTIVITY, not Matrix presence. Matrix presence
            // landed in v0.7.x (PresenceDot.qml) and is shown for OTHER
            // users — on DM rows, the People list and the profile popover.
            // This dot deliberately stays a sync-connection indicator: it
            // answers "is this client talking to the homeserver", which is
            // a different question from "what state has this account
            // published", and conflating the two would let a network blip
            // read as the user going away. It uses the presence palette
            // only because those are the app's online/away inks.
            Rectangle {
                objectName: "railConnectionDot"
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                width: 11; height: 11; radius: 5.5
                border.color: AppTheme.rail
                border.width: 2
                color: app.connectionStatus === qsTr("Connected")
                       ? AppTheme.presenceOnline : AppTheme.presenceAway
                Accessible.role: Accessible.Indicator
                Accessible.name: app.connectionStatus === qsTr("Connected")
                                 ? qsTr("Connected to your homeserver")
                                 : qsTr("Not connected to your homeserver")
            }

            HoverHandler { id: accountHover }
            Rectangle {
                anchors.fill: parent
                anchors.margins: -3
                radius: AppTheme.radiusPill
                color: AppTheme.hover
                visible: accountHover.hovered
                z: -1
            }
            TapHandler { onTapped: railAccountMenu.open() }
            ToolTip {
                visible: accountHover.hovered
                text: app.accounts ? (app.accounts.activeUserId || "") : ""
                delay: 500
            }

            AccountMenu {
                id: railAccountMenu
                x: parent.width + AppTheme.spacing8
                // v0.6.5: the vertical identity-card stack can grow far
                // taller than the old single-header popover. By default it
                // still grows upward from the rail avatar's bottom (unchanged
                // behavior), but it must never push its top above the
                // window's top edge. `root.height` is read only to force
                // this binding to re-evaluate on a window resize —
                // Item.mapFromItem() results are not tracked reactively by
                // QML's binding engine on their own.
                readonly property real _windowTopLocalY: {
                    var _dep = root.height
                    return parent ? parent.mapFromItem(null, 0, 0).y : 0
                }
                y: Math.max(_windowTopLocalY + AppTheme.spacing12,
                            parent.height - implicitHeight)
            }
            // Development-only: the screenshot-demo "account-switching" scenario
            // opens the real account switcher popover. Null target in a
            // non-demo build makes this an inert no-op.
            Connections {
                target: app.demo
                enabled: app.screenshotDemoActive
                function onAccountSwitcherRequested() { railAccountMenu.open() }
            }
        }
    }
}
