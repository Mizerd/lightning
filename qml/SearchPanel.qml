import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Room-scoped server search plus a draft/apply filter surface. Matrix can
// apply room and sender on the server; MessageSearchController applies the
// remaining predicates to a bounded number of returned pages. Unsupported
// Discord concepts (embed, forward and guessed bot identity) are absent.
Rectangle {
    id: root

    signal closeRequested()
    signal jumpToEventRequested(string eventId)
    signal findLoadedRequested()

    property bool historyAvailable: true
    property bool filterEditing: false
    property var draftFrom: []
    property var draftMentions: []
    property var draftTypes: []
    property string draftAfter: ""
    property string draftBefore: ""
    property string draftPinned: "any"
    property string memberNeedle: ""
    property string memberSection: ""
    property string dateError: ""

    color: AppTheme.surface

    function cloneStrings(value) {
        var out = []
        if (!value) return out
        for (var i = 0; i < value.length; ++i)
            out.push(String(value[i]))
        return out
    }

    function beginFilters() {
        var applied = app.messageSearch.filters || ({})
        draftFrom = cloneStrings(applied.fromUserIds)
        draftMentions = cloneStrings(applied.mentionUserIds)
        draftTypes = cloneStrings(applied.contentTypes)
        draftAfter = applied.afterDate || ""
        draftBefore = applied.beforeDate || ""
        draftPinned = applied.pinnedMode || "any"
        memberNeedle = ""
        memberSection = ""
        dateError = ""
        filterEditing = true
    }

    function contains(values, value) {
        return values.indexOf(value) >= 0
    }

    function toggleValue(propertyName, value) {
        var values = cloneStrings(root[propertyName])
        var at = values.indexOf(value)
        if (at >= 0) values.splice(at, 1)
        else values.push(value)
        root[propertyName] = values
    }

    // The roster comes from RoomInfoController, which tracks the ROOM
    // INFORMATION panel's room — not necessarily this one. Opening search
    // without ever having opened Room Information left it pointed elsewhere
    // (or nowhere), so every member section came up empty and clicking
    // Mentions or From found nobody. Point it at the room being searched
    // before reading the roster.
    onVisibleChanged: if (visible) ensureRoster()
    Component.onCompleted: if (visible) ensureRoster()

    function ensureRoster() {
        if (!app.roomInfo || !app.currentRoomId)
            return
        if (app.roomInfo.roomId !== app.currentRoomId)
            app.roomInfo.roomId = app.currentRoomId
    }

    function filteredMembers() {
        // Reading the snapshot keeps the binding live when the async roster
        // arrives. filterMembers is case-insensitive and returns its native
        // avatar/member shape.
        var snapshot = app.roomInfo.members
        var rows = app.roomInfo.filterMembers(memberNeedle)
        var joined = []
        for (var i = 0; i < rows.length; ++i) {
            if (rows[i].membership === "joined") joined.push(rows[i])
        }
        return joined
    }

    function parseLocalDate(text) {
        var match = /^(\d{4})-(\d{2})-(\d{2})$/.exec(text)
        if (!match) return -1
        var year = Number(match[1])
        var month = Number(match[2])
        var day = Number(match[3])
        var date = new Date(year, month - 1, day)
        if (date.getFullYear() !== year || date.getMonth() !== month - 1
                || date.getDate() !== day) return -1
        return date.getTime()
    }

    function clearDraft() {
        draftFrom = []
        draftMentions = []
        draftTypes = []
        draftAfter = ""
        draftBefore = ""
        draftPinned = "any"
        dateError = ""
    }

    function applyFilters() {
        var afterMs = draftAfter.length > 0 ? parseLocalDate(draftAfter) : 0
        var beforeMs = draftBefore.length > 0 ? parseLocalDate(draftBefore) : 0
        if (afterMs < 0 || beforeMs < 0) {
            dateError = qsTr("Use dates in YYYY-MM-DD format.")
            return
        }
        if (afterMs > 0 && beforeMs > 0 && afterMs >= beforeMs) {
            dateError = qsTr("The before date must be later than the after date.")
            return
        }
        dateError = ""
        app.messageSearch.filters = {
            fromUserIds: cloneStrings(draftFrom),
            mentionUserIds: cloneStrings(draftMentions),
            contentTypes: cloneStrings(draftTypes),
            afterDate: draftAfter,
            beforeDate: draftBefore,
            afterMs: afterMs,
            beforeMs: beforeMs,
            pinnedMode: draftPinned,
            pinnedEventIds: app.pinned && app.pinned.roomId === app.currentRoomId
                            ? cloneStrings(app.pinned.ids) : []
        }
        filterEditing = false
        app.messageSearch.search()
    }

    function typeLabel(value) {
        if (value === "image") return qsTr("Image")
        if (value === "video") return qsTr("Video")
        if (value === "audio") return qsTr("Audio")
        if (value === "file") return qsTr("File")
        if (value === "link") return qsTr("Link")
        if (value === "sticker") return qsTr("Sticker")
        return value
    }

    function hasActiveFilters() {
        var value = app.messageSearch.filters || ({})
        return !!((value.fromUserIds && value.fromUserIds.length > 0)
            || (value.mentionUserIds && value.mentionUserIds.length > 0)
            || (value.contentTypes && value.contentTypes.length > 0)
            || (value.afterMs && Number(value.afterMs) > 0)
            || (value.beforeMs && Number(value.beforeMs) > 0)
            || (value.pinnedMode && value.pinnedMode !== "any"))
    }

    function activateResult(index) {
        var row = app.messageSearch.rowAt(index)
        if (row.eventId) root.jumpToEventRequested(row.eventId)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: AppTheme.spacing12
            spacing: AppTheme.spacing8
            Label {
                text: root.filterEditing ? qsTr("Search filters")
                                         : qsTr("Search messages")
                color: AppTheme.textPrimary
                // The shared pane-header role (16), same as the room list
                // header and Room Information beside it.
                font.pixelSize: AppTheme.textTitle
                font.weight: AppTheme.weightBold
                Layout.fillWidth: true
            }
            IconButton {
                visible: !root.filterEditing
                iconName: "settings"
                active: root.hasActiveFilters()
                Accessible.name: qsTr("Search filters")
                ToolTip.text: Accessible.name
                ToolTip.visible: hovered
                ToolTip.delay: 500
                onClicked: root.beginFilters()
            }
            IconButton {
                iconName: "close"
                Accessible.name: root.filterEditing ? qsTr("Cancel filters")
                                                     : qsTr("Close search")
                onClicked: root.filterEditing ? root.filterEditing = false
                                               : root.closeRequested()
            }
        }

        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

        ColumnLayout {
            visible: !root.filterEditing
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: AppTheme.spacing8

            AppTextField {
                id: searchField
                objectName: "roomSearchField"
                Layout.fillWidth: true
                Layout.leftMargin: AppTheme.spacing12
                Layout.rightMargin: AppTheme.spacing12
                Layout.topMargin: AppTheme.spacing12
                searchIcon: true
                clearButton: true
                enabled: root.historyAvailable
                placeholderText: qsTr("Search room history…")
                text: app.messageSearch.query
                onTextChanged: app.messageSearch.query = text
                onAccepted: app.messageSearch.search()
            }

            Rectangle {
                visible: !root.historyAvailable
                Layout.fillWidth: true
                Layout.leftMargin: AppTheme.spacing12
                Layout.rightMargin: AppTheme.spacing12
                implicitHeight: unavailableCol.implicitHeight + AppTheme.spacing12 * 2
                radius: AppTheme.radiusMd
                color: AppTheme.card
                border.width: 1
                border.color: AppTheme.border
                ColumnLayout {
                    id: unavailableCol
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing8
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("The server cannot search end-to-end encrypted room history. You can still find text in messages currently loaded on this device.")
                        color: AppTheme.textSecondary
                        font.pixelSize: AppTheme.textBody
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                        wrapMode: Text.Wrap
                    }
                    AppButton {
                        text: qsTr("Find in loaded messages")
                        onClicked: root.findLoadedRequested()
                    }
                }
            }

            Label {
                visible: root.historyAvailable
                Layout.fillWidth: true
                Layout.leftMargin: AppTheme.spacing12
                Layout.rightMargin: AppTheme.spacing12
                text: qsTr("Server-side search. Additional filters are applied to a bounded result window.")
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                wrapMode: Text.Wrap
            }

            // Absorbs the slack when the results list is hidden — which is
            // the ordinary case in an ENCRYPTED room, where the server
            // cannot search and only the field plus the notice are shown.
            // Without it nothing in this column can stretch, so the OUTER
            // ColumnLayout hands its children an equal share of the panel
            // height and centres each one inside it: the header floated to
            // y=240 and the field to y=1178 of a 2000px panel, which is the
            // reported "search bar not at the top". A layout needs exactly
            // one thing that can grow.
            Item {
                visible: !root.historyAvailable
                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            ListView {
                id: resultsList
                objectName: "roomSearchResultsList"
                visible: root.historyAvailable
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.leftMargin: AppTheme.spacing8
                Layout.rightMargin: AppTheme.spacing8
                clip: true
                spacing: AppTheme.spacing4
                model: app.messageSearch
                keyNavigationEnabled: true
                ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
                onAtYEndChanged: {
                    if (atYEnd && app.messageSearch.canLoadMore)
                        app.messageSearch.loadMore()
                }
                delegate: ItemDelegate {
                    id: resultDelegate
                    required property int index
                    required property string sender
                    required property string senderDisplayName
                    required property string senderAvatarUrl
                    required property var timestampMs
                    required property string body
                    width: resultsList.width
                    implicitHeight: resultRow.implicitHeight + AppTheme.spacing8 * 2
                    hoverEnabled: true
                    onClicked: root.activateResult(index)
                    Accessible.name: qsTr("Open message from %1")
                        .arg(senderDisplayName || sender)
                    background: Rectangle {
                        radius: AppTheme.radiusMd
                        color: resultDelegate.hovered || resultDelegate.highlighted
                               ? AppTheme.hover : "transparent"
                    }
                    contentItem: RowLayout {
                        id: resultRow
                        spacing: AppTheme.spacing8
                        Avatar {
                            size: 30
                            Layout.alignment: Qt.AlignTop
                            mxc: resultDelegate.senderAvatarUrl
                            name: resultDelegate.senderDisplayName
                                  || resultDelegate.sender
                            colorKey: resultDelegate.sender
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    // Remote or externally chosen text: never markup.
                                    textFormat: Text.PlainText
                                    text: resultDelegate.senderDisplayName
                                          || resultDelegate.sender
                                    // Identity ink, as in the timeline.
                                    color: AppTheme.userColor(resultDelegate.sender)
                                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                                    font.weight: AppTheme.weightStrong
                                    elide: Label.ElideRight
                                    Layout.fillWidth: true
                                }
                                Label {
                                    text: new Date(Number(resultDelegate.timestampMs))
                                              .toLocaleDateString(Qt.locale(), Locale.ShortFormat)
                                    color: AppTheme.textMuted
                                    // Was an unscaled 10 beside a scaled
                                    // sender name: at 140% this row sheared.
                                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                                }
                            }
                            Label {
                                // Remote or externally chosen text: never markup.
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                text: resultDelegate.body
                                color: AppTheme.textSecondary
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                                lineHeight: AppTheme.lineHeightBody
                                lineHeightMode: Text.ProportionalHeight
                                wrapMode: Text.Wrap
                                maximumLineCount: 3
                                elide: Label.ElideRight
                            }
                        }
                    }
                }

                footer: Item {
                    width: resultsList.width
                    height: footerCol.implicitHeight + AppTheme.spacing12 * 2
                    ColumnLayout {
                        id: footerCol
                        anchors.centerIn: parent
                        spacing: AppTheme.spacing8
                        AppBusyIndicator {
                            visible: app.messageSearch.state === "loading"
                                     || app.messageSearch.state === "loading_more"
                            running: visible
                            size: 24
                            Layout.alignment: Qt.AlignHCenter
                        }
                        Label {
                            visible: app.messageSearch.state === "no_results"
                                     || app.messageSearch.state === "error"
                            text: app.messageSearch.state === "error"
                                  ? qsTr("Search could not be completed.")
                                  : qsTr("No matching messages")
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.textBody
                        }
                        AppButton {
                            visible: app.messageSearch.canLoadMore
                                     && app.messageSearch.state !== "loading_more"
                            text: qsTr("Load more")
                            onClicked: app.messageSearch.loadMore()
                        }
                    }
                }
            }
        }

        // Scrollable criteria with the action footer outside it.
        ScrollView {
            visible: root.filterEditing
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
            contentWidth: availableWidth

            ColumnLayout {
                width: parent.width
                spacing: AppTheme.spacing16

                SearchMemberSection {
                    Layout.fillWidth: true
                    title: qsTr("From")
                    description: qsTr("Sent by any selected user")
                    sectionKey: "from"
                    selectedValues: root.draftFrom
                    expanded: root.memberSection === sectionKey
                    needle: expanded ? root.memberNeedle : ""
                    members: expanded ? root.filteredMembers() : []
                    onToggleExpanded: {
                        root.memberSection = expanded ? "" : sectionKey
                        root.memberNeedle = ""
                        if (root.memberSection !== "")
                            root.ensureRoster()
                    }
                    onNeedleChangedByUser: (value) => root.memberNeedle = value
                    onUserToggled: (userId) => root.toggleValue("draftFrom", userId)
                }

                SearchMemberSection {
                    Layout.fillWidth: true
                    title: qsTr("Mentions")
                    description: qsTr("Mentions any selected user")
                    sectionKey: "mentions"
                    selectedValues: root.draftMentions
                    expanded: root.memberSection === sectionKey
                    needle: expanded ? root.memberNeedle : ""
                    members: expanded ? root.filteredMembers() : []
                    onToggleExpanded: {
                        root.memberSection = expanded ? "" : sectionKey
                        root.memberNeedle = ""
                        if (root.memberSection !== "")
                            root.ensureRoster()
                    }
                    onNeedleChangedByUser: (value) => root.memberNeedle = value
                    onUserToggled: (userId) => root.toggleValue("draftMentions", userId)
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: AppTheme.spacing12
                    Layout.rightMargin: AppTheme.spacing12
                    spacing: AppTheme.spacing6
                    Label { text: qsTr("Date"); color: AppTheme.textPrimary; font.weight: AppTheme.weightBold }
                    Label { text: qsTr("Use local dates in YYYY-MM-DD format"); color: AppTheme.textMuted; font.pixelSize: AppTheme.textMeta }
                    AppTextField {
                        Layout.fillWidth: true
                        placeholderText: qsTr("On or after, YYYY-MM-DD")
                        text: root.draftAfter
                        inputMethodHints: Qt.ImhDate
                        onTextChanged: root.draftAfter = text
                    }
                    AppTextField {
                        Layout.fillWidth: true
                        placeholderText: qsTr("Before, YYYY-MM-DD")
                        text: root.draftBefore
                        inputMethodHints: Qt.ImhDate
                        onTextChanged: root.draftBefore = text
                    }
                    Label {
                        visible: root.dateError.length > 0
                        Layout.fillWidth: true
                        text: root.dateError
                        color: AppTheme.danger
                        font.pixelSize: AppTheme.textMeta
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                        wrapMode: Text.Wrap
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: AppTheme.spacing12
                    Layout.rightMargin: AppTheme.spacing12
                    spacing: AppTheme.spacing6
                    Label { text: qsTr("Has"); color: AppTheme.textPrimary; font.weight: AppTheme.weightBold }
                    Label { text: qsTr("Contains any selected content type"); color: AppTheme.textMuted; font.pixelSize: AppTheme.textMeta }
                    Flow {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing6
                        Repeater {
                            model: ["image", "video", "audio", "file", "link", "sticker"]
                            CheckBox {
                                required property string modelData
                                // Native control on a themed surface — the
                                // label ink must follow the app palette
                                // (SettingsScreen convention), or it
                                // renders OS-default gray on dark themes.
                                palette.windowText: AppTheme.textPrimary
                                text: root.typeLabel(modelData)
                                checked: root.contains(root.draftTypes, modelData)
                                onClicked: root.toggleValue("draftTypes", modelData)
                            }
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: AppTheme.spacing12
                    Layout.rightMargin: AppTheme.spacing12
                    Layout.bottomMargin: AppTheme.spacing12
                    spacing: AppTheme.spacing6
                    Label { text: qsTr("Pinned"); color: AppTheme.textPrimary; font.weight: AppTheme.weightBold }
                    Label { text: qsTr("Whether the message is pinned"); color: AppTheme.textMuted; font.pixelSize: AppTheme.textMeta }
                    AppComboBox {
                        Layout.fillWidth: true
                        model: [qsTr("Any"), qsTr("Pinned"), qsTr("Not pinned")]
                        currentIndex: root.draftPinned === "pinned" ? 1
                                      : root.draftPinned === "not_pinned" ? 2 : 0
                        onActivated: (index) => {
                            root.draftPinned = index === 1 ? "pinned"
                                               : index === 2 ? "not_pinned" : "any"
                        }
                    }
                }
            }
        }

        Rectangle {
            visible: root.filterEditing
            Layout.fillWidth: true
            implicitHeight: 1
            color: AppTheme.border
        }
        RowLayout {
            visible: root.filterEditing
            Layout.fillWidth: true
            Layout.margins: AppTheme.spacing12
            spacing: AppTheme.spacing8
            AppButton {
                text: qsTr("Clear filters")
                onClicked: root.clearDraft()
            }
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Cancel")
                onClicked: root.filterEditing = false
            }
            AppButton {
                text: qsTr("Apply filters")
                kind: "primary"
                enabled: root.historyAvailable
                onClicked: root.applyFilters()
            }
        }
    }
}
