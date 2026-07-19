import QtQuick
import QtQuick.Controls
import QtQuick.Effects
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
                height: 48

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

                Rectangle {
                    id: spaceTile
                    width: 40; height: 40
                    anchors.centerIn: parent
                    radius: AppTheme.radiusLg
                    color: spaceItem.isHome
                           ? (spaceItem.isActive ? AppTheme.accent
                                                 : AppTheme.cardElevated)
                           : AppTheme.cardElevated

                    Behavior on color { ColorAnimation { duration: 120 } }

                    // Pseudo rows use monochrome icons; real Spaces show
                    // their initial until the avatar loads.
                    Icon {
                        anchors.centerIn: parent
                        visible: spaceItem.isPseudo
                                 && spaceImage.status !== Image.Ready
                        name: spaceItem.isHome ? "home" : "workspaces"
                        size: 20
                        color: spaceItem.isHome && spaceItem.isActive
                               ? AppTheme.accentText : AppTheme.textSecondary
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: !spaceItem.isPseudo
                                 && spaceImage.status !== Image.Ready
                        text: model.name && model.name.length > 0
                              ? model.name[0].toUpperCase() : "#"
                        font.pixelSize: 15
                        font.weight: Font.Bold
                        color: AppTheme.textSecondary
                    }

                    // Real Space avatar via the shared media bridge; shown
                    // only once fully decoded, masked to the tile shape.
                    Image {
                        id: spaceImage
                        anchors.fill: parent
                        visible: false
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: true
                        property string mxc: spaceItem.isPseudo
                                             ? "" : (model.avatarUrl || "")
                        source: mxc.length > 0 && app.mediaBridge.supported
                                ? app.mediaBridge.avatarSource(mxc, 80) : ""
                        Connections {
                            target: app.mediaBridge
                            enabled: spaceImage.mxc.length > 0
                            function onMediaCached(cacheKey) {
                                spaceImage.source = app.mediaBridge.avatarSource(
                                    spaceImage.mxc, 80)
                            }
                        }
                    }
                    Item {
                        id: spaceMask
                        anchors.fill: parent
                        visible: false
                        layer.enabled: true
                        Rectangle {
                            anchors.fill: parent
                            radius: spaceTile.radius
                            color: "black"
                        }
                    }
                    MultiEffect {
                        anchors.fill: parent
                        source: spaceImage
                        maskEnabled: true
                        maskSource: spaceMask
                        visible: spaceImage.status === Image.Ready
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
                            font.weight: Font.DemiBold
                            color: AppTheme.accentText
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
                    onTapped: if (app.spaces) app.spaces.activeSpaceId = model.spaceId
                }

                ToolTip {
                    visible: spaceHover.hovered
                    text: spaceItem.Accessible.name
                    delay: 500
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

        ToolButton {
            id: railSettingsButton
            Layout.alignment: Qt.AlignHCenter
            implicitWidth: 40; implicitHeight: 40
            visible: app.loggedIn
            Accessible.name: qsTr("Settings")
            ToolTip.text: qsTr("Settings")
            ToolTip.visible: hovered
            ToolTip.delay: 500
            contentItem: Icon {
                name: "settings"
                size: 19
                color: AppTheme.textSecondary
            }
            background: Rectangle {
                radius: AppTheme.radiusLg
                color: railSettingsButton.hovered ? AppTheme.hover : "transparent"
            }
            onClicked: app.showSettings()
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
            }
            // Presence: Lightning shows its own connection state on the
            // self avatar (Matrix presence is not surfaced yet).
            Rectangle {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                width: 11; height: 11; radius: 5.5
                border.color: AppTheme.rail
                border.width: 2
                color: app.connectionStatus === qsTr("Connected")
                       ? AppTheme.presenceOnline : AppTheme.presenceAway
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
                y: -implicitHeight + parent.height
            }
        }
    }
}
