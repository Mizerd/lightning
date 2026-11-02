import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

ColumnLayout {
    id: root
    spacing: AppTheme.spacing8

    signal userSelected(string userId, string displayName, string avatarUrl)

    property alias searchText: searchField.text
    readonly property var model: app.conversations.userSearch
    property bool omniboxStyle: false
    property var suggestions: []
    property string suggestionsTitle: qsTr("Recent conversations")
    readonly property bool showingSuggestions:
        searchField.text.trim().length === 0 && root.suggestions.length > 0

    function clear() {
        app.conversations.userSearch.clear()
        searchField.text = ""
        resultsList.currentIndex = -1
        suggestionList.currentIndex = -1
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
        font.pixelSize: root.omniboxStyle ? AppTheme.textTitle
                                          : AppTheme.textBody
        placeholderText: root.omniboxStyle
            ? qsTr("Type a name, an @user ID, or a #room address…")
            : qsTr("Search people, or enter a full Matrix ID…")
        Accessible.name: qsTr("Search for a user")
        storm: true
        background: Rectangle {
            radius: root.omniboxStyle ? AppTheme.radiusOmnibox : AppTheme.radiusMd
            color: AppTheme.stormInset
            border.width: searchField.activeFocus ? 1.5 : 1
            border.color: searchField.activeFocus ? AppTheme.bolt
                          : (root.omniboxStyle || searchField.hovered)
                            ? AppTheme.stormBorderStrong
                          : AppTheme.stormBorder
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
            color: searchField.activeFocus ? AppTheme.bolt
                                           : AppTheme.stormTextMuted
        }
        onTextChanged: {
            app.conversations.userSearch.query = text
            resultsList.currentIndex = -1
        }
        Keys.onDownPressed: {
            if (root.showingSuggestions) {
                suggestionList.currentIndex =
                        Math.min(suggestionList.currentIndex + 1, suggestionList.count - 1)
                return
            }
            if (resultsList.count > 0) {
                resultsList.currentIndex =
                        Math.min(resultsList.currentIndex + 1, resultsList.count - 1)
            }
        }
        Keys.onUpPressed: {
            if (root.showingSuggestions) {
                suggestionList.currentIndex = Math.max(suggestionList.currentIndex - 1, -1)
                return
            }
            if (resultsList.count > 0)
                resultsList.currentIndex = Math.max(resultsList.currentIndex - 1, 0)
        }
        Keys.onReturnPressed: (event) => {
            if (root.showingSuggestions) {
                if (suggestionList.currentIndex >= 0) {
                    suggestionList.selectRow(suggestionList.currentIndex)
                    event.accepted = true
                } else {
                    event.accepted = false
                }
                return
            }
            if (resultsList.count > 0) {
                var row = resultsList.currentIndex >= 0 ? resultsList.currentIndex : 0
                resultsList.selectRow(row)
                event.accepted = true
            } else {
                event.accepted = false
            }
        }
    }

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

    Label {
        objectName: root.objectName.length > 0
                    ? root.objectName + "SuggestionsTitle" : "userPickerSuggestionsTitle"
        visible: root.showingSuggestions
        Layout.fillWidth: true
        text: root.suggestionsTitle
        color: AppTheme.stormTextMuted
        font.family: AppTheme.menuSectionFont
        font.pixelSize: AppTheme.menuSectionSize
        font.weight: AppTheme.menuSectionWeight
        font.letterSpacing: AppTheme.menuSectionTracking
    }
    ListView {
        id: suggestionList
        objectName: root.objectName.length > 0
                    ? root.objectName + "Suggestions" : "userPickerSuggestions"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, 240)
        visible: root.showingSuggestions
        clip: true
        model: root.showingSuggestions ? root.suggestions : []
        currentIndex: -1
        Accessible.role: Accessible.List
        Accessible.name: root.suggestionsTitle
        ScrollBar.vertical: AppScrollBar { thin: true; policy: ScrollBar.AsNeeded }

        function selectRow(row) {
            var s = root.suggestions[row]
            if (!s || !s.userId || s.userId.length === 0)
                return
            var item = suggestionList.itemAtIndex(row)
            root.userSelected(s.userId,
                              item ? item.shownName : (s.displayName || ""),
                              item ? item.shownAvatar : (s.avatarUrl || ""))
        }

        delegate: ItemDelegate {
            id: suggestionRow
            required property var modelData
            required property int index
            objectName: "userPickerSuggestion_" + index
            width: ListView.view.width
            highlighted: ListView.isCurrentItem
            readonly property var profiles: typeof app !== "undefined" && app
                                            ? app.userProfiles : null
            property var resolvedProfile: profiles
                ? profiles.lookup(modelData.userId) : ({})
            Connections {
                target: suggestionRow.profiles
                function onResolved(userId, displayName, avatarUrl) {
                    if (userId === suggestionRow.modelData.userId)
                        suggestionRow.resolvedProfile = { displayName: displayName,
                                                          avatarUrl: avatarUrl,
                                                          known: true }
                }
            }
            readonly property string shownName:
                (modelData.displayName || "").length > 0 ? modelData.displayName
                : (resolvedProfile && (resolvedProfile.displayName || "").length > 0
                   ? resolvedProfile.displayName : "")
            readonly property string shownAvatar:
                (modelData.avatarUrl || "").length > 0 ? modelData.avatarUrl
                : (resolvedProfile && resolvedProfile.avatarUrl
                   ? resolvedProfile.avatarUrl : "")
            onClicked: suggestionList.selectRow(index)
            Accessible.name: suggestionRow.shownName.length > 0
                             ? qsTr("%1 (%2)").arg(suggestionRow.shownName).arg(modelData.userId)
                             : modelData.userId
            contentItem: RowLayout {
                spacing: AppTheme.spacing8
                Avatar {
                    size: 32
                    name: suggestionRow.shownName.length > 0
                          ? suggestionRow.shownName
                          : suggestionRow.modelData.userId.slice(1)
                    mxc: suggestionRow.shownAvatar
                    colorKey: suggestionRow.modelData.userId
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Label {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: suggestionRow.shownName.length > 0
                              ? suggestionRow.shownName
                              : suggestionRow.modelData.userId
                        color: suggestionRow.highlighted ? AppTheme.stormText
                                                         : AppTheme.stormTextSecondary
                        font.family: AppTheme.menuFont
                        font.pixelSize: AppTheme.textSubtitle
                        font.weight: AppTheme.weightStrong
                        elide: Label.ElideRight
                    }
                    Label {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: suggestionRow.modelData.userId
                        color: AppTheme.stormTextMuted
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.fontMonoXS
                        elide: Label.ElideMiddle
                    }
                }
            }
            background: Rectangle {
                color: (suggestionRow.highlighted || suggestionRow.hovered)
                       ? AppTheme.stormSelection : "transparent"
                radius: AppTheme.radiusSm
            }
        }
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
