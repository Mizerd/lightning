import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// "From your other accounts" in the Activity panel: what the other live
// accounts received, each row labelled with its account. A click switches to
// that account and opens the room.
ColumnLayout {
    id: section
    objectName: "unifiedInboxSection"
    spacing: AppTheme.spacing6

    readonly property var inbox: typeof app !== "undefined" ? app.unifiedInbox : null
    readonly property int savedAccounts: typeof app !== "undefined" && app.accounts
                                         ? app.accounts.knownUserIds.length : 0
    readonly property bool hintShown: typeof app !== "undefined"
        && app.liveAccountsLabsAvailable === true && savedAccounts > 1
        && (!inbox || inbox.count === 0)
        && (app.liveAccountsActive !== true || app.liveAccountCount < 2)
    property int maxVisibleRows: 3

    signal entryOpened()
    signal settingsOpened()

    function timeLabel(ms) {
        if (!ms || ms <= 0)
            return ""
        var d = new Date(ms)
        var now = new Date()
        return d.toDateString() === now.toDateString()
               ? Qt.formatTime(d, app.settings.clockTimeFormat)
               : Qt.locale(AppLocale.name).toString(d, Locale.ShortFormat)
    }
    // Single pass, so a name containing "%2" cannot rewrite the sentence.
    function fill(template, values) {
        return template.replace(/%([1-9])/g, function(m, d) {
            var i = parseInt(d, 10) - 1
            return i < values.length ? String(values[i]) : m
        })
    }
    function isolate(name) {
        var n = String(name === undefined || name === null ? "" : name)
        return "⁨" + n.replace(/[‪-‮⁦-⁩]/g, "") + "⁩"
    }
    function senderLabel(name, id) {
        if (name && name.length > 0 && name !== id)
            return name
        var s = (id || "").replace(/^@/, "")
        var colon = s.indexOf(":")
        return colon > 0 ? s.substring(0, colon) : s
    }
    function open(accountUserId, roomId, eventId, threadRootId) {
        if (app.openUnifiedInboxEntry(accountUserId, roomId, eventId, threadRootId))
            section.entryOpened()
    }

    readonly property bool hasContent: (!!inbox && inbox.count > 0) || hintShown

    RowLayout {
        Layout.fillWidth: true
        visible: section.hintShown
        spacing: AppTheme.spacing8
        Label {
            objectName: "unifiedInboxHint"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            text: qsTr("Activity from your other accounts shows up here while they stay signed in. Choose which ones in Settings, under Labs.")
        }
        AppButton {
            objectName: "unifiedInboxOpenLabs"
            storm: true
            kind: "secondary"
            size: "sm"
            text: qsTr("Open Labs")
            onClicked: {
                section.settingsOpened()
                app.showSettingsSection("labs")
            }
        }
    }

    Label {
        objectName: "unifiedInboxHeader"
        visible: !!section.inbox && section.inbox.count > 0
        text: qsTr("From your other accounts")
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        font.weight: AppTheme.weightStrong
    }

    ListView {
        id: inboxList
        objectName: "unifiedInboxList"
        visible: !!section.inbox && section.inbox.count > 0
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, section.maxVisibleRows * 58)
        clip: true
        spacing: AppTheme.spacing4
        model: section.inbox
        boundsBehavior: Flickable.StopAtBounds
        Accessible.role: Accessible.List
        Accessible.name: qsTr("From your other accounts")
        delegate: Rectangle {
            id: row
            required property string accountUserId
            required property string accountLabel
            required property string roomId
            required property string roomName
            required property string senderId
            required property string senderName
            required property string preview
            required property bool undecryptable
            required property double timestampMs
            required property string eventId
            required property string threadRootId
            required property bool highlight
            objectName: "unifiedInboxRow"
            width: ListView.view.width
            implicitHeight: rowContent.implicitHeight + AppTheme.spacing8 * 2
            radius: AppTheme.radiusTile
            color: rowHover.hovered ? AppTheme.stormSelection : AppTheme.stormInset
            border.width: 1
            border.color: AppTheme.stormBorderStrong
            readonly property string bodyText: row.undecryptable
                ? qsTr("Encrypted message")
                : section.isolate(section.senderLabel(row.senderName, row.senderId))
                  + ": " + row.preview
            Accessible.role: Accessible.Button
            Accessible.name: section.fill(qsTr("%1 account, %2: %3"),
                [section.isolate(row.accountLabel), section.isolate(row.roomName), row.bodyText])
            Accessible.onPressAction: section.open(row.accountUserId, row.roomId,
                                                   row.eventId, row.threadRootId)
            HoverHandler { id: rowHover }
            TapHandler {
                gesturePolicy: TapHandler.WithinBounds
                onTapped: section.open(row.accountUserId, row.roomId,
                                       row.eventId, row.threadRootId)
            }
            ColumnLayout {
                id: rowContent
                anchors.fill: parent
                anchors.margins: AppTheme.spacing8
                spacing: AppTheme.spacing2
                RowLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing6
                    Rectangle {
                        objectName: "unifiedInboxAccountChip"
                        radius: height / 2
                        color: AppTheme.stormSelection
                        border.width: 1
                        border.color: AppTheme.stormBorder
                        implicitWidth: Math.min(160, chipLabel.implicitWidth + AppTheme.spacing6 * 2)
                        implicitHeight: chipLabel.implicitHeight + 2
                        Label {
                            id: chipLabel
                            objectName: "unifiedInboxAccountLabel"
                            anchors.fill: parent
                            anchors.leftMargin: AppTheme.spacing6
                            anchors.rightMargin: AppTheme.spacing6
                            text: row.accountLabel
                            textFormat: Text.PlainText
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.textMeta
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    Icon {
                        visible: row.highlight
                        name: "alternate_email"
                        size: 14
                        color: AppTheme.stormText
                    }
                    Label {
                        Layout.fillWidth: true
                        text: row.roomName
                        // Attacker-chosen: never rendered as markup.
                        textFormat: Text.PlainText
                        color: AppTheme.stormText
                        font.pixelSize: AppTheme.textBody
                        font.weight: AppTheme.weightStrong
                        elide: Text.ElideRight
                    }
                    Label {
                        text: section.timeLabel(row.timestampMs)
                        color: AppTheme.stormTextMuted
                        font.pixelSize: AppTheme.textMeta
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: row.bodyText
                    textFormat: Text.PlainText
                    color: row.undecryptable ? AppTheme.stormTextMuted
                                             : AppTheme.stormTextSecondary
                    font.italic: row.undecryptable
                    font.pixelSize: AppTheme.textBody
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }
            }
        }
    }
}
