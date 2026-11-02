import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

ColumnLayout {
    id: root
    objectName: "labsConcurrentAccountsCard"

    Layout.fillWidth: true
    spacing: AppTheme.spacing8

    readonly property bool available: typeof app !== "undefined" && !!app
                                      && app.liveAccountsLabsAvailable === true
    readonly property bool enabledSetting: root.available && !!app.settings
                                           && app.settings.labsConcurrentAccounts === true
    readonly property bool active: root.available && app.liveAccountsActive === true

    visible: root.available

    property var rows: null
    property string refusalUserId: ""
    property string refusalText: ""

    ListModel { id: rowModel }
    property var order: []
    property int refusalPinned: -1

    function syncRowModel(snapshot) {
        const list = snapshot || []
        const byId = {}
        const ids = []
        let pinned = 0
        for (let i = 0; i < list.length; ++i) {
            const id = list[i].userId || ""
            byId[id] = list[i]
            ids.push(id)
            if (list[i].keepRunning === true)
                ++pinned
        }
        const next = []
        for (let i = 0; i < root.order.length; ++i) {
            if (byId[root.order[i]] !== undefined)
                next.push(root.order[i])
        }
        for (let i = 0; i < ids.length; ++i) {
            if (next.indexOf(ids[i]) < 0)
                next.push(ids[i])
        }
        root.order = next
        for (let i = rowModel.count - 1; i >= 0; --i) {
            if (byId[rowModel.get(i).userId] === undefined)
                rowModel.remove(i)
        }
        for (let i = 0; i < next.length; ++i) {
            const r = byId[next[i]]
            const values = {
                "userId": next[i],
                "displayName": r.displayName || "",
                "keepRunning": r.keepRunning === true,
                "liveState": r.liveState !== undefined ? r.liveState : 0
            }
            if (i < rowModel.count && rowModel.get(i).userId === next[i]) {
                rowModel.set(i, values)
                continue
            }
            let from = -1
            for (let j = i + 1; j < rowModel.count; ++j) {
                if (rowModel.get(j).userId === next[i]) { from = j; break }
            }
            if (from >= 0) {
                rowModel.move(from, i, 1)
                rowModel.set(i, values)
            } else {
                rowModel.insert(i, values)
            }
        }
        if (root.refusalUserId.length > 0 && root.refusalPinned !== pinned) {
            root.refusalUserId = ""
            root.refusalText = ""
        }
        root.pinnedNow = pinned
    }
    property int pinnedNow: 0

    function refreshRows() {
        root.rows = (typeof app !== "undefined" && app && app.accountList)
            ? app.accountList.rowsSnapshot() : null
        root.syncRowModel(root.rows)
    }
    Component.onCompleted: root.refreshRows()
    Connections {
        target: typeof app !== "undefined" ? app : null
        // The signal only exists in builds that carry Labs.
        ignoreUnknownSignals: true
        function onLiveAccountsActiveChanged() {
            root.refusalUserId = ""
            root.refusalText = ""
            root.refreshRows()
        }
    }
    Connections {
        target: (typeof app !== "undefined" && app && app.accountList)
                ? app.accountList : null
        function onRowsChanged() { root.refreshRows() }
    }

    function setKeepRunning(userId, on) {
        // The refusal is about the count the pin was asked at. Recorded
        // BEFORE the call, because setAccountKeepRunning may emit rowsChanged
        // synchronously and syncRowModel then compares against this value.
        const pinnedAtAsk = root.pinnedNow
        root.refusalPinned = pinnedAtAsk
        const refusal = app.setAccountKeepRunning(userId, on) || ""
        root.refusalUserId = refusal.length > 0 ? userId : ""
        root.refusalText = refusal
        root.refusalPinned = pinnedAtAsk
    }

    component SettingsCard: Pane {
        Layout.fillWidth: true
        background: Rectangle {
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            radius: AppTheme.radiusMd
        }
    }
    component SettingsGroupLabel: Label {
        Layout.topMargin: AppTheme.spacing8
        color: AppTheme.stormTextMuted
        font.family: AppTheme.menuSectionFont
        font.pixelSize: AppTheme.menuSectionSize
        font.weight: AppTheme.menuSectionWeight
        font.letterSpacing: AppTheme.menuSectionTracking
    }
    component NoteLabel: Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        lineHeight: AppTheme.lineHeightBody
        lineHeightMode: Text.ProportionalHeight
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
    }

    SettingsGroupLabel {
        objectName: "labsConcurrentAccountsHeading"
        text: qsTr("Accounts")
    }

    SettingsCard {
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing12
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Run several accounts at once")
                    color: AppTheme.stormText
                    font.weight: AppTheme.weightStrong
                    wrapMode: Text.WordWrap
                }
                AppSwitch {
                    objectName: "labsConcurrentAccountsSwitch"
                    checked: root.enabledSetting
                    enabled: root.available && !!app.settings
                    Accessible.name: qsTr("Run several accounts at once")
                    onToggled: app.settings.labsConcurrentAccounts =
                        !(app.settings.labsConcurrentAccounts === true)
                }
            }
            NoteLabel {
                objectName: "labsConcurrentAccountsHelp"
                text: qsTr("Keep chosen accounts signed in while you use "
                           + "another one. They keep receiving messages and "
                           + "show their notifications. Each one uses its own "
                           + "memory, network and battery. Lightning sends no "
                           + "read receipts or typing from them, and does not "
                           + "show them as online.")
            }
            NoteLabel {
                objectName: "labsConcurrentAccountsCap"
                text: qsTr("Up to %1 accounts can keep running.")
                      .arg(root.available ? app.maxPinnedAccounts : 0)
            }
            NoteLabel {
                objectName: "labsAlwaysSignedInHint"
                visible: !root.enabledSetting
                text: qsTr("Turn this on to choose which accounts stay "
                           + "signed in.")
            }
            NoteLabel {
                objectName: "labsConcurrentAccountsOffNote"
                visible: root.enabledSetting
                text: qsTr("Turning this off stops the background accounts "
                           + "now. It fully reverts at the next start.")
            }
            NoteLabel {
                objectName: "labsConcurrentAccountsUnavailable"
                visible: root.enabledSetting && !root.active
                text: qsTr("Background accounts are not available in this "
                           + "session.")
            }
            Label {
                objectName: "labsConcurrentAccountsStatus"
                visible: root.active
                Layout.fillWidth: true
                text: qsTr("%1 of %2 running")
                      .arg(root.active ? app.liveAccountCount : 0)
                      .arg(root.available ? app.maxLiveAccounts : 0)
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textMeta
                font.weight: AppTheme.weightStrong
            }

            Label {
                objectName: "labsAlwaysSignedInHeading"
                visible: root.active
                Layout.fillWidth: true
                Layout.topMargin: AppTheme.spacing8
                text: qsTr("Always signed in")
                color: AppTheme.stormText
                font.weight: AppTheme.weightStrong
                Accessible.role: Accessible.Heading
            }
            NoteLabel {
                objectName: "labsAlwaysSignedInIntro"
                visible: root.active
                text: qsTr("Choose the accounts that stay signed in while "
                           + "another one is open. The others sign in only "
                           + "when you switch to them.")
            }
            Repeater {
                objectName: "labsConcurrentAccountsRows"
                model: root.active ? rowModel : null
                delegate: ColumnLayout {
                    id: accountRow
                    required property int index
                    required property string userId
                    required property string displayName
                    required property bool keepRunning
                    required property int liveState
                    objectName: "labsAccountRow_" + userId
                    Layout.fillWidth: true
                    spacing: 2

                    readonly property bool unhealthy:
                        liveState === AccountActivityModel.NeedsSignIn
                        || liveState === AccountActivityModel.KeyringLocked
                        || liveState === AccountActivityModel.Stopped
                        || liveState === AccountActivityModel.StoreBusy
                        || liveState === AccountActivityModel.EncryptionDataMissing
                        || liveState === AccountActivityModel.OpenToFinishSetup
                    readonly property bool canSignInAgain:
                        liveState === AccountActivityModel.NeedsSignIn
                        || liveState === AccountActivityModel.OpenToFinishSetup

                    AccountActivityBadge {
                        id: stateWords
                        visible: false
                        liveState: accountRow.liveState
                    }

                    Rectangle {
                        objectName: "labsAccountSeparator"
                        opacity: accountRow.index > 0 ? 1 : 0
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: AppTheme.stormBorder
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.minimumHeight: AppTheme.buttonHeightSm
                        spacing: AppTheme.spacing12
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label {
                                id: nameLabel
                                objectName: "labsAccountName"
                                Layout.fillWidth: true
                                textFormat: Text.PlainText
                                text: accountRow.displayName.length > 0
                                      ? accountRow.displayName
                                      : accountRow.userId
                                color: AppTheme.stormText
                                font.weight: AppTheme.weightStrong
                                elide: Label.ElideRight
                                HoverHandler { id: nameHover }
                                ToolTip.visible: nameHover.hovered && nameLabel.truncated
                                ToolTip.delay: 500
                                ToolTip.text: nameLabel.text
                            }
                            Label {
                                objectName: "labsAccountUserId"
                                Layout.fillWidth: true
                                textFormat: Text.PlainText
                                text: accountRow.userId
                                color: AppTheme.stormTextMuted
                                font.family: AppTheme.monoFont
                                font.pixelSize: AppTheme.fontMonoXS
                                elide: Label.ElideMiddle
                            }
                            NoteLabel {
                                objectName: "labsAccountState"
                                visible: true
                                Layout.minimumHeight: stateMetrics.height
                                wrapMode: Text.NoWrap
                                maximumLineCount: 1
                                elide: Text.ElideRight
                                text: stateWords.stateLabel
                                color: accountRow.unhealthy ? AppTheme.stormDanger
                                                            : AppTheme.stormTextMuted
                                Accessible.ignored: text.length === 0
                                FontMetrics {
                                    id: stateMetrics
                                    font.pixelSize: AppTheme.textMeta
                                }
                            }
                        }
                        AppButton {
                            objectName: "labsSignInAgain_" + accountRow.userId
                            visible: accountRow.canSignInAgain
                            storm: true
                            size: "sm"
                            text: qsTr("Sign in again")
                            onClicked: app.switchToAccount(accountRow.userId)
                        }
                        AppSwitch {
                            id: pinSwitch
                            objectName: "labsKeepRunning_" + accountRow.userId
                            checked: accountRow.keepRunning
                            Accessible.name: qsTr("Keep %1 always signed in")
                                             .arg(accountRow.userId)
                            Accessible.description: stateWords.stateLabel
                            onToggled: root.setKeepRunning(accountRow.userId,
                                                           !accountRow.keepRunning)
                        }
                    }
                    Label {
                        objectName: "labsKeepRunningRefusal"
                        visible: root.refusalUserId.length > 0
                                 && root.refusalUserId === accountRow.userId
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                        text: root.refusalText
                        color: AppTheme.stormDanger
                        font.pixelSize: AppTheme.textMeta
                        Accessible.role: Accessible.AlertMessage
                        Accessible.name: text
                    }
                }
            }
        }
    }
}
