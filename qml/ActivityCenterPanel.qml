import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.9 (phase 2): the global Activity Center. One list, across every room,
// of what was addressed to the user — mentions, replies, replies in the
// user's threads, reactions to the user's messages, invites and keyword
// hits — with its OWN seen state (independent of read receipts) and a
// click that lands on the exact event. Shared by both room-list layouts:
// the button that opens it sits in the RoomsPanel header they both use.
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

    function openPanel() {
        open()
        keywordsField.text = root.activity ? root.activity.keywords.join(", ") : ""
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
        case "reaction": return qsTr("Reacted %1 to your message").arg(reactionKey)
        case "invite": return qsTr("Invited you")
        case "keyword": return qsTr("Keyword")
        // The server told us this was highlighted for this account and not
        // which rule matched, so this says exactly that and no more.
        case "highlight": return qsTr("Highlighted for you")
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
                       : Qt.formatDateTime(d, "ddd d MMM")
    }
    function applyKeywords() {
        if (!root.activity)
            return
        var parts = keywordsField.text.split(",")
        var out = []
        for (var i = 0; i < parts.length; ++i) {
            var t = parts[i].trim()
            if (t.length > 0)
                out.push(t)
        }
        root.activity.keywords = out
        keywordsField.text = root.activity.keywords.join(", ")
    }

    background: Rectangle {
        radius: AppTheme.radiusLg
        color: AppTheme.stormPanel
        border.color: AppTheme.stormBorder
        border.width: 1
    }

    contentItem: ColumnLayout {
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

        // Keyword highlights: whole words, case-insensitive, this account.
        RowLayout {
            Layout.fillWidth: true
            visible: root.activity && root.activity.filter === "keywords"
            spacing: AppTheme.spacing8
            AppTextField {
                id: keywordsField
                objectName: "activityKeywordsField"
                storm: true
                Layout.fillWidth: true
                placeholderText: qsTr("Keywords, separated by commas")
                onEditingFinished: root.applyKeywords()
            }
            AppButton {
                storm: true
                kind: "secondary"
                size: "sm"
                text: qsTr("Save")
                onClicked: root.applyKeywords()
            }
        }

        Label {
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
                switch (root.activity.filter) {
                case "keywords":
                    return root.activity.keywords.length === 0
                           ? qsTr("Add keywords above to be told when someone uses them.")
                           : qsTr("No message has used your keywords yet.")
                case "invites": return qsTr("No pending invites.")
                case "reactions": return qsTr("Nobody has reacted to your messages yet.")
                default: return qsTr("Nothing here yet. Mentions, replies and reactions to you will show up as they arrive.")
                }
            }
        }

        ListView {
            id: listView
            objectName: "activityList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: AppTheme.spacing4
            model: root.activity
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
                Accessible.name: qsTr("%1 in %2, %3: %4")
                                   .arg(root.kindLabel(row.kind, row.reactionKey))
                                   .arg(row.roomName).arg(row.senderName)
                                   .arg(row.encrypted ? qsTr("encrypted message") : row.preview)
                HoverHandler { id: rowHover }
                TapHandler {
                    onTapped: {
                        root.activity.open(row.entryId)
                        root.close()
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
                                // A room name is attacker-chosen text, and an
                                // UNSOLICITED INVITE puts a stranger's into
                                // this list with no acceptance. Label defaults
                                // to Text.AutoText, so a name containing a
                                // known tag would be rendered as rich text and
                                // `<img src=...>` would beacon on open. The
                                // body Label below always set this; these two
                                // were the omission.
                                textFormat: Text.PlainText
                                color: AppTheme.stormText
                                font.pixelSize: AppTheme.textBody
                                font.weight: row.seen ? AppTheme.weightBody
                                                      : AppTheme.weightStrong
                                elide: Text.ElideRight
                                Layout.maximumWidth: 240
                            }
                            Label {
                                // Carries the reaction key, which is an
                                // arbitrary sender-chosen string.
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
                                font.family: AppTheme.monoFont
                                font.pixelSize: AppTheme.textMicro
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: row.encrypted
                                  ? qsTr("Encrypted message")
                                  : (row.kind === "invite"
                                     ? qsTr("From %1").arg(row.senderName)
                                     : (row.senderName + ": " + row.preview))
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
    }
}
