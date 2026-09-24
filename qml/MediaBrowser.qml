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
        { key: "file",   label: qsTr("Files") },
        { key: "link",   label: qsTr("Links") },
    ]
    property string category: "media"
    /// Grid for visual categories, list for the rest.
    property bool gridMode: category === "media" || category === "image"
                            || category === "video"

    // Push the initial value too: onCategoryChanged only fires on change.
    onCategoryChanged: root.applyCategory()
    onModelChanged: root.applyCategory()
    Component.onCompleted: root.applyCategory()

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
    onVisibleChanged: {
        if (visible && model && model.loadedCount === 0 && !model.complete)
            model.loadMore()
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
                        || root.model.senderFilter.length > 0)
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
                visible: root.gridMode
                model: root.model
                clip: true
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
                    onActivated: root.activate(index)
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
                    onActivated: root.activate(index)
                    onJumpRequested: root.jumpToEventRequested(eventId)
                }
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
