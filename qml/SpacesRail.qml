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

    // 2026-08-19 tester request: inline space expansion — spaceId -> how
    // many child rooms are revealed under the tile (absent = collapsed;
    // reveals grow in steps of 5). State lives on the rail root, not the
    // delegate, so ListView recycling and model resets never forget what
    // the user expanded. Cleared on account switch.
    property var railExpansion: ({})
    // Bumped on every SpaceManager change so the revealed-rooms bindings
    // (function calls, which QML tracks through this read) re-evaluate.
    property int spacesRevision: 0
    function expandedCount(spaceId) {
        return railExpansion[spaceId] || 0
    }
    function toggleSpaceExpansion(spaceId) {
        var next = {}
        for (var k in railExpansion)
            next[k] = railExpansion[k]
        if (next[spaceId])
            delete next[spaceId]
        else
            next[spaceId] = 5
        railExpansion = next
    }
    function showMoreRooms(spaceId) {
        var next = {}
        for (var k in railExpansion)
            next[k] = railExpansion[k]
        next[spaceId] = (next[spaceId] || 0) + 5
        railExpansion = next
    }
    // The space's joined child rooms, most recently active first — the
    // quick-access reading of "top rooms". Unjoined children are join
    // offers, not rooms this rail can open; they live on Space Home.
    function topRoomsInSpace(spaceId) {
        void spacesRevision
        if (!app.spaces || !spaceId || spaceId.charAt(0) !== "!")
            return []
        var rooms = app.spaces.childRoomsDetailed(spaceId)
        rooms.sort(function(a, b) {
            return (b.lastActivity || 0) - (a.lastActivity || 0)
        })
        return rooms
    }
    Connections {
        target: app.spaces
        function onSpacesChanged() { root.spacesRevision++ }
    }
    Connections {
        target: app
        function onAccountSwitchingChanged() {
            if (app.accountSwitching)
                root.railExpansion = ({})
        }
    }

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

            ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Item {
                id: spaceItem
                width: list.width
                // 48 = 40px tile + 4px on each side so the active accent
                // outline (drawn at -4px margins) is never clipped by the
                // list bounds — this was the Home-icon clipping defect.
                // Home carries the handoff divider (32×2) below its tile.
                // The inline expansion (2026-08-19) grows the row below
                // the tile band; tileBandHeight is the original height.
                readonly property int tileBandHeight: isHome ? 58 : 48
                height: tileBandHeight
                        + (expansionCol.visible ? expansionCol.height + 2 : 0)

                property bool isActive: app.spaces
                                        && app.spaces.activeSpaceId === model.spaceId
                property bool isPseudo: model.spaceId === ""
                                        || model.spaceId === "@orphans"
                property bool isHome: model.spaceId === ""
                // Captured for the expansion rows below: inside their
                // Repeater the outer ListView's `model` is shadowed.
                readonly property string ownSpaceId: model.spaceId || ""
                readonly property bool isRealSpace:
                    !isPseudo && ownSpaceId.charAt(0) === "!"
                readonly property int revealCount:
                    root.expandedCount(ownSpaceId)
                readonly property var revealedRooms:
                    revealCount > 0 ? root.topRoomsInSpace(ownSpaceId) : []

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
                             && !expandChevronArea.visible
                    width: 6; height: 2; radius: 1
                    color: AppTheme.border
                    anchors.verticalCenter: spaceTile.verticalCenter
                    anchors.right: spaceTile.left
                    anchors.rightMargin: 1
                }

                // 2026-08-19 (third pass, screenshot-driven): the ONLY
                // expansion trigger — a quiet tree-expander glyph living
                // ENTIRELY in the gutter left of the tile, never touching
                // the active accent outline (the disc badge floated over
                // the ring and read as a misplaced blob; the bare glyph
                // before it clipped INTO the ring). Right-pointing when
                // collapsed, down when expanded — the tree convention.
                // Hover- or expanded-only; real spaces only.
                Item {
                    id: expandChevronArea
                    objectName: "railSpaceExpandChevron"
                    visible: spaceItem.isRealSpace
                             && (spaceHover.hovered
                                 || spaceItem.revealCount > 0)
                    anchors.left: parent.left
                    // Up to the accent ring's outer edge (tile - 4px) —
                    // the glyph can never overlap it.
                    width: Math.max(0, spaceTile.x - 4)
                    height: 40
                    anchors.verticalCenter: spaceTile.verticalCenter
                    Icon {
                        anchors.centerIn: parent
                        name: spaceItem.revealCount > 0 ? "expand_more"
                                                        : "chevron_right"
                        size: 10
                        color: chevronHover.hovered ? AppTheme.text
                                                    : AppTheme.textMuted
                    }
                    HoverHandler { id: chevronHover }
                    TapHandler {
                        onTapped:
                            root.toggleSpaceExpansion(spaceItem.ownSpaceId)
                    }
                    Accessible.role: Accessible.Button
                    Accessible.name: spaceItem.revealCount > 0
                                     ? qsTr("Collapse space rooms")
                                     : qsTr("Expand space rooms")
                }
                Rectangle {
                    id: spaceTile
                    width: 40; height: 40
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.horizontalCenterOffset:
                        !spaceItem.isPseudo && (model.level || 0) > 0 ? 5 : 0
                    y: 4
                    radius: AppTheme.radiusLg
                    // 2026-08-21: Home used to be a permanent solid 40x40
                    // accent block — the single largest patch of bolt on
                    // screen, present whether or not Home was the active
                    // view. With the wordmark glyph, the focus ring, every
                    // active icon chip, the selected-room edge and every
                    // primary button also bolt, the accent was carrying five
                    // jobs at once and therefore signalling none of them.
                    //
                    // ACTIVE is now ONE language for every tile in the rail:
                    // the accent ring above, plus a soft accent WASH here
                    // (a tint, not a block). Painting the active Home tile
                    // solid bolt could not work either way round — the ring
                    // is bolt, so a bolt fill four pixels inside it reads as
                    // one yellow blob rather than as "you are here".
                    color: spaceItem.isActive ? AppTheme.accentSoft
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
                        // Follows the tile: accent ink on the active wash,
                        // the plain icon ink otherwise. accentText was the
                        // ink for the solid fill that no longer exists, and
                        // on a soft wash it is unreadable.
                        color: spaceItem.isActive ? AppTheme.accent
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
                            font.pixelSize: AppTheme.textMicro
                            font.weight: AppTheme.weightBold
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
                    // 2026-08-19 (revised same day, maintainer request):
                    // a single tap on a REAL Space opens its overview —
                    // the unified rooms-and-spaces list REPLACES the chat
                    // view (openSpaceHome also activates the space, so
                    // the room-list column follows). The pseudo tiles
                    // (Home / "Other rooms") only filter — they have no
                    // overview to open and must not tear down the open
                    // room. There is deliberately NO double-tap: the
                    // chevron badge is the one expansion trigger. Scoped
                    // to the tile band (the expansion rows below carry
                    // their own handlers) and excluding the chevron's
                    // disc — TapHandlers are non-exclusive across
                    // subtrees, so without the exclusion a chevron click
                    // would also navigate.
                    function pointOnChevron(eventPoint) {
                        if (!expandChevronArea.visible)
                            return false
                        var cp = spaceItem.mapToItem(expandChevronArea,
                                                     eventPoint.position.x,
                                                     eventPoint.position.y)
                        return cp.x >= 0 && cp.x <= expandChevronArea.width
                               && cp.y >= 0
                               && cp.y <= expandChevronArea.height
                    }
                    onTapped: (eventPoint) => {
                        if (eventPoint.position.y > spaceItem.tileBandHeight)
                            return
                        if (pointOnChevron(eventPoint))
                            return
                        if (spaceItem.isRealSpace)
                            app.openSpaceHome(spaceItem.ownSpaceId)
                        else if (app.spaces)
                            app.spaces.activeSpaceId = model.spaceId
                    }
                }

                // Attached, not a declared child: a declared ToolTip is a
                // full Popup (background + Label) instantiated PER ROW, and
                // the attached form reuses the one shared instance Main.qml
                // hardens to plain text.
                ToolTip.visible: spaceHover.hovered
                ToolTip.text: spaceItem.Accessible.name
                ToolTip.delay: 500

                // Inline expansion: up to revealCount of the space's top
                // rooms as 28px tiles, then a "+N" pill revealing 5 more.
                // Tiles indent like a nested space so the hierarchy reads.
                Column {
                    id: expansionCol
                    visible: spaceItem.revealCount > 0
                             && spaceItem.revealedRooms.length > 0
                    y: spaceItem.tileBandHeight
                    width: parent.width
                    spacing: 2

                    Repeater {
                        model: expansionCol.visible
                               ? spaceItem.revealedRooms.slice(
                                     0, spaceItem.revealCount)
                               : []
                        delegate: Item {
                            id: expansionRoomRow
                            required property var modelData
                            width: expansionCol.width
                            height: 32
                            Rectangle {
                                anchors.fill: roomTile
                                anchors.margins: -2
                                radius: 10
                                color: AppTheme.hover
                                visible: roomHover.hovered
                            }
                            Rectangle {
                                id: roomTile
                                width: 28; height: 28
                                radius: 8
                                color: "transparent"
                                anchors.horizontalCenter:
                                    parent.horizontalCenter
                                // One level deeper than the OWNING tile —
                                // a nested space's rooms step in further,
                                // not back to the flat +5 (audit find).
                                anchors.horizontalCenterOffset:
                                    spaceTile.anchors.horizontalCenterOffset
                                    + 5
                                anchors.verticalCenter: parent.verticalCenter
                                Avatar {
                                    anchors.fill: parent
                                    size: 28
                                    circle: expansionRoomRow.modelData
                                                .isDirect === true
                                    squareRadius: 8
                                    labelSize: 11
                                    name: expansionRoomRow.modelData
                                              .name || ""
                                    colorKey: expansionRoomRow.modelData
                                                  .identityColorKey
                                              || expansionRoomRow.modelData
                                                     .roomId
                                    mxc: expansionRoomRow.modelData
                                             .avatarUrl || ""
                                }
                                Rectangle {
                                    visible: expansionRoomRow.modelData
                                                 .hasUnread === true
                                    width: 10; height: 10; radius: 5
                                    color: (expansionRoomRow.modelData
                                                .highlightCount || 0) > 0
                                           ? AppTheme.mentionBadge
                                           : AppTheme.unreadBadge
                                    border.color: AppTheme.rail
                                    border.width: 2
                                    anchors.top: parent.top
                                    anchors.right: parent.right
                                    anchors.topMargin: -2
                                    anchors.rightMargin: -2
                                }
                            }
                            HoverHandler { id: roomHover }
                            TapHandler {
                                // Opening from the rail also activates
                                // the space so the room-list column
                                // follows — openRoom itself never
                                // touches activeSpaceId.
                                onTapped: {
                                    if (app.spaces)
                                        app.spaces.activeSpaceId =
                                            spaceItem.ownSpaceId
                                    app.openRoom(
                                        expansionRoomRow.modelData.roomId)
                                }
                            }
                            ToolTip.visible: roomHover.hovered
                            ToolTip.text: expansionRoomRow.modelData.name
                                          || expansionRoomRow.modelData.roomId
                            ToolTip.delay: 300
                            Accessible.role: Accessible.Button
                            Accessible.name: expansionRoomRow.modelData.name
                                             || expansionRoomRow.modelData
                                                    .roomId
                        }
                    }

                    Item {
                        visible: spaceItem.revealedRooms.length
                                 > spaceItem.revealCount
                        width: expansionCol.width
                        height: 22
                        Rectangle {
                            id: morePill
                            objectName: "railSpaceMoreButton"
                            width: 32; height: 18; radius: 9
                            color: moreHover.hovered ? AppTheme.hover
                                                     : AppTheme.cardElevated
                            border.color: AppTheme.border
                            border.width: 1
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.horizontalCenterOffset:
                                spaceTile.anchors.horizontalCenterOffset + 5
                            anchors.verticalCenter: parent.verticalCenter
                            Label {
                                anchors.centerIn: parent
                                text: "+" + Math.min(
                                          5,
                                          spaceItem.revealedRooms.length
                                          - spaceItem.revealCount)
                                font.pixelSize: AppTheme.textMicro
                                font.weight: AppTheme.weightBold
                                color: AppTheme.textSecondary
                            }
                        }
                        HoverHandler { id: moreHover }
                        TapHandler {
                            onTapped:
                                root.showMoreRooms(spaceItem.ownSpaceId)
                        }
                        ToolTip.visible: moreHover.hovered
                        ToolTip.text: qsTr("Show more rooms")
                        ToolTip.delay: 300
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Show more rooms")
                    }
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
                // `danger`/`warning` became INK-ONLY roles on 2026-08-21
                // (they route light on dark themes so they stay AA as text);
                // a badge is a FILL and must ask for the saturated fill by
                // name, or the dot renders as a pale rose smudge on the rail.
                color: app.sessionVerificationWarning ? AppTheme.dangerFill
                                                      : AppTheme.warningFill
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
            ToolTip.visible: accountHover.hovered
            ToolTip.text: app.accounts ? (app.accounts.activeUserId || "") : ""
            ToolTip.delay: 500

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
