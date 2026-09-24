import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The Space Home LOBBY (2026-09-23): the list of what a Space contains.
//
// Sectioned the way Sable's lobby is, on a tester's report comparing the two
// ("you cant tell which rooms belong to each space"): the Space's own rooms
// first, then ONE collapsible section per joined subspace listing that
// subspace's rooms, every row carrying the room's topic as a second line.
// The flat list it replaced read SpaceManager::childRoomsDetailed(), which is
// TRANSITIVE, so every subspace's rooms ran together with the Space's own.
//
// PRESENTATION ONLY. The sections are built in C++
// (SpaceManager::lobbySections: grouping, m.space.child order, search, the
// collapsed state) and arrive here as `sections`; this file draws them and
// reports what the user asked for through signals, which TimelinePane's Space
// Home turns into app calls. A section that is folded or filtered away is
// absent from the DATA — never a delegate hidden with `visible:` — because a
// setting that removes rows is a model state (CLAUDE.md §16).
//
// Moved strings keep the "TimelinePane" translation context through
// qsTranslate, so the catalogs' existing translations still apply.
//
// Both Repeaters take a COUNT, not the array: every sync hands this a new
// array, and an array model rebuilds every delegate — closing an open menu
// and dropping focus. With a count, delegates survive and rebind by index.
ColumnLayout {
    id: root
    objectName: "spaceLobby"
    spacing: AppTheme.spacingS

    // ---- inputs -------------------------------------------------------------
    /// SpaceManager::lobbySections() output.
    property var sections: []
    /// Whether the account may send m.space.child in the Home Space.
    property bool canManage: false
    /// Selected direct children of the Home Space: { roomId: true }.
    property var selectedIds: ({})
    property string filterText: ""
    /// A join/knock is in flight (RoomDiscoveryController.busy).
    property bool busy: false
    property string errorMessage: ""
    /// /hierarchy has not answered yet: for the Home, and by section id.
    property bool homeLoading: false
    property var loadingIds: ({})

    // ---- outputs ------------------------------------------------------------
    signal filterEdited(string text)
    signal openRoomRequested(string roomId)
    signal openSpaceRequested(string spaceId)
    signal joinRequested(string roomId, var via, bool isSpace)
    signal knockRequested(string roomId, var via)
    signal selectionToggled(string roomId)
    signal collapseToggled(string sectionId, bool collapsed)
    signal removeRequested(var roomIds)
    signal suggestRequested(var roomIds, bool suggested)

    readonly property int selectedCount: Object.keys(selectedIds).length

    // Keyboard focus to hand back to a header after a rebuild, by id.
    property string focusSectionId: ""
    property bool restoreFocus: false

    function sectionById(id) {
        for (var i = 0; i < sections.length; ++i) {
            if (sections[i].sectionId === id)
                return sections[i]
        }
        return null
    }
    // Called when the Home switches Space: a menu aimed at the old Space's
    // section must not survive into the new one.
    function closeMenus() { sectionMenu.close() }
    function restoreHeaderFocus() {
        if (!restoreFocus)
            return
        restoreFocus = false
        for (var i = 0; i < sectionRepeater.count; ++i) {
            var item = sectionRepeater.itemAt(i)
            if (item && item.modelData.sectionId === focusSectionId) {
                item.focusHeader()
                return
            }
        }
    }
    onSectionsChanged: {
        // The shared menu follows its section by id, or closes with it.
        if (sectionMenu.opened) {
            var t = sectionById(sectionMenu.target.sectionId || "")
            if (t)
                sectionMenu.target = t
            else
                sectionMenu.close()
        }
        if (restoreFocus)
            Qt.callLater(root.restoreHeaderFocus)
    }
    readonly property int totalRows: {
        var n = 0
        for (var i = 0; i < sections.length; ++i)
            n += Number(sections[i].matchCount || 0)
        return n
    }

    // Every selectable entry currently known, by id: root rows and subspace
    // HEADERS (a subspace is itself a direct child of the Home Space).
    function selectableById() {
        var out = {}
        for (var i = 0; i < sections.length; ++i) {
            var s = sections[i]
            if (!s.isRoot && s.selectable === true)
                out[s.roomId] = s
            var rows = s.rows || []
            for (var j = 0; j < rows.length; ++j) {
                if (rows[j].selectable === true)
                    out[rows[j].roomId] = rows[j]
            }
        }
        return out
    }
    // The suggest toggle mirrors Element: one button whose action follows the
    // selection — all-suggested flips off, otherwise on.
    function selectedAllSuggested() {
        var known = selectableById()
        var any = false
        for (var id in selectedIds) {
            var entry = known[id]
            if (!entry)
                continue
            any = true
            if (entry.suggested !== true)
                return false
        }
        return any
    }

    // ---- toolbar: selection actions and search -----------------------------
    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: AppTheme.spacingS
        spacing: AppTheme.spacing8
        Label {
            // Pinned by SpaceSettingsContractTest and ElementParityContract-
            // Test; the casing is legacy, the size and weight are on scale.
            text: qsTranslate("TimelinePane", "ROOMS AND SPACES")
            color: AppTheme.textSecondary
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
            font.weight: AppTheme.weightStrong
            font.letterSpacing: 0.8
        }
        Label {
            visible: root.selectedCount > 0
            text: qsTranslate("TimelinePane", "%n selected", "",
                              root.selectedCount)
            color: AppTheme.chipAccentInk
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
            font.weight: AppTheme.weightMedium
        }
        Item { Layout.fillWidth: true }
        AppButton {
            objectName: "spaceChildRemoveSelectedButton"
            visible: root.canManage
            kind: "danger"
            enabled: root.selectedCount > 0
            text: qsTranslate("TimelinePane", "Remove")
            onClicked: root.removeRequested(Object.keys(root.selectedIds))
        }
        AppButton {
            objectName: "spaceChildSuggestToggleButton"
            visible: root.canManage
            enabled: root.selectedCount > 0
            text: root.selectedAllSuggested()
                  ? qsTranslate("TimelinePane", "Mark as not suggested")
                  : qsTranslate("TimelinePane", "Mark as suggested")
            onClicked: root.suggestRequested(Object.keys(root.selectedIds),
                                             !root.selectedAllSuggested())
        }
    }
    AppTextField {
        objectName: "spaceChildFilterField"
        Layout.fillWidth: true
        searchIcon: true
        clearButton: true
        placeholderText:
            qsTranslate("TimelinePane", "Search names and descriptions")
        Accessible.name: qsTranslate("TimelinePane", "Search rooms and spaces")
        text: root.filterText
        onTextChanged: {
            if (text !== root.filterText)
                root.filterEdited(text)
        }
    }

    // A filter with no matches says so — a silently blank list reads as
    // stuck (2026-08-19 audit).
    Label {
        objectName: "spaceLobbyNoMatches"
        visible: root.sections.length === 0 && root.filterText !== ""
        Layout.fillWidth: true
        text: qsTranslate("TimelinePane", "No rooms or spaces match “%1”.")
                  .arg(root.filterText)
        textFormat: Text.PlainText
        color: AppTheme.textMuted
        font.family: AppTheme.uiFont
        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
        wrapMode: Text.Wrap
    }

    // Empty state for a fresh Space.
    Rectangle {
        objectName: "spaceLobbyEmpty"
        visible: root.sections.length === 0 && root.filterText === ""
        Layout.fillWidth: true
        radius: AppTheme.radiusMd
        color: AppTheme.cardElevated
        border.color: AppTheme.border
        border.width: 1
        implicitHeight: emptyCol.implicitHeight + AppTheme.spacing16 * 2
        ColumnLayout {
            id: emptyCol
            anchors.fill: parent
            anchors.margins: AppTheme.spacing16
            spacing: AppTheme.spacingXS
            Label {
                objectName: "spaceLobbyEmptyTitle"
                text: root.homeLoading
                      ? qsTranslate("RoomListPane", "Loading rooms…")
                      : qsTranslate("TimelinePane", "No rooms yet")
                color: AppTheme.text
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.scaled(AppTheme.textSubtitle)
                font.weight: AppTheme.weightStrong
            }
            Label {
                visible: !root.homeLoading
                Layout.fillWidth: true
                text: qsTranslate("TimelinePane",
                                  "Create a room here or add one of "
                                  + "your existing rooms to organise "
                                  + "it under this Space.")
                color: AppTheme.textSecondary
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
            }
        }
    }

    // ---- the sections --------------------------------------------------------
    Repeater {
        id: sectionRepeater
        model: root.sections.length
        delegate: ColumnLayout {
            id: sectionItem
            required property int index
            readonly property var modelData: root.sections[index] || ({})
            objectName: "spaceLobbySection"
            function focusHeader() { sectionHeader.forceActiveFocus() }
            readonly property bool isRoot: modelData.isRoot === true
            readonly property bool collapsed: modelData.collapsed === true
            readonly property bool headerSelected:
                root.selectedIds[modelData.roomId] === true
            readonly property string title: isRoot
                ? qsTr("Rooms")
                : (modelData.name || qsTranslate("TimelinePane", "Space"))
            Layout.fillWidth: true
            Layout.topMargin: sectionItem.index > 0 ? AppTheme.spacing8
                                                   : AppTheme.spacingXS
            spacing: AppTheme.spacingXS

            // Section header: chevron, avatar (subspaces), name, count, and
            // for a subspace its menu and — for a manager — its selection
            // box. A tap anywhere else on the header folds the section.
            Item {
                id: sectionHeader
                objectName: "spaceLobbySectionHeader"
                Layout.fillWidth: true
                implicitHeight: 36
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: sectionItem.collapsed
                    ? qsTranslate("ChannelCategoryHeader", "%1, collapsed")
                          .arg(sectionItem.title)
                    : qsTranslate("ChannelCategoryHeader", "%1, expanded")
                          .arg(sectionItem.title)
                Accessible.description: sectionItem.collapsed
                    ? qsTranslate("ChannelCategoryHeader", "Activate to expand")
                    : qsTranslate("ChannelCategoryHeader", "Activate to collapse")
                function toggle() {
                    if (sectionHeader.activeFocus) {
                        root.focusSectionId = sectionItem.modelData.sectionId
                        root.restoreFocus = true
                    }
                    root.collapseToggled(sectionItem.modelData.sectionId,
                                         !sectionItem.collapsed)
                }
                Keys.onReturnPressed: toggle()
                Keys.onSpacePressed: toggle()
                HoverHandler { id: headerHover }
                Rectangle {
                    anchors.fill: parent
                    radius: AppTheme.radiusSm
                    color: headerHover.hovered || sectionHeader.activeFocus
                           ? AppTheme.hover : "transparent"
                }
                TapHandler {
                    // The menu button and the selection box are EXCLUDED:
                    // TapHandlers are non-exclusive across subtrees, so a tap
                    // meant for either would otherwise also fold the section.
                    onTapped: (eventPoint) => {
                        var bands = [sectionMenuButton, sectionSelectBox]
                        for (var i = 0; i < bands.length; ++i) {
                            var b = bands[i]
                            if (!b.visible)
                                continue
                            var p = sectionHeader.mapToItem(
                                b, eventPoint.position.x,
                                eventPoint.position.y)
                            if (p.x >= 0 && p.x <= b.width
                                    && p.y >= 0 && p.y <= b.height)
                                return
                        }
                        sectionHeader.toggle()
                    }
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: AppTheme.spacingXS
                    anchors.rightMargin: AppTheme.spacingS
                    spacing: AppTheme.spacing8
                    Icon {
                        name: sectionItem.collapsed ? "chevron_right"
                                                    : "expand_more"
                        size: 18
                        color: AppTheme.textMuted
                    }
                    Avatar {
                        visible: !sectionItem.isRoot
                        size: 24
                        circle: false
                        squareRadius: AppTheme.radiusSm
                        name: sectionItem.modelData.name || ""
                        mxc: sectionItem.modelData.avatarUrl || ""
                        colorKey: sectionItem.modelData.identityColorKey
                                  || sectionItem.modelData.roomId || ""
                    }
                    Label {
                        objectName: "spaceLobbySectionTitle"
                        text: sectionItem.title
                        textFormat: Text.PlainText
                        color: AppTheme.text
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                        font.weight: AppTheme.weightStrong
                        elide: Label.ElideRight
                        // Against the HEADER, never this layout's own width
                        // (a cap read from the arranged width is an input
                        // the layout produces: "recursive rearrange").
                        Layout.maximumWidth: sectionHeader.width * 0.5
                    }
                    Label {
                        text: qsTranslate("TimelinePane", "%n room(s)",
                                          "rooms inside a Space",
                                          Number(sectionItem.modelData
                                                     .roomCount || 0))
                        color: AppTheme.textMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    }
                    SuggestedChip {
                        visible: !sectionItem.isRoot
                                 && sectionItem.modelData.suggested === true
                    }
                    // A folded section must not mute its rooms: it carries
                    // their activity (the Channels header's rule).
                    Rectangle {
                        objectName: "spaceLobbyFoldedUnread"
                        visible: sectionItem.collapsed
                                 && sectionItem.modelData.hasUnread === true
                        radius: height / 2
                        color: Number(sectionItem.modelData
                                          .highlightTotal || 0) > 0
                               ? AppTheme.dangerFill : AppTheme.unreadBadge
                        implicitHeight: 18
                        implicitWidth: Math.max(18, foldedCount.implicitWidth + 10)
                        Label {
                            id: foldedCount
                            anchors.centerIn: parent
                            text: {
                                var u = Number(sectionItem.modelData
                                                   .unreadTotal || 0)
                                return u > 99 ? "99+" : u > 0 ? String(u) : ""
                            }
                            color: Number(sectionItem.modelData
                                              .highlightTotal || 0) > 0
                                   ? AppTheme.dangerText : AppTheme.boltInk
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                            font.weight: AppTheme.weightBold
                        }
                    }
                    Item { Layout.fillWidth: true }
                    IconButton {
                        id: sectionMenuButton
                        objectName: "spaceLobbySectionMenuButton"
                        visible: !sectionItem.isRoot
                        size: "sm"
                        iconName: "more_vert"
                        active: sectionMenu.opened
                                && sectionMenu.target.sectionId
                                   === sectionItem.modelData.sectionId
                        Accessible.name: qsTr("More actions for %1")
                                             .arg(sectionItem.title)
                        // Plain text app-wide: Main.qml's sharedToolTipGuard.
                        ToolTip.text: Accessible.name
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: {
                            sectionMenu.target = sectionItem.modelData
                            // Anchored to the LOBBY, not this button: a
                            // rebuild that changes the section count
                            // destroys the button, and a popup whose parent
                            // item goes away closes with it.
                            var p = sectionMenuButton.mapToItem(
                                root, 0,
                                sectionMenuButton.height + AppTheme.spacing4)
                            sectionMenu.popup(root, p.x, p.y)
                        }
                    }
                    SelectBox {
                        id: sectionSelectBox
                        visible: root.canManage && !sectionItem.isRoot
                                 && sectionItem.modelData.selectable === true
                        checked: sectionItem.headerSelected
                        label: sectionItem.title
                        onToggled: root.selectionToggled(
                                       sectionItem.modelData.roomId)
                    }
                }
            }

            // The section's rooms, in one card. Absent when folded: the model
            // hands a folded section no rows.
            Rectangle {
                objectName: "spaceLobbySectionCard"
                visible: !sectionItem.collapsed
                Layout.fillWidth: true
                radius: AppTheme.radiusMd
                color: AppTheme.cardElevated
                border.color: AppTheme.border
                border.width: 1
                implicitHeight: sectionRows.implicitHeight
                                + AppTheme.spacingXS * 2
                ColumnLayout {
                    id: sectionRows
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: AppTheme.spacingXS
                    spacing: 0
                    Label {
                        objectName: "spaceLobbySectionEmpty"
                        visible: (sectionItem.modelData.rows || []).length === 0
                        Layout.fillWidth: true
                        Layout.margins: AppTheme.spacing8
                        text: root.loadingIds[sectionItem.modelData.sectionId]
                              === true
                              ? qsTranslate("RoomListPane", "Loading rooms…")
                              : qsTranslate("TimelinePane", "No rooms yet")
                        color: AppTheme.textMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    }
                    Repeater {
                        model: (sectionItem.modelData.rows || []).length
                        // One room (or nested Space) in a section: avatar, name, badges, and the
                        // room's TOPIC as a second line — plain text always, it is server text
                        // anyone with state access may set (§6). Joined rows open on a tap;
                        // unjoined ones act only through their Join button. There is no "Joined"
                        // chip: the row's action already says it (Join vs the open arrow), and a
                        // chip beside every room read as the user's ROLE — Portuguese renders it
                        // "Membro" (tester report, 2026-09-23).
                        delegate: Rectangle {
                            id: row
                            required property int index
                            readonly property var modelData:
                                (sectionItem.modelData.rows || [])[index]
                                || ({})
                            objectName: "spaceUnifiedChildRow"
                            Layout.fillWidth: true
                            readonly property bool joined: modelData.joined === true
                            readonly property bool isSpace: modelData.isSpace === true
                            readonly property bool rowSelected:
                                root.selectedIds[modelData.roomId] === true
                            readonly property bool rowKnocks:
                                modelData.joinRule === "knock"
                                || modelData.joinRule === "knock_restricted"
                            readonly property string topic: modelData.topic || ""
                            readonly property string displayName:
                                modelData.name || qsTranslate("TimelinePane", "Room")
                            implicitHeight: Math.max(52, rowText.implicitHeight
                                                         + AppTheme.spacing8 * 2)
                            radius: AppTheme.radiusSm
                            // Hover feedback only where a click acts.
                            color: rowHover.hovered && row.joined ? AppTheme.hover : "transparent"
                            HoverHandler { id: rowHover }
                            TapHandler {
                                // The selection box's band is excluded: a select tap must not
                                // ALSO open the row.
                                onTapped: (eventPoint) => {
                                    if (selectBox.visible) {
                                        var sp = row.mapToItem(selectBox, eventPoint.position.x,
                                                               eventPoint.position.y)
                                        if (sp.x >= 0 && sp.x <= selectBox.width
                                                && sp.y >= 0 && sp.y <= selectBox.height)
                                            return
                                    }
                                    if (!row.joined)
                                        return
                                    if (row.isSpace)
                                        root.openSpaceRequested(row.modelData.roomId)
                                    else
                                        root.openRoomRequested(row.modelData.roomId)
                                }
                            }
                            Accessible.role: Accessible.Button
                            Accessible.name: row.joined
                                ? row.displayName
                                : qsTr("%1, not joined").arg(row.displayName)
                            Accessible.description: row.topic

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: AppTheme.spacingS
                                anchors.rightMargin: AppTheme.spacingS
                                spacing: AppTheme.spacingS
                                Avatar {
                                    size: 32
                                    circle: row.modelData.isDirect === true
                                    name: row.modelData.name || ""
                                    mxc: row.modelData.avatarUrl || ""
                                    colorKey: row.modelData.identityColorKey
                                              || row.modelData.roomId || ""
                                }
                                ColumnLayout {
                                    id: rowText
                                    Layout.fillWidth: true
                                    spacing: 1
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: AppTheme.spacing6
                                        Label {
                                            objectName: "spaceLobbyRowName"
                                            text: row.displayName
                                            textFormat: Text.PlainText
                                            color: AppTheme.text
                                            font.family: AppTheme.uiFont
                                            font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                                            font.weight: row.modelData.hasUnread === true
                                                         ? AppTheme.weightBold
                                                         : AppTheme.weightMedium
                                            elide: Label.ElideRight
                                            // Measured against the ROW, never this layout.
                                            Layout.maximumWidth: row.width * 0.55
                                        }
                                        SuggestedChip {
                                            visible: row.modelData.suggested === true
                                        }
                                        Label {
                                            objectName: "spaceLobbyRowMeta"
                                            visible: text.length > 0
                                            text: {
                                                var d = row.modelData
                                                if (d.isSpace && d.joined)
                                                    return qsTranslate("TimelinePane",
                                                        "Space · %n room(s)", "",
                                                        Number(d.childCount || 0))
                                                if (d.isSpace)
                                                    return qsTranslate("TimelinePane",
                                                        "Space · %n room(s) inside", "",
                                                        Number(d.childrenCount || 0))
                                                if (Number(d.members || 0) > 0)
                                                    return qsTranslate("TimelinePane",
                                                        "%n member(s)", "", Number(d.members))
                                                return ""
                                            }
                                            color: AppTheme.textMuted
                                            font.family: AppTheme.uiFont
                                            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                                            elide: Label.ElideRight
                                            Layout.maximumWidth: row.width * 0.3
                                        }
                                        Item { Layout.fillWidth: true }
                                    }
                                    Label {
                                        objectName: "spaceLobbyRowTopic"
                                        visible: row.topic.length > 0
                                        Layout.fillWidth: true
                                        text: row.topic
                                        // Unsanitized server text; NEVER AutoText or StyledText.
                                        textFormat: Text.PlainText
                                        color: AppTheme.textSecondary
                                        font.family: AppTheme.uiFont
                                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                                        elide: Label.ElideRight
                                        maximumLineCount: 1
                                        wrapMode: Text.NoWrap
                                    }
                                }
                                Rectangle {
                                    visible: Number(row.modelData.highlightCount || 0) > 0
                                    radius: height / 2
                                    color: AppTheme.dangerFill
                                    implicitHeight: 18
                                    implicitWidth: Math.max(18, rowMention.implicitWidth + 10)
                                    Label {
                                        id: rowMention
                                        anchors.centerIn: parent
                                        text: "@"
                                        color: AppTheme.dangerText
                                        font.family: AppTheme.uiFont
                                        font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                                        font.weight: AppTheme.weightBold
                                    }
                                }
                                Rectangle {
                                    visible: row.modelData.hasUnread === true
                                    radius: height / 2
                                    color: AppTheme.unreadBadge
                                    implicitHeight: 18
                                    implicitWidth: Math.max(18, rowUnread.implicitWidth + 10)
                                    Label {
                                        id: rowUnread
                                        anchors.centerIn: parent
                                        visible: Number(row.modelData.unreadCount || 0) > 0
                                        text: Number(row.modelData.unreadCount || 0) > 99
                                              ? "99+" : String(row.modelData.unreadCount || 0)
                                        color: AppTheme.boltInk
                                        font.family: AppTheme.uiFont
                                        font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                                        font.weight: AppTheme.weightBold
                                    }
                                }
                                Label {
                                    visible: row.modelData.membership === "knocked"
                                    text: qsTranslate("TimelinePane", "Request pending")
                                    color: AppTheme.chipWarningInk
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                                    font.weight: AppTheme.weightStrong
                                    leftPadding: AppTheme.chipPaddingH
                                    rightPadding: AppTheme.chipPaddingH
                                    topPadding: AppTheme.keycapPaddingV
                                    bottomPadding: AppTheme.keycapPaddingV
                                    background: Rectangle {
                                        radius: AppTheme.chipRadius
                                        color: AppTheme.chipWarningFill
                                        border.color: AppTheme.chipWarningBorder
                                        border.width: 1
                                    }
                                }
                                AppButton {
                                    objectName: "spaceLobbyJoinButton"
                                    visible: !row.joined && row.modelData.membership !== "knocked"
                                    kind: "primary"
                                    size: "sm"
                                    enabled: !root.busy
                                    text: row.rowKnocks ? qsTranslate("TimelinePane", "Ask to join")
                                                        : qsTranslate("TimelinePane", "Join")
                                    onClicked: {
                                        var via = row.modelData.via || []
                                        if (row.rowKnocks)
                                            root.knockRequested(row.modelData.roomId, via)
                                        else
                                            root.joinRequested(row.modelData.roomId, via,
                                                               row.isSpace)
                                    }
                                }
                                // The open affordance. Decoration only: the whole row is the
                                // button, so a second handler here would open the room twice.
                                Rectangle {
                                    objectName: "spaceLobbyOpenGlyph"
                                    visible: row.joined
                                    implicitWidth: 28
                                    implicitHeight: 28
                                    radius: AppTheme.radiusSm
                                    color: rowHover.hovered ? AppTheme.cardElevated : "transparent"
                                    Icon {
                                        anchors.centerIn: parent
                                        name: row.isSpace ? "chevron_right" : "arrow_forward"
                                        size: 16
                                        color: AppTheme.textMuted
                                    }
                                }
                                SelectBox {
                                    id: selectBox
                                    visible: root.canManage && row.modelData.selectable === true
                                    checked: row.rowSelected
                                    label: row.modelData.name || ""
                                    onToggled: root.selectionToggled(row.modelData.roomId)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Label {
        visible: root.errorMessage.length > 0 && root.totalRows > 0
        Layout.fillWidth: true
        text: root.errorMessage
        textFormat: Text.PlainText
        color: AppTheme.danger
        font.family: AppTheme.uiFont
        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
        wrapMode: Text.Wrap
    }

    // ONE menu for every subspace header. It lives here, not in a delegate,
    // so a rebuild while it is open cannot destroy it; `target` is the
    // section's DATA, re-read by id in onSectionsChanged.
    AppMenu {
        id: sectionMenu
        objectName: "spaceLobbySectionMenu"
        property var target: ({})
        AppMenuItem {
            objectName: "spaceLobbyOpenSpace"
            iconName: "arrow_forward"
            text: qsTr("Open space")
            onTriggered: root.openSpaceRequested(sectionMenu.target.roomId)
        }
        AppMenuItem {
            visible: root.canManage
            iconName: "star"
            text: sectionMenu.target.suggested === true
                  ? qsTranslate("TimelinePane", "Mark as not suggested")
                  : qsTranslate("TimelinePane", "Mark as suggested")
            onTriggered: root.suggestRequested([sectionMenu.target.roomId],
                                               sectionMenu.target.suggested
                                               !== true)
        }
        AppMenuItem {
            visible: root.canManage
            iconName: "delete"
            danger: true
            text: qsTr("Remove from this Space")
            onTriggered: root.removeRequested([sectionMenu.target.roomId])
        }
    }

    // ---- building blocks ----------------------------------------------------

    // "Suggested" is the Space owner's recommendation, a different KIND of
    // fact from membership, so it keeps its own chip family.
    component SuggestedChip: Label {
        text: qsTranslate("TimelinePane", "Suggested")
        color: AppTheme.chipAccentInk
        font.family: AppTheme.uiFont
        font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
        font.weight: AppTheme.weightStrong
        leftPadding: AppTheme.chipPaddingH
        rightPadding: AppTheme.chipPaddingH
        topPadding: AppTheme.keycapPaddingV
        bottomPadding: AppTheme.keycapPaddingV
        background: Rectangle {
            radius: AppTheme.chipRadius
            color: AppTheme.chipAccentFill
            border.color: AppTheme.chipAccentBorder
            border.width: 1
        }
    }

    // Element's selection UI: a checkbox, shown only where the account can
    // actually send m.space.child. WithinBounds takes the EXCLUSIVE grab, so
    // an ancestor's TapHandler does not also fire on the same press.
    component SelectBox: Item {
        id: box
        objectName: "spaceChildSelectBox"
        property bool checked: false
        property string label: ""
        signal toggled()
        implicitWidth: 26
        implicitHeight: 26
        Layout.preferredWidth: 26
        Layout.preferredHeight: 26
        Rectangle {
            anchors.centerIn: parent
            width: 18; height: 18
            radius: 4
            color: box.checked ? AppTheme.accent : "transparent"
            border.color: box.checked ? AppTheme.accent : AppTheme.borderStrong
            border.width: 1
            Icon {
                anchors.centerIn: parent
                visible: box.checked
                name: "check"
                size: 13
                color: AppTheme.accentText
            }
        }
        TapHandler {
            gesturePolicy: TapHandler.WithinBounds
            onTapped: box.toggled()
        }
        Accessible.role: Accessible.CheckBox
        Accessible.checked: box.checked
        Accessible.name: qsTranslate("TimelinePane", "Select %1").arg(box.label)
    }
}
