import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Reusable Matrix user search, used by the New Conversation and Invite People
// dialogs. Debounce, stale-result rejection and deduplication live in C++
// (UserSearchModel); this provides the input, results, keyboard navigation and
// selection signal. All instances share one UserSearchModel
// (app.conversations.userSearch); owners keep at most one picker visible and
// clear the search when switching.
ColumnLayout {
    id: root
    spacing: AppTheme.spacing8

    // Emitted on click or Enter. avatarUrl is additive, so two-argument
    // handlers still work.
    signal userSelected(string userId, string displayName, string avatarUrl)

    property alias searchText: searchField.text
    readonly property var model: app.conversations.userSearch
    // The New Conversation omnibox reuses this instance and its objectNames
    // with a different treatment, so CreationDialogQmlTest's
    // "<objectName>SearchField" lookups keep working.
    property bool omniboxStyle: false

    function clear() {
        app.conversations.userSearch.clear()
        searchField.text = ""
        resultsList.currentIndex = -1
    }
    function focusSearch() {
        searchField.forceActiveFocus()
    }

    // HTML-escape untrusted display names before the StyledText highlight, then
    // tint the matched substring. The caller passes bolt for the highlighted
    // row only.
    function escapeHtml(s) {
        return String(s)
            .replace(/&/g, "&amp;").replace(/</g, "&lt;")
            .replace(/>/g, "&gt;").replace(/"/g, "&quot;")
    }
    function highlightedName(text, query, tintColor) {
        var safe = escapeHtml(text)
        var q = (query || "").trim()
        if (q.length === 0) return safe
        var lowerSafe = safe.toLowerCase()
        var lowerQ = escapeHtml(q).toLowerCase()
        var idx = lowerSafe.indexOf(lowerQ)
        if (idx === -1) return safe
        var tint = (tintColor && ("" + tintColor).length > 0)
                   ? ("" + tintColor) : ("" + AppTheme.stormText)
        return safe.slice(0, idx) + "<font color=\"" + tint + "\">"
             + safe.slice(idx, idx + lowerQ.length) + "</font>"
             + safe.slice(idx + lowerQ.length)
    }

    AppTextField {
        id: searchField
        objectName: root.objectName.length > 0
                    ? root.objectName + "SearchField" : "userPickerSearchField"
        Layout.fillWidth: true
        searchIcon: !root.omniboxStyle
        clearButton: true
        leftPadding: root.omniboxStyle ? 38 : (searchIcon ? 32 : 12)
        // Title size for the omnibox, which is the dialog's subject; body size
        // elsewhere.
        font.pixelSize: root.omniboxStyle ? AppTheme.textTitle
                                          : AppTheme.textBody
        placeholderText: root.omniboxStyle
            ? qsTr("Type a name, an @user ID, or a #room address…")
            : qsTr("Search people, or enter a full Matrix ID…")
        Accessible.name: qsTr("Search for a user")
        // stormInset field with a bolt focus border and halo. The omnibox's
        // larger radius and leading glyph are overridden here rather than in
        // AppTextField.
        storm: true
        background: Rectangle {
            radius: root.omniboxStyle ? AppTheme.radiusOmnibox : AppTheme.radiusMd
            color: AppTheme.stormInset
            border.width: searchField.activeFocus ? 1.5 : 1
            border.color: searchField.activeFocus ? AppTheme.bolt
                          : (root.omniboxStyle || searchField.hovered)
                            ? AppTheme.stormBorderStrong
                          : AppTheme.stormBorder
            // Focus halo: an outside ring, never field geometry.
            Rectangle {
                visible: searchField.activeFocus
                anchors.fill: parent
                anchors.margins: -3
                radius: parent.radius + 3
                color: "transparent"
                border.width: 3
                border.color: AppTheme.stormBoltGlow
            }
        }
        Icon {
            visible: root.omniboxStyle
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            name: "bolt"
            size: 18
            // The omnibox glyph inks bolt while focused.
            color: searchField.activeFocus ? AppTheme.bolt
                                           : AppTheme.stormTextMuted
        }
        onTextChanged: {
            app.conversations.userSearch.query = text
            resultsList.currentIndex = -1
        }
        Keys.onDownPressed: {
            if (resultsList.count > 0) {
                resultsList.currentIndex =
                        Math.min(resultsList.currentIndex + 1, resultsList.count - 1)
            }
        }
        Keys.onUpPressed: {
            if (resultsList.count > 0)
                resultsList.currentIndex = Math.max(resultsList.currentIndex - 1, 0)
        }
        Keys.onReturnPressed: (event) => {
            if (resultsList.count > 0) {
                var row = resultsList.currentIndex >= 0 ? resultsList.currentIndex : 0
                resultsList.selectRow(row)
                event.accepted = true
            } else {
                event.accepted = false
            }
        }
    }

    // State line: loading / no results / error.
    Label {
        objectName: root.objectName.length > 0
                    ? root.objectName + "StateLabel" : "userPickerStateLabel"
        visible: text.length > 0
        Layout.fillWidth: true
        text: {
            var s = app.conversations.userSearch.state
            if (s === "loading") return qsTr("Searching…")
            if (s === "no_results") return qsTr("No results")
            if (s === "error") return qsTr("Search failed. Check your connection and try again.")
            return ""
        }
        color: app.conversations.userSearch.state === "error"
               ? AppTheme.stormDanger : AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textBody
    }

    ListView {
        id: resultsList
        objectName: root.objectName.length > 0
                    ? root.objectName + "Results" : "userPickerResults"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, 240)
        visible: count > 0
        clip: true
        model: root.model
        currentIndex: -1
        keyNavigationEnabled: true
        ScrollBar.vertical: AppScrollBar { thin: true; policy: ScrollBar.AsNeeded }

        function selectRow(row) {
            var userId = root.model.userIdAt(row)
            if (userId && userId.length > 0) {
                // The avatar comes from the visible delegate (the highlighted
                // row is instantiated).
                var delegateItem = resultsList.itemAtIndex(row)
                root.userSelected(userId, root.model.displayNameAt(row) || "",
                                  delegateItem ? delegateItem.rowAvatarUrl : "")
            }
        }

        delegate: ItemDelegate {
            id: row
            width: ListView.view.width
            highlighted: ListView.isCurrentItem
            readonly property string rowAvatarUrl: model.avatarUrl || ""
            onClicked: root.userSelected(model.userId, model.displayName || "",
                                         rowAvatarUrl)
            Accessible.name: model.displayName && model.displayName.length > 0
                             ? qsTr("%1 (%2)").arg(model.displayName).arg(model.userId)
                             : model.userId

            contentItem: RowLayout {
                spacing: AppTheme.spacing8
                Avatar {
                    size: 32
                    name: (model.displayName && model.displayName.length > 0)
                          ? model.displayName
                          : (model.userId.length > 1 ? model.userId.slice(1) : "?")
                    mxc: model.avatarUrl || ""
                    colorKey: model.userId
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Label {
                        Layout.fillWidth: true
                        textFormat: Text.StyledText
                        text: root.highlightedName(
                            model.displayName && model.displayName.length > 0
                                ? model.displayName : model.userId,
                            root.model.query,
                            row.highlighted ? "" + AppTheme.bolt
                                            : "" + AppTheme.stormText)
                        color: row.highlighted ? AppTheme.stormText
                                               : AppTheme.stormTextSecondary
                        font.family: AppTheme.menuFont
                        font.pixelSize: AppTheme.textSubtitle
                        font.weight: AppTheme.weightStrong
                        elide: Label.ElideRight
                    }
                    Label {
                        // Untrusted text: never markup.
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        visible: model.displayName && model.displayName.length > 0
                        text: model.userId
                        color: AppTheme.stormTextMuted
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.fontMonoXS
                        elide: Label.ElideMiddle
                    }
                }
                // Provenance chip: directory result vs confirmed exact lookup.
                StatusChip {
                    readonly property string src: model.source || "directory"
                    storm: true
                    tone: "info"
                    visible: src !== "directory"
                    label: src === "exact_local" ? qsTr("From your server")
                         : src === "exact_mxid"  ? qsTr("Exact Matrix ID")
                         : ""
                }
            }
            background: Rectangle {
                color: (row.highlighted || row.hovered)
                       ? AppTheme.stormSelection : "transparent"
                radius: AppTheme.radiusSm
            }
        }
    }
}
