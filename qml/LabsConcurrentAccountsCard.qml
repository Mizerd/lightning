import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings -> Labs -> Run several accounts at once (concurrent live accounts,
// Phase 1).
//
// Always offered (lead decision 2026-10-20, replacing DECISION 1's "hidden
// until Phase 2"), OFF by default. It hides only when the test override
// LIGHTNING_LABS_LIVE_ACCOUNTS forces Labs off (`liveAccountsLabsAvailable`
// false), and then its switch cannot be reached and a stored
// `labs/concurrentAccounts = true` does nothing.
//
// The card is also where the user picks which accounts stay "always signed
// in": once the switch is on, every saved account is listed under that
// heading with its own switch, and while it is off the card says that this is
// where the choice is made.
//
// The master switch is `app.settings.labsConcurrentAccounts`; AppController
// reacts to it. The per-account rows come from `app.accountList`, which exists
// only while Labs is active and reads no secret. Its `rowsSnapshot()` is a
// Q_INVOKABLE, so the rows are copied into `rows` by handlers and never read
// inside a binding. A pin goes through `app.setAccountKeepRunning()`, which
// answers "" or a translated refusal shown under that account's row.
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

    // AccountListModel rows, or null while Labs is not active. Written only by
    // refreshRows().
    property var rows: null
    // The last refusal, shown under that account's row until the next change.
    property string refusalUserId: ""
    property string refusalText: ""

    // The rows the Repeater shows: updated IN PLACE (a delegate keeps its
    // identity, so keyboard focus and a pointer stay on the same account's
    // switch) and in a STABLE order, the order the card first saw them in -
    // never "pinned first", which moved an account to the bottom on its own
    // click (C54-D04, D05). New accounts append.
    ListModel { id: rowModel }
    property var order: []
    // How many accounts were pinned when the refusal was shown. The refusal is
    // about that count, so it is dropped when the count moves (C54-F01).
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
        // A refusal changes nothing, so the count is still the one asked at.
        root.refusalPinned = pinnedAtAsk
    }

    // SettingsScreen's SettingsCard and SettingsGroupLabel are inline
    // components there and are not visible from this file; same plane and the
    // same heading ink, as LogExportCard does.
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
            // Where the "always signed in" choice is, said while it is not
            // shown yet.
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
            // On, but nothing can run in this session (no Rust backend).
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

            // The picker: which accounts stay signed in in the background.
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

                    // Words that say an account is not healthy: said in the
                    // danger ink, never in the muted grey of "Running".
                    readonly property bool unhealthy:
                        liveState === AccountActivityModel.NeedsSignIn
                        || liveState === AccountActivityModel.KeyringLocked
                        || liveState === AccountActivityModel.Stopped
                        || liveState === AccountActivityModel.StoreBusy
                        || liveState === AccountActivityModel.EncryptionDataMissing
                        || liveState === AccountActivityModel.OpenToFinishSetup
                    // A fresh sign-in helps only where the sign-in is the
                    // problem. A locked keyring is not one (CLAUDE.md s.6).
                    readonly property bool canSignInAgain:
                        liveState === AccountActivityModel.NeedsSignIn
                        || liveState === AccountActivityModel.OpenToFinishSetup

                    // Only for the live state's words.
                    AccountActivityBadge {
                        id: stateWords
                        visible: false
                        liveState: accountRow.liveState
                    }

                    Rectangle {
                        objectName: "labsAccountSeparator"
                        // Present on every row (the first draws none), so
                        // all rows are one height.
                        opacity: accountRow.index > 0 ? 1 : 0
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: AppTheme.stormBorder
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        // The row never grows when "Sign in again" appears.
                        Layout.minimumHeight: AppTheme.buttonHeightSm
                        spacing: AppTheme.spacing12
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label {
                                id: nameLabel
                                objectName: "labsAccountName"
                                Layout.fillWidth: true
                                // Remote or externally chosen text: never markup.
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
                                // Remote or externally chosen text: never markup.
                                textFormat: Text.PlainText
                                text: accountRow.userId
                                color: AppTheme.stormTextMuted
                                font.family: AppTheme.monoFont
                                font.pixelSize: AppTheme.fontMonoXS
                                elide: Label.ElideMiddle
                            }
                            // One line is reserved on EVERY row, so a state
                            // appearing never moves the switches below it
                            // (C54-F04, D05).
                            NoteLabel {
                                objectName: "labsAccountState"
                                visible: true
                                Layout.minimumHeight: stateMetrics.height
                                // One line, always: a long sentence in a
                                // narrow window elides (the full text is the
                                // accessible description) instead of growing
                                // the row.
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
                        // Our own sentence, but plain: it is never markup.
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
