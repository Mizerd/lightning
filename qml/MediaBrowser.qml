import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The room's media, files and links across the whole accessible history, not
// just what the timeline has loaded. Drives MediaHistoryModel, which walks
// /messages on its own cursor (rust/src/mediahistory.rs) and never moves the
// reader's timeline. Completeness is shown, not implied: the strip under the
// toolbar says how much history was examined and whether the start was reached,
// and undecryptableCount reports history that exists but cannot be read.
Item {
    id: root

    /// The model (app.mediaHistory), passed in so this component has no global
    /// lookups and can be tested.
    property var model: null
    /// The room whose history is browsed.
    property string roomId: ""
    /// Ask the host to open the image viewer at `index` of `entries`, this
    /// browser's own image list in view order. Passing a key alone made the
    /// viewer search the timeline's loaded images, which do not contain this
    /// history.
    signal openImagesRequested(var entries, int index)
    /// Ask the host to jump the timeline to this event via its existing
    /// search/permalink path; this component never navigates.
    signal jumpToEventRequested(string eventId)

    readonly property bool ready: model !== null && model.available

    // Categories. "media" is the combined visual view; the individual ones stay
    // reachable.
    readonly property var categories: [
        { key: "media",  label: qsTr("Media") },
        { key: "image",  label: qsTr("Images") },
        { key: "video",  label: qsTr("Videos") },
        { key: "audio",  label: qsTr("Audio") },
        { key: "gif",    label: qsTr("GIFs") },
        { key: "sticker", label: qsTr("Stickers") },
        { key: "file",   label: qsTr("Files") },
        { key: "link",   label: qsTr("Links") },
    ]
    property string category: "media"
    /// Grid for visual categories, list for the rest.
    property bool gridMode: category === "media" || category === "image"
                            || category === "video" || category === "gif"
                            || category === "sticker"

    // Push the initial value too: onCategoryChanged only fires on change.
    onCategoryChanged: root.applyCategory()
    onModelChanged: root.applyCategory()
    Component.onCompleted: root.applyCategory()

    /// Why the date fields are not applied ("" when they are, or are empty).
    property string dateError: ""

    /// Pushes both fields to the model, which owns the day semantics and the
    /// parsing. An invalid entry leaves the previous range in force and says
    /// so, rather than silently showing everything.
    function applyDates() {
        if (!model)
            return
        var from = fromField.text.trim()
        var to = toField.text.trim()
        // Half-typed dates are not errors yet.
        var partial = (from.length > 0 && from.length < 10)
                      || (to.length > 0 && to.length < 10)
        if (partial) {
            dateError = ""
            return
        }
        if (model.setDateRange(from, to)) {
            dateError = ""
        } else {
            dateError = qsTr("Use real dates in YYYY-MM-DD format, with From not after To.")
        }
    }

    function applyCategory() {
        if (model)
            model.category = category
    }
    onRoomIdChanged: {
        if (!model)
            return
        model.roomId = roomId
        // Fetch the first page when shown, not on room change.
        if (visible)
            model.loadMore()
    }
    // How long the panel may stay hidden before a re-show rescans the room.
    // Switching Room info sections away and back is shorter than this and keeps
    // the reader's place and the pages already loaded.
    property int staleAfterMs: 30000
    property double _hiddenAt: 0
    property string _hiddenRoom: ""
    onVisibleChanged: {
        if (!model)
            return
        if (!visible) {
            _hiddenAt = Date.now()
            _hiddenRoom = roomId
            return
        }
        if (model.loadedCount > 0 || model.complete) {
            // Shown again after being hidden: what the model holds is the room
            // as it was at the last scan, and nothing appends later messages to
            // it. Rescan from the live edge only when it can plausibly be
            // stale: the room changed, or the panel was away for a while.
            var away = _hiddenAt > 0 ? Date.now() - _hiddenAt : staleAfterMs
            if (_hiddenRoom !== roomId || away >= staleAfterMs)
                model.reload()
        } else {
            model.loadMore()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: AppTheme.spacing8

        // Category tabs in a Flow, so all six stay one click away in a narrow
        // panel.
        Flow {
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing12
            Layout.rightMargin: AppTheme.spacing12
            Layout.topMargin: AppTheme.spacing8
            spacing: AppTheme.spacing4
            Repeater {
                model: root.categories
                delegate: AppButton {
                    required property var modelData
                    text: modelData.label
                    // The selected category is a real button; the rest are
                    // ghosts.
                    kind: root.category === modelData.key ? "secondary" : "ghost"
                    size: "sm"
                    onClicked: root.category = modelData.key
                    Accessible.name: modelData.label
                }
            }
        }

        // Search and filters
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing12
            Layout.rightMargin: AppTheme.spacing12
            spacing: AppTheme.spacing8

            AppTextField {
                id: searchField
                Layout.fillWidth: true
                placeholderText: qsTr("Search filename, caption, sender, link…")
                onTextChanged: if (root.model) root.model.query = text
                Accessible.name: qsTr("Search this room's media")
            }
            AppButton {
                objectName: "mediaBrowserViewToggle"
                kind: "ghost"
                size: "sm"
                iconName: root.gridMode ? "view_list" : "grid_view"
                text: root.gridMode ? qsTr("List") : qsTr("Grid")
                onClicked: root.gridMode = !root.gridMode
                Accessible.name: root.gridMode ? qsTr("Show as a list")
                                               : qsTr("Show as a grid")
            }
        }

        // Date range: a message is kept when its day, in this computer's time
        // zone, is From <= day <= To (To includes the whole of its day).
        RowLayout {
            objectName: "mediaBrowserDateRow"
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing12
            Layout.rightMargin: AppTheme.spacing12
            spacing: AppTheme.spacing8
            Label {
                text: qsTr("Dates")
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
            }
            AppTextField {
                id: fromField
                objectName: "mediaBrowserFromDate"
                Layout.fillWidth: true
                placeholderText: qsTr("From YYYY-MM-DD")
                inputMethodHints: Qt.ImhDate
                onTextChanged: root.applyDates()
                Accessible.name: qsTr("Show media from this date")
            }
            AppTextField {
                id: toField
                objectName: "mediaBrowserToDate"
                Layout.fillWidth: true
                placeholderText: qsTr("To YYYY-MM-DD")
                inputMethodHints: Qt.ImhDate
                onTextChanged: root.applyDates()
                Accessible.name: qsTr("Show media up to and including this date")
            }
        }
        Label {
            objectName: "mediaBrowserDateError"
            visible: root.dateError.length > 0
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing12
            Layout.rightMargin: AppTheme.spacing12
            text: root.dateError
            color: AppTheme.danger
            font.pixelSize: AppTheme.textMeta
            wrapMode: Text.WordWrap
        }

        // Sender filter, offered once more than one person appears.
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing12
            Layout.rightMargin: AppTheme.spacing12
            spacing: AppTheme.spacing8
            visible: root.model && root.model.knownSenders.length > 1
            Label {
                text: qsTr("From")
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
            }
            AppComboBox {
                id: senderBox
                Layout.fillWidth: true
                // Known senders are data: a same-size rebuild is a real
                // change (AppComboBox).
                restoreAfterModelReset: false
                // "Anyone" is index 0 and is not a user id.
                model: [qsTr("Anyone")].concat(
                    root.model ? root.model.knownSenders : [])
                onActivated: {
                    if (!root.model)
                        return
                    root.model.senderFilter =
                        currentIndex <= 0 ? "" : textAt(currentIndex)
                }
                Accessible.name: qsTr("Filter by sender")
            }
        }

        // The completeness line
        Label {
            objectName: "mediaBrowserCoverage"
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing12
            Layout.rightMargin: AppTheme.spacing12
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textMeta
            text: {
                if (!root.model)
                    return ""
                if (!root.model.available)
                    return qsTr("This backend cannot browse room history.")
                if (root.model.lastError.length > 0)
                    return qsTr("History could not be read, so this is only "
                                + "part of it. %1").arg(root.model.lastError)
                var shown = root.model.shownCount
                var scanned = root.model.scannedTotal
                var parts = []
                if (root.model.complete) {
                    parts.push(qsTr("%n item(s) — all of this room's history.",
                                    "", shown))
                } else if (root.model.loading) {
                    parts.push(qsTr("%n item(s) so far — reading older messages…",
                                    "", shown))
                } else {
                    parts.push(qsTr("%n item(s) in the %1 message(s) read so far.",
                                    "", shown).arg(scanned))
                }
                if (root.model.undecryptableCount > 0) {
                    // Never let missing keys quietly shorten the list.
                    parts.push(qsTr("%n message(s) could not be decrypted and "
                                    + "may hold more.", "",
                                    root.model.undecryptableCount))
                }
                return parts.join(" ")
            }
        }

        // Older history is reached on request as well as by scrolling: a room
        // whose first pages hold a handful of items never overflows the view,
        // so nothing would ever scroll to ask for more. "Load more" is one
        // page; "Show all media" keeps going, bounded, until the start of
        // history, and Stop ends it.
        RowLayout {
            objectName: "mediaBrowserPagingRow"
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing12
            Layout.rightMargin: AppTheme.spacing12
            spacing: AppTheme.spacing8
            visible: root.model && root.model.available && !root.model.complete
                     && root.model.lastError.length === 0
            AppButton {
                objectName: "mediaBrowserLoadMore"
                kind: "secondary"
                size: "sm"
                text: qsTr("Load more")
                enabled: !!root.model && !root.model.loading
                onClicked: if (root.model) root.model.loadMore()
                Accessible.name: qsTr("Load more media from older messages")
            }
            AppButton {
                objectName: "mediaBrowserShowAll"
                kind: "ghost"
                size: "sm"
                text: root.model && root.model.walking ? qsTr("Stop")
                                                       : qsTr("Show all media")
                onClicked: {
                    if (!root.model)
                        return
                    if (root.model.walking)
                        root.model.cancelWalk()
                    else
                        root.model.loadAll()
                }
                Accessible.name: root.model && root.model.walking
                    ? qsTr("Stop searching older messages")
                    : qsTr("Search all older messages for media")
            }
        }

        // The results
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            // Empty state, specific about which kind of empty.
            Label {
                anchors.centerIn: parent
                width: parent.width - AppTheme.spacing24 * 2
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                visible: root.model && root.model.shownCount === 0
                         && !root.model.loading
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textBody
                text: {
                    if (!root.model)
                        return ""
                    if (root.model.query.length > 0
                        || root.model.senderFilter.length > 0
                        || root.model.fromDate.toString().length > 0
                        || root.model.toDate.toString().length > 0)
                        return qsTr("Nothing matches those filters in the "
                                    + "history read so far.")
                    return root.model.complete
                        ? qsTr("Nothing of this kind in this room.")
                        : qsTr("Nothing of this kind yet — keep loading to "
                               + "search further back.")
                }
            }

            GridView {
                id: grid
                anchors.fill: parent
                // Aligned with the header's margin: the tile draws 2 px inside
                // its cell.
                anchors.leftMargin: AppTheme.spacing12 - 2
                visible: root.gridMode
                model: root.model
                clip: true
                // The view is the one Tab stop; arrows, Home/End and Page keys
                // move the current tile, Return opens it, Menu opens its menu.
                activeFocusOnTab: true
                keyNavigationEnabled: true
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Media")
                Keys.onReturnPressed: root.activate(currentIndex)
                Keys.onEnterPressed: root.activate(currentIndex)
                Keys.onSpacePressed: root.activate(currentIndex)
                Keys.onMenuPressed: if (currentItem) currentItem.openMenu()
                Keys.onPressed: (event) => root.navigate(this, event)
                cellWidth: Math.max(96, Math.floor(width / Math.max(1,
                    Math.floor(width / 116))))
                cellHeight: cellWidth
                ScrollBar.vertical: AppScrollBar {}
                SmoothWheelArea {}
                // Paginate when the end comes into view. loadMore() is a no-op
                // while a page is in flight or the walk is done.
                onContentYChanged: {
                    if (contentY + height > contentHeight - cellHeight * 2)
                        root.requestMore()
                }
                delegate: MediaBrowserTile {
                    width: grid.cellWidth
                    height: grid.cellHeight
                    onActivated: { grid.currentIndex = index; root.activate(index) }
                    onJumpRequested: root.jumpToEventRequested(eventId)
                }
            }

            ListView {
                id: list
                anchors.fill: parent
                visible: !root.gridMode
                model: root.model
                clip: true
                spacing: 0
                activeFocusOnTab: true
                keyNavigationEnabled: true
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Media")
                Keys.onReturnPressed: root.activate(currentIndex)
                Keys.onEnterPressed: root.activate(currentIndex)
                Keys.onSpacePressed: root.activate(currentIndex)
                Keys.onMenuPressed: if (currentItem) currentItem.openMenu()
                Keys.onPressed: (event) => root.navigate(this, event)
                ScrollBar.vertical: AppScrollBar {}
                SmoothWheelArea {}
                onContentYChanged: {
                    if (contentY + height > contentHeight - height)
                        root.requestMore()
                }
                // Date headers, locale-resolved in C++ (DateGroupRole).
                section.property: "dateGroup"
                section.criteria: ViewSection.FullString
                section.delegate: Rectangle {
                    width: ListView.view.width
                    height: sectionLabel.implicitHeight + AppTheme.spacing8 * 2
                    color: AppTheme.background
                    Label {
                        id: sectionLabel
                        anchors.left: parent.left
                        anchors.leftMargin: AppTheme.spacing12
                        anchors.verticalCenter: parent.verticalCenter
                        text: section
                        textFormat: Text.PlainText
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.textMeta
                        font.weight: AppTheme.weightStrong
                    }
                }
                delegate: MediaBrowserRow {
                    width: ListView.view.width
                    onActivated: { list.currentIndex = index; root.activate(index) }
                    onJumpRequested: root.jumpToEventRequested(eventId)
                }
            }

            // Middle-click autoscroll, as in the timeline and the room list.
            // A sibling over the views, which stay the ones drawn and focused.
            MiddleClickScroller {
                objectName: "mediaBrowserMiddleClickScroller"
                anchors.fill: parent
                z: latched ? 30 : 2
                view: root.gridMode ? grid : list
            }

            // A quiet spinner; the grid stays usable while older history loads.
            BusyIndicator {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: AppTheme.spacing12
                running: root.model && root.model.loading
                visible: running
                implicitWidth: 24
                implicitHeight: 24
            }
        }

        // A failed page means the rest of history is unknown, so offer Retry.
        AppButton {
            Layout.alignment: Qt.AlignHCenter
            Layout.bottomMargin: AppTheme.spacing12
            visible: root.model && root.model.lastError.length > 0
            text: qsTr("Try again")
            onClicked: if (root.model) root.model.loadMore()
        }
    }

    // A view whose content does not fill it cannot scroll, so the scroll
    // handlers never ask for the next page. Ask for it here, a few pages at
    // most (the model bounds it) and re-evaluated after every page.
    function fillIfShort() {
        if (!model || !root.visible)
            return
        var v = root.gridMode ? grid : list
        if (v.contentHeight <= v.height + 1)
            model.fillViewport()
    }
    Connections {
        target: root.model
        ignoreUnknownSignals: true
        function onStateChanged() { Qt.callLater(root.fillIfShort) }
        function onFiltersChanged() { Qt.callLater(root.fillIfShort) }
    }
    onGridModeChanged: Qt.callLater(root.fillIfShort)

    /// Home, End and the Page keys, which neither view's keyNavigation covers
    /// (arrows it does). A page is what the view shows.
    function navigate(view, event) {
        var n = view.count
        if (n <= 0)
            return
        var target = -1
        if (event.key === Qt.Key_Home) {
            target = 0
        } else if (event.key === Qt.Key_End) {
            target = n - 1
        } else if (event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown) {
            var step
            if (view.cellWidth !== undefined) {
                step = Math.max(1, Math.floor(view.height / view.cellHeight))
                       * Math.max(1, Math.floor(view.width / view.cellWidth))
            } else {
                var h = view.currentItem ? Math.max(1, view.currentItem.height) : 48
                step = Math.max(1, Math.floor(view.height / h))
            }
            var dir = event.key === Qt.Key_PageUp ? -1 : 1
            target = Math.max(0, Math.min(n - 1, view.currentIndex + dir * step))
        }
        if (target < 0)
            return
        view.currentIndex = target
        view.positionViewAtIndex(target, view.cellWidth !== undefined
                                 ? GridView.Contain : ListView.Contain)
        event.accepted = true
    }

    function requestMore() {
        if (model && !model.loading && !model.complete)
            model.loadMore()
    }

    /// An image opens in the viewer; anything else jumps to its message.
    function activate(row) {
        if (!model)
            return
        var entry = model.entryAt(row)
        if (!entry || !entry.eventId)
            return
        if (entry.kind === "image") {
            // The index comes from the model, which owns the ordering. The
            // bridge is keyed by event id (an encrypted room's mxc is not
            // fetchable).
            var index = model.imageIndexForRow(row)
            if (index < 0)
                return
            root.openImagesRequested(model.imageEntries(), index)
        } else {
            root.jumpToEventRequested(entry.eventId)
        }
    }
}
