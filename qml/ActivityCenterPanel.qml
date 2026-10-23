import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The global Activity Center: one list, across every room, of what was
// addressed to the user (mentions, replies, replies in their threads,
// reactions to their messages, invites and keyword hits), with its own seen
// state independent of read receipts, and a click that lands on the exact
// event. Opened from the RoomsPanel header shared by both room-list layouts.
Dialog {
    id: root
    objectName: "activityCenterDialog"
    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    width: Math.min(620, parent ? parent.width - AppTheme.spacing24 * 2 : 620)
    height: Math.min(640, parent ? parent.height - AppTheme.spacing24 * 2 : 640)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    readonly property var activity: app.activity
    // Opening a modal gives the popup focus, but not its keyboard-driven
    // list. Do this on every open, including after tabbing to a control;
    // leave the current row and the model's seen state alone.
    onOpened: listView.forceActiveFocus(Qt.PopupFocusReason)

    // The keyword list is NOT edited here: it used to be a second, local list
    // that only filtered this panel and never notified or pushed. The account's
    // keywords are push rules, edited in Settings > Notifications. Nothing in
    // this panel writes the model's keywords or seen marker except the
    // explicit "Mark all seen".
    function openPanel() {
        open()
    }

    function dialogAction(event) {
        return root.opened && app.shortcuts
            ? app.shortcuts.dialogActionForKey(event.key, event.modifiers, false) : ""
    }
    function handleDialogKey(event) {
        const action = root.dialogAction(event)
        if (action === "dialog.close") {
            root.close()
        } else if (action === "dialog.previousItem" || action === "dialog.nextItem") {
            if (listView.count > 0) {
                const delta = action === "dialog.previousItem" ? -1 : 1
                listView.currentIndex = Math.max(0, Math.min(listView.count - 1,
                    listView.currentIndex < 0 ? 0 : listView.currentIndex + delta))
                listView.positionViewAtIndex(listView.currentIndex, ListView.Contain)
            }
            listView.forceActiveFocus(Qt.ShortcutFocusReason)
        } else {
            return false
        }
        event.accepted = true
        return true
    }

    function openEntry(entryId) {
        if (!root.activity)
            return
        root.activity.open(entryId)
        root.close()
    }

    // Single-pass %N substitution. QML's String.arg() takes ONE argument and
    // each call rewrites the lowest-numbered marker anywhere in the string,
    // including markers an earlier substitution inserted, so a reaction key or
    // a room name containing "%2" could rewrite the sentence. This replaces
    // every marker of the TEMPLATE once and never rescans the inserted text.
    function fill(template, values) {
        return template.replace(/%([1-9])/g, function(m, d) {
            var i = parseInt(d, 10) - 1
            return i < values.length ? String(values[i]) : m
        })
    }
    // Names are other people's text. Strip any bidi control that could close
    // our isolate or override the direction, then isolate (FSI ... PDI) so a
    // hostile or opposite-direction name cannot reorder its neighbours.
    function isolate(name) {
        var n = String(name === undefined || name === null ? "" : name)
        n = n.replace(/[\u202A-\u202E\u2066-\u2069]/g, "")
        return "\u2068" + n + "\u2069"
    }
    // A reaction key is remote text. MSC2545 clients put the pack image's mxc
    // URI there; show the shortcode if a pack this account holds knows it,
    // else a generic label, never the raw URI (as MessageDelegate does).
    function reactionDisplay(key) {
        var k = key || ""
        if (k.indexOf("mxc://") !== 0)
            return k
        var code = ""
        if (typeof app !== "undefined" && app.stickers) {
            var rev = app.stickers.revision
            code = app.stickers.shortcodeForUrl(k)
        }
        return code && code.length > 0 ? code : qsTr("custom emoji")
    }
    function previewText(kind, preview, reactionKey) {
        var p = preview || ""
        if (kind === "reaction")
            return reactionDisplay(reactionKey || p)
        return p.indexOf("mxc://") === 0 ? reactionDisplay(p) : p
    }
    function rowAccessibleName(kind, reactionKey, roomName, senderName,
                               senderId, preview, encrypted, seen, timestampMs) {
        var body = encrypted ? qsTr("encrypted message")
                             : previewText(kind, preview, reactionKey)
        var vals = [kindLabel(kind, reactionKey), isolate(roomName),
                    isolate(senderLabel(senderName, senderId)), body,
                    timeLabel(timestampMs)]
        return fill(seen ? qsTr("%1 in %2, %3: %4, %5")
                         : qsTr("Unread. %1 in %2, %3: %4, %5"), vals)
    }

    function kindIcon(kind) {
        switch (kind) {
        case "mention": return "alternate_email"
        case "room_mention": return "alternate_email"
        case "reply": return "reply"
        case "thread": return "forum"
        case "reaction": return "add_reaction"
        case "invite": return "group_add"
        case "keyword": return "tag"
        }
        return "notifications"
    }
    function kindLabel(kind, reactionKey) {
        switch (kind) {
        case "mention": return qsTr("Mentioned you")
        case "room_mention": return qsTr("Mentioned everyone")
        case "reply": return qsTr("Replied to you")
        case "thread": return qsTr("Replied in your thread")
        case "reaction": return fill(qsTr("Reacted %1 to your message"),
                                     [reactionDisplay(reactionKey)])
        case "invite": return qsTr("Invited you")
        case "keyword": return qsTr("Keyword")
        // The server says it's highlighted, not which rule matched; a
        // highlight is a mention of you or one of your keywords.
        case "highlight": return qsTr("Mention or keyword")
        }
        return ""
    }
    function timeLabel(ms) {
        if (!ms || ms <= 0)
            return ""
        var d = new Date(ms)
        var now = new Date()
        var sameDay = d.getFullYear() === now.getFullYear()
                      && d.getMonth() === now.getMonth()
                      && d.getDate() === now.getDate()
        return sameDay ? Qt.formatTime(d, app.settings.clockTimeFormat)
                       : Qt.locale(AppLocale.name).toString(d, Locale.ShortFormat)
    }
    // A sender the model could not name yet arrives as a bare Matrix ID; the
    // localpart reads as a name ("@bob:example.org" -> "bob"), as mention
    // pills fall back. The full ID stays in the accessible name.
    function senderLabel(senderName, senderId) {
        var n = senderName || ""
        if (n.length === 0 || n === senderId) {
            var id = senderId || ""
            if (id.charAt(0) === "@")
                id = id.substring(1)
            var colon = id.indexOf(":")
            return colon > 0 ? id.substring(0, colon) : id
        }
        return n
    }

    background: Rectangle {
        radius: AppTheme.radiusLg
        color: AppTheme.stormPanel
        border.color: AppTheme.stormBorder
        border.width: 1
    }

    contentItem: ColumnLayout {
        id: panelColumn
        Keys.onShortcutOverride: (event) => {
            if (root.dialogAction(event) !== "")
                event.accepted = true
        }
        Keys.onPressed: (event) => root.handleDialogKey(event)
        spacing: AppTheme.spacing12

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Label {
                text: qsTr("Activity")
                color: AppTheme.stormText
                font.family: AppTheme.menuFont
                font.pixelSize: AppTheme.textTitle
                font.weight: AppTheme.weightBold
            }
            StatusChip {
                storm: true
                visible: root.activity && root.activity.unseenCount > 0
                label: root.activity
                       ? qsTr("%n new", "", root.activity.unseenCount) : ""
                tone: "bolt"
            }
            Item { Layout.fillWidth: true }
            AppButton {
                objectName: "activityMarkAllSeen"
                storm: true
                kind: "ghost"
                size: "sm"
                iconName: "done_all"
                text: qsTr("Mark all seen")
                enabled: root.activity && root.activity.unseenCount > 0
                onClicked: root.activity.markAllSeen()
            }
            IconButton {
                storm: true
                iconName: "close"
                size: "sm"
                Accessible.name: qsTr("Close")
                onClicked: root.close()
            }
        }

        SegmentedControl {
            objectName: "activityFilter"
            storm: true
            dense: true
            fitWidth: true
            Layout.fillWidth: true
            model: [
                { value: "all", label: qsTr("All") },
                { value: "mentions", label: qsTr("Mentions") },
                { value: "replies", label: qsTr("Replies") },
                { value: "threads", label: qsTr("Threads") },
                { value: "reactions", label: qsTr("Reactions") },
                { value: "invites", label: qsTr("Invites") },
                { value: "keywords", label: qsTr("Keywords") }
            ]
            current: root.activity ? root.activity.filter : "all"
            onActivated: (value) => { if (root.activity) root.activity.filter = value }
        }

        // Keywords are account push rules; they are edited in Settings.
        RowLayout {
            Layout.fillWidth: true
            visible: root.activity && root.activity.filter === "keywords"
            spacing: AppTheme.spacing8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textBody
                text: qsTr("Keywords are managed in Settings, under Notifications.")
            }
            AppButton {
                objectName: "activityOpenKeywordSettings"
                storm: true
                kind: "secondary"
                size: "sm"
                text: qsTr("Open keyword settings")
                onClicked: {
                    root.close()
                    app.showSettingsSection("notifications")
                }
            }
        }

        // The server cannot list past highlights (no GET /notifications), so
        // after a restart only what arrived since is here, while the room list
        // still counts older mentions. Said once, above the rows; an empty
        // list says it in place of "Nothing here yet". Invites and keywords do
        // not come from that history.
        readonly property bool historyHintApplies: !!root.activity
            && root.activity.historyUnavailable
            && root.activity.filter !== "invites"
            && root.activity.filter !== "keywords"

        Label {
            objectName: "activityHistoryUnavailable"
            visible: panelColumn.historyHintApplies && listView.count > 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            text: qsTr("Older activity is not available from this server.")
        }

        Label {
            objectName: "activityEmptyState"
            visible: listView.count === 0
            Layout.fillWidth: true
            Layout.topMargin: AppTheme.spacing24
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textBody
            text: {
                if (!root.activity)
                    return ""
                if (panelColumn.historyHintApplies)
                    return qsTr("Older activity is not available from this server. New mentions, replies and reactions will show up here as they arrive.")
                switch (root.activity.filter) {
                case "keywords":
                    return root.activity.keywords.length === 0
                           ? qsTr("No keywords are set. Add some in Settings, under Notifications.")
                           : qsTr("No message has used your keywords yet.")
                case "invites": return qsTr("No pending invites.")
                case "reactions": return qsTr("Nobody has reacted to your messages yet.")
                default: return qsTr("Nothing here yet. Mentions, replies and reactions to you will show up as they arrive.")
                }
            }
        }

        Item {
            id: listHost
            Layout.fillWidth: true
            Layout.fillHeight: true
            ListView {
                id: listView
                objectName: "activityList"
                anchors.fill: parent
                clip: true
                spacing: AppTheme.spacing4
                model: root.activity
                activeFocusOnTab: true
                keyNavigationEnabled: true
                currentIndex: 0
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Activity")
                Keys.onReturnPressed: listView.openCurrent()
                Keys.onEnterPressed: listView.openCurrent()
                Keys.onSpacePressed: listView.openCurrent()
                // keyNavigationEnabled covers Up/Down only. Home/End/Page keys
                // move the current row, and the view follows it, so a list that
                // is Tab-focused while scrolled away shows its ring (C08-F03).
                Keys.onPressed: function(event) {
                    if (listView.count === 0)
                        return
                    var page = Math.max(1, Math.floor(listView.height / 64))
                    var target = -1
                    switch (event.key) {
                    case Qt.Key_Home: target = 0; break
                    case Qt.Key_End: target = listView.count - 1; break
                    case Qt.Key_PageUp:
                        target = Math.max(0, listView.currentIndex - page); break
                    case Qt.Key_PageDown:
                        target = Math.min(listView.count - 1,
                                          listView.currentIndex + page); break
                    default: return
                    }
                    listView.currentIndex = target
                    listView.positionViewAtIndex(target, ListView.Contain)
                    event.accepted = true
                }
                onActiveFocusChanged: if (activeFocus && count > 0)
                    positionViewAtIndex(Math.max(0, currentIndex), ListView.Contain)
                function openCurrent() {
                    if (listView.currentIndex >= 0 && listView.currentIndex < listView.count
                            && listView.currentItem)
                        root.openEntry(listView.currentItem.entryId)
                }
                ScrollBar.vertical: ScrollBar { }
                delegate: Rectangle {
                    id: row
                    required property int index
                    required property string entryId
                    required property string kind
                    required property string roomId
                    required property string roomName
                    required property string senderId
                    required property string senderName
                    required property string preview
                    required property double timestampMs
                    required property string eventId
                    required property string threadRootId
                    required property bool seen
                    required property bool encrypted
                    required property string reactionKey
                    width: ListView.view.width
                    implicitHeight: rowContent.implicitHeight + AppTheme.spacing12 * 2
                    radius: AppTheme.radiusTile
                    color: rowHover.hovered ? AppTheme.stormSelection : AppTheme.stormInset
                    border.width: 1
                    border.color: row.seen ? AppTheme.stormBorder : AppTheme.stormBorderStrong
                    Accessible.role: Accessible.Button
                    objectName: "activityRow"
                    readonly property string a11yName: root.rowAccessibleName(
                        row.kind, row.reactionKey, row.roomName, row.senderName,
                        row.senderId, row.preview, row.encrypted, row.seen,
                        row.timestampMs)
                    Accessible.name: row.a11yName
                    Accessible.onPressAction: root.openEntry(row.entryId)
                    // Visible keyboard focus: the current row while the list has
                    // focus. Drawn inside the row so it is never clipped.
                    Rectangle {
                        objectName: "activityRowFocusRing"
                        anchors.fill: parent
                        radius: parent.radius
                        color: "transparent"
                        border.width: 2
                        border.color: AppTheme.stormBorderStrong
                        visible: row.ListView.isCurrentItem && listView.activeFocus
                    }
                    HoverHandler { id: rowHover }
                    TapHandler {
                        gesturePolicy: TapHandler.WithinBounds
                        onTapped: {
                            listView.currentIndex = row.index
                            root.openEntry(row.entryId)
                        }
                    }
                    RowLayout {
                        id: rowContent
                        anchors.fill: parent
                        anchors.margins: AppTheme.spacing12
                        spacing: AppTheme.spacing10
                        Rectangle {
                            Layout.alignment: Qt.AlignTop
                            Layout.topMargin: 6
                            width: 8; height: 8; radius: 4
                            color: AppTheme.bolt
                            visible: !row.seen
                        }
                        Icon {
                            Layout.alignment: Qt.AlignTop
                            name: root.kindIcon(row.kind)
                            size: 18
                            color: row.seen ? AppTheme.stormTextMuted : AppTheme.stormText
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing2
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: AppTheme.spacing6
                                Label {
                                    text: row.roomName
                                    // Room names are attacker-chosen, and an
                                    // unsolicited invite puts a stranger's here;
                                    // with AutoText, markup such as `<img src=...>`
                                    // would render and beacon.
                                    textFormat: Text.PlainText
                                    color: AppTheme.stormText
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: row.seen ? AppTheme.weightBody
                                                          : AppTheme.weightStrong
                                    elide: Text.ElideRight
                                    Layout.maximumWidth: 240
                                }
                                Label {
                                    // Includes the reaction key, an arbitrary
                                    // sender-chosen string.
                                    text: root.kindLabel(row.kind, row.reactionKey)
                                    textFormat: Text.PlainText
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.textMeta
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Label {
                                    text: root.timeLabel(row.timestampMs)
                                    color: AppTheme.stormTextMuted
                                    // Proportional, like every other list's time.
                                    font.pixelSize: AppTheme.textMeta
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                text: row.encrypted
                                      ? qsTr("Encrypted message")
                                      : (row.kind === "invite"
                                         ? root.fill(qsTr("From %1"), [root.isolate(
                                               root.senderLabel(row.senderName,
                                                                row.senderId))])
                                         : (root.isolate(root.senderLabel(
                                                row.senderName, row.senderId))
                                            + ": " + root.previewText(
                                                row.kind, row.preview,
                                                row.reactionKey)))
                                color: row.encrypted ? AppTheme.stormTextMuted
                                                     : AppTheme.stormTextSecondary
                                font.italic: row.encrypted
                                font.pixelSize: AppTheme.textBody
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                            }
                        }
                    }
                }
            }
            MiddleClickScroller {
                objectName: "activityMiddleClickScroller"
                anchors.fill: parent
                z: 1
                view: listView
            }
        }
    }
}
