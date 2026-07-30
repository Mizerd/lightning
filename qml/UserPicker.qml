import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.5.9: reusable Matrix user search (Phase 7). Debounce, stale-result
// rejection and duplicate removal live in C++ (UserSearchModel); this
// component provides the input, result list, keyboard navigation and
// selection signal. Used by the New Conversation and Invite People dialogs.
//
// ALL instances share ONE UserSearchModel (app.conversations.userSearch);
// owners keep at most one picker visible at a time and clear the search
// when switching surfaces.
ColumnLayout {
    id: root
    spacing: AppTheme.spacing8

    // Emitted when the user picks a result row (click or Enter). Existing
    // two-argument handlers keep working; avatarUrl is additive.
    signal userSelected(string userId, string displayName, string avatarUrl)

    property alias searchText: searchField.text
    readonly property var model: app.conversations.userSearch
    // v0.6.5 (SPEC 1u): the New Conversation omnibox reuses this exact
    // instance/objectNames with a different border/icon/type treatment
    // instead of a separate field, so every CreationDialogQmlTest assertion
    // keyed on "<objectName>SearchField" keeps working untouched.
    property bool omniboxStyle: false

    function clear() {
        app.conversations.userSearch.clear()
        searchField.text = ""
        resultsList.currentIndex = -1
    }
    function focusSearch() {
        searchField.forceActiveFocus()
    }

    // HTML-escape untrusted display names before any StyledText highlight,
    // then tint the matched query substring accent (SPEC 1t typeahead).
    function escapeHtml(s) {
        return String(s)
            .replace(/&/g, "&amp;").replace(/</g, "&lt;")
            .replace(/>/g, "&gt;").replace(/"/g, "&quot;")
    }
    function highlightedName(text, query) {
        var safe = escapeHtml(text)
        var q = (query || "").trim()
        if (q.length === 0) return safe
        var lowerSafe = safe.toLowerCase()
        var lowerQ = escapeHtml(q).toLowerCase()
        var idx = lowerSafe.indexOf(lowerQ)
        if (idx === -1) return safe
        return safe.slice(0, idx) + "<font color=\"" + AppTheme.accent + "\">"
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
        font.pixelSize: root.omniboxStyle ? 14 : 13
        placeholderText: root.omniboxStyle
            ? qsTr("Type a name, an @user ID, or a #room address…")
            : qsTr("Search people, or enter a full Matrix ID…")
        Accessible.name: qsTr("Search for a user")
        // The omnibox treatment (SPEC 1u): 1.5px accent border, radiusOmnibox,
        // a leading bolt glyph instead of the plain search icon — overridden
        // from here rather than editing AppTextField.qml (lead-owned).
        background: Rectangle {
            radius: root.omniboxStyle ? AppTheme.radiusOmnibox : AppTheme.radiusMd
            color: AppTheme.inputBackground
            border.width: root.omniboxStyle ? 1.5
                          : (searchField.activeFocus ? 2 : 1)
            border.color: root.omniboxStyle ? AppTheme.accent
                          : (searchField.activeFocus ? AppTheme.focusRing
                             : searchField.hovered ? AppTheme.borderStrong
                             : AppTheme.border)
        }
        Icon {
            visible: root.omniboxStyle
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            name: "bolt"
            size: 18
            color: AppTheme.accent
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

    // State line: loading / no results / error. Results replace it.
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
               ? AppTheme.danger : AppTheme.textMuted
        font.pixelSize: AppTheme.fontSizeS
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
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        function selectRow(row) {
            var userId = root.model.userIdAt(row)
            if (userId && userId.length > 0) {
                // Avatar comes from the visible delegate (the model keeps
                // avatarUrl as a role; the highlighted row is instantiated).
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
                            root.model.query)
                        color: AppTheme.textPrimary
                        font.pixelSize: AppTheme.fontSizeM
                        elide: Label.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: model.displayName && model.displayName.length > 0
                        text: model.userId
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.fontSizeS
                        elide: Label.ElideMiddle
                    }
                }
                // v0.5.11: provenance chip so the user understands where a
                // result came from (directory vs a confirmed exact lookup).
                Label {
                    readonly property string src: model.source || "directory"
                    visible: src !== "directory"
                    text: src === "exact_local" ? qsTr("From your server")
                        : src === "exact_mxid"  ? qsTr("Exact Matrix ID")
                        : ""
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.fontSizeXS
                }
            }
            background: Rectangle {
                color: row.highlighted ? AppTheme.selected
                     : row.hovered ? AppTheme.hover : "transparent"
                radius: AppTheme.radiusSm
            }
        }
    }
}
