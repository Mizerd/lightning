import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings -> Notifications, the account half: the push rules your homeserver
// applies to every device (master mute, default alert levels, mention levels,
// keywords, a few event rules) and a troubleshooter for "I got no
// notification". Bound to app.pushRules and app.notificationTroubleshooter.
//
// Visibility is toggled by the host, never a Loader, so a half-typed keyword
// survives switching categories. A level is -1 (the homeserver has no such
// rule: the row is hidden), 0 Off, 1 Notify, 2 Loud.
ColumnLayout {
    id: root
    objectName: "notificationRulesSection"

    Layout.fillWidth: true
    spacing: AppTheme.spacing12

    readonly property var rules: (typeof app !== "undefined" && app)
                                 ? app.pushRules : null
    readonly property var shoot: (typeof app !== "undefined" && app)
                                 ? app.notificationTroubleshooter : null
    readonly property bool supported: root.rules ? root.rules.supported === true
                                                 : false

    onVisibleChanged: if (visible && root.rules) root.rules.refresh()
    Component.onCompleted: if (root.rules) root.rules.refresh()

    component RulesCard: Pane {
        Layout.fillWidth: true
        background: Rectangle {
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            radius: AppTheme.radiusMd
        }
    }

    component Hint: Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        lineHeight: AppTheme.lineHeightBody
        lineHeightMode: Text.ProportionalHeight
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
    }

    component SubTitle: Label {
        color: AppTheme.stormTextSecondary
        font.pixelSize: AppTheme.textBody
        font.weight: AppTheme.weightStrong
    }

    // One row: a label and a three-way (or two-way) level picker.
    component LevelRow: RowLayout {
        id: levelRow
        property string title
        property var options: [qsTr("Off"), qsTr("Notify"), qsTr("Loud")]
        property int level: 0
        property string pickerName
        signal picked(int index)
        Layout.fillWidth: true
        spacing: AppTheme.spacing8
        Label {
            Layout.fillWidth: true
            text: levelRow.title
            color: AppTheme.stormText
            font.pixelSize: AppTheme.textBody
            elide: Label.ElideRight
        }
        AppComboBox {
            storm: true
            objectName: levelRow.pickerName
            Layout.preferredWidth: 180
            enabled: !root.rules.masterMute
            model: levelRow.options
            Accessible.name: levelRow.title
            currentIndex: Math.min(Math.max(levelRow.level, 0),
                                   levelRow.options.length - 1)
            onActivated: (index) => levelRow.picked(index)
        }
    }

    Label {
        text: qsTr("Account notification rules")
        color: AppTheme.stormText
        font.pixelSize: AppTheme.textTitle
        font.weight: AppTheme.weightStrong
        Layout.topMargin: AppTheme.spacing8
        visible: root.supported
    }

    RulesCard {
        visible: root.supported
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing12

            Hint {
                text: qsTr("These are saved to your Matrix account, so they "
                           + "apply on every device and in every client you "
                           + "use. A room you have set up yourself keeps its "
                           + "own level whatever is chosen here.")
            }
            Label {
                objectName: "pushRulesError"
                visible: root.rules.errorText.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: AppTheme.stormDanger
                font.pixelSize: AppTheme.textMeta
                text: root.rules.errorText
            }

            // ---- Master mute ----
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing12
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        text: qsTr("Mute all notifications")
                        color: AppTheme.stormText
                        font.pixelSize: AppTheme.textBody
                        font.weight: AppTheme.weightMedium
                    }
                    Hint {
                        text: qsTr("Silences every room on every device until "
                                   + "you turn it off.")
                    }
                }
                AppSwitch {
                    objectName: "pushMasterMuteSwitch"
                    checked: root.rules.masterMute
                    Accessible.name: qsTr("Mute all notifications")
                    onToggled: root.rules.setMasterMute(!root.rules.masterMute)
                }
            }
        }
    }

    // ---- Default alert level ----
    Label {
        text: qsTr("Default alerts")
        color: AppTheme.stormText
        font.pixelSize: AppTheme.textTitle
        font.weight: AppTheme.weightStrong
        Layout.topMargin: AppTheme.spacing8
        visible: root.supported
    }
    RulesCard {
        visible: root.supported
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8
            LevelRow {
                title: qsTr("Direct messages")
                pickerName: "pushDmDefaultCombo"
                options: [qsTr("All messages"), qsTr("Mentions and keywords")]
                level: root.rules.dmAll ? 0 : 1
                onPicked: (i) => root.rules.setDefaultAll(true, i === 0)
            }
            LevelRow {
                title: qsTr("Rooms")
                pickerName: "pushRoomsDefaultCombo"
                options: [qsTr("All messages"), qsTr("Mentions and keywords")]
                level: root.rules.roomsAll ? 0 : 1
                onPicked: (i) => root.rules.setDefaultAll(false, i === 0)
            }
            Hint {
                objectName: "pushDriftNote"
                visible: root.rules.dmDrift || root.rules.roomsDrift
                color: AppTheme.warning
                text: qsTr("Encrypted and unencrypted rooms were set to "
                           + "different levels, probably by another client. "
                           + "Choosing a level here sets both.")
            }
            Hint {
                text: qsTr("This is the level for rooms that have no setting "
                           + "of their own. Set a single room from its Room "
                           + "information.")
            }
        }
    }

    // ---- Mentions ----
    Label {
        text: qsTr("Mentions")
        color: AppTheme.stormText
        font.pixelSize: AppTheme.textTitle
        font.weight: AppTheme.weightStrong
        Layout.topMargin: AppTheme.spacing8
        visible: root.supported
    }
    RulesCard {
        visible: root.supported
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8
            LevelRow {
                visible: root.rules.mentionUserId >= 0
                title: qsTr("Your user ID")
                pickerName: "pushMentionUserIdCombo"
                level: root.rules.mentionUserId
                onPicked: (i) => root.rules.setMention("userId", i)
            }
            LevelRow {
                visible: root.rules.mentionDisplayName >= 0
                title: qsTr("Your display name")
                pickerName: "pushMentionDisplayNameCombo"
                level: root.rules.mentionDisplayName
                onPicked: (i) => root.rules.setMention("displayName", i)
            }
            LevelRow {
                visible: root.rules.mentionUsername >= 0
                title: qsTr("Your username")
                pickerName: "pushMentionUsernameCombo"
                level: root.rules.mentionUsername
                onPicked: (i) => root.rules.setMention("username", i)
            }
            LevelRow {
                visible: root.rules.mentionRoom >= 0
                title: qsTr("@room")
                pickerName: "pushMentionRoomCombo"
                level: root.rules.mentionRoom
                onPicked: (i) => root.rules.setMention("room", i)
            }
            Hint {
                text: qsTr("Notify alerts you. Loud alerts you with a sound. "
                           + "Off ignores that kind of mention. These apply "
                           + "in every room that is not muted.")
            }
        }
    }

    // ---- Keywords ----
    Label {
        text: qsTr("Keywords")
        color: AppTheme.stormText
        font.pixelSize: AppTheme.textTitle
        font.weight: AppTheme.weightStrong
        Layout.topMargin: AppTheme.spacing8
        visible: root.supported
    }
    RulesCard {
        visible: root.supported
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                AppTextField {
                    id: keywordField
                    objectName: "pushKeywordField"
                    storm: true
                    Layout.fillWidth: true
                    placeholderText: qsTr("Add a word or phrase")
                    maximumLength: 120
                    Accessible.name: qsTr("New keyword")
                    onAccepted: addButton.add()
                }
                AppComboBox {
                    id: keywordLevel
                    storm: true
                    objectName: "pushKeywordLevelCombo"
                    Layout.preferredWidth: 120
                    model: [qsTr("Off"), qsTr("Notify"), qsTr("Loud")]
                    currentIndex: 1
                    Accessible.name: qsTr("Level for the new keyword")
                }
                AppButton {
                    id: addButton
                    objectName: "pushKeywordAddButton"
                    storm: true
                    text: qsTr("Add")
                    enabled: keywordField.text.trim().length > 0
                    function add() {
                        if (!enabled)
                            return
                        if (root.rules.addKeyword(keywordField.text,
                                                  keywordLevel.currentIndex))
                            keywordField.clear()
                    }
                    onClicked: add()
                }
            }
            Hint {
                visible: root.rules.keywords.length === 0
                text: qsTr("No keywords. Add a word and you are notified "
                           + "whenever someone says it.")
            }
            Repeater {
                model: root.rules.keywords
                delegate: LevelRow {
                    required property var modelData
                    title: modelData.keyword
                    pickerName: "pushKeywordRow"
                    level: modelData.level
                    onPicked: (i) =>
                        root.rules.setKeywordLevel(modelData.keyword, i)
                    AppButton {
                        storm: true
                        size: "sm"
                        text: qsTr("Remove")
                        Accessible.name:
                            qsTr("Remove keyword %1").arg(modelData.keyword)
                        onClicked: root.rules.removeKeyword(modelData.keyword)
                    }
                }
            }
            Hint {
                text: qsTr("Your homeserver cannot read encrypted messages, so "
                           + "in encrypted rooms a keyword matches only on "
                           + "devices that can decrypt them.")
            }
        }
    }

    // ---- Other events ----
    Label {
        text: qsTr("Other events")
        color: AppTheme.stormText
        font.pixelSize: AppTheme.textTitle
        font.weight: AppTheme.weightStrong
        Layout.topMargin: AppTheme.spacing8
        visible: root.supported
              && (root.rules.eventReactions >= 0
                  || root.rules.eventInvites >= 0
                  || root.rules.eventNotices >= 0)
    }
    RulesCard {
        visible: root.supported
              && (root.rules.eventReactions >= 0
                  || root.rules.eventInvites >= 0
                  || root.rules.eventNotices >= 0)
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8
            LevelRow {
                visible: root.rules.eventReactions >= 0
                title: qsTr("Reactions to your messages")
                pickerName: "pushEventReactionsCombo"
                options: [qsTr("Off"), qsTr("Notify")]
                level: root.rules.eventReactions
                onPicked: (i) => root.rules.setEventRule("reactions", i)
            }
            LevelRow {
                visible: root.rules.eventInvites >= 0
                title: qsTr("Invitations to you")
                pickerName: "pushEventInvitesCombo"
                options: [qsTr("Off"), qsTr("Notify")]
                level: root.rules.eventInvites
                onPicked: (i) => root.rules.setEventRule("invites", i)
            }
            LevelRow {
                visible: root.rules.eventNotices >= 0
                title: qsTr("Messages from bots")
                pickerName: "pushEventNoticesCombo"
                options: [qsTr("Off"), qsTr("Notify")]
                level: root.rules.eventNotices
                onPicked: (i) => root.rules.setEventRule("notices", i)
            }
            Hint {
                text: qsTr("Bot messages are the automated notices some "
                           + "bridges and bots post. Off keeps them quiet; "
                           + "Notify treats them like any other message in "
                           + "that room.")
            }
        }
    }

    // ---- Troubleshooter ----
    Label {
        text: qsTr("Not getting notifications?")
        color: AppTheme.stormText
        font.pixelSize: AppTheme.textTitle
        font.weight: AppTheme.weightStrong
        Layout.topMargin: AppTheme.spacing8
        visible: root.shoot !== null
    }
    RulesCard {
        visible: root.shoot !== null
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8

            Hint {
                text: qsTr("Checks the things Lightning can see: whether "
                           + "something can show a notification, the "
                           + "switches here, your account, and the room you "
                           + "have open. The test notification covers the "
                           + "rest.")
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                AppButton {
                    objectName: "troubleshooterRunButton"
                    storm: true
                    text: qsTr("Run checks")
                    onClicked: root.shoot.run(app.currentRoomId)
                }
                AppButton {
                    objectName: "troubleshooterTestButton"
                    storm: true
                    text: qsTr("Send a test notification")
                    onClicked: root.shoot.sendTest()
                }
            }
            Repeater {
                model: root.shoot ? root.shoot.checks : []
                delegate: ColumnLayout {
                    id: checkRow
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 2
                    readonly property int status: modelData.status
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        Label {
                            // Words, not colour alone.
                            text: checkRow.status === 0 ? qsTr("OK")
                                : checkRow.status === 1 ? qsTr("Check")
                                : checkRow.status === 2 ? qsTr("Problem")
                                : qsTr("Unknown")
                            color: checkRow.status === 0 ? AppTheme.stormSuccess
                                 : checkRow.status === 2 ? AppTheme.stormDanger
                                 : checkRow.status === 1 ? AppTheme.warning
                                 : AppTheme.stormTextSecondary
                            font.pixelSize: AppTheme.textMeta
                            font.weight: AppTheme.weightStrong
                            Layout.preferredWidth: 64
                        }
                        Label {
                            Layout.fillWidth: true
                            text: checkRow.modelData.title
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.textBody
                            wrapMode: Text.WordWrap
                        }
                        AppButton {
                            visible: checkRow.modelData.fix === "enableApp"
                                  || checkRow.modelData.fix === "unmute"
                            storm: true
                            size: "sm"
                            text: checkRow.modelData.fix === "enableApp"
                                  ? qsTr("Turn on") : qsTr("Unmute")
                            onClicked: {
                                if (checkRow.modelData.fix === "enableApp")
                                    app.settings.notificationsEnabled = true
                                else
                                    root.rules.setMasterMute(false)
                                root.shoot.run(app.currentRoomId)
                            }
                        }
                    }
                    Hint {
                        Layout.leftMargin: 64 + AppTheme.spacing8
                        text: checkRow.modelData.detail
                    }
                }
            }
        }
    }
}
