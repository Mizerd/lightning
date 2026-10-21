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

    function refreshRows() {
        root.rows = (typeof app !== "undefined" && app && app.accountList)
            ? app.accountList.rowsSnapshot() : null
    }
    Component.onCompleted: root.refreshRows()
    Connections {
        target: typeof app !== "undefined" ? app : null
        // The signal only exists in builds that carry Labs.
        ignoreUnknownSignals: true
        function onLiveAccountsActiveChanged() { root.refreshRows() }
    }
    Connections {
        target: (typeof app !== "undefined" && app && app.accountList)
                ? app.accountList : null
        function onRowsChanged() { root.refreshRows() }
    }

    function setKeepRunning(userId, on) {
        const refusal = app.setAccountKeepRunning(userId, on) || ""
        root.refusalUserId = refusal.length > 0 ? userId : ""
        root.refusalText = refusal
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
                model: root.active ? (root.rows || []) : []
                delegate: ColumnLayout {
                    id: accountRow
                    required property var modelData
                    objectName: "labsAccountRow_" + (modelData.userId || "")
                    Layout.fillWidth: true
                    spacing: 2

                    // Only for the live state's words.
                    AccountActivityBadge {
                        id: stateWords
                        visible: false
                        liveState: accountRow.modelData.liveState !== undefined
                                   ? accountRow.modelData.liveState : 0
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label {
                                objectName: "labsAccountName"
                                Layout.fillWidth: true
                                // Remote or externally chosen text: never markup.
                                textFormat: Text.PlainText
                                text: (accountRow.modelData.displayName || "").length > 0
                                      ? accountRow.modelData.displayName
                                      : (accountRow.modelData.userId || "")
                                color: AppTheme.stormText
                                font.weight: AppTheme.weightStrong
                                elide: Label.ElideRight
                            }
                            Label {
                                objectName: "labsAccountUserId"
                                Layout.fillWidth: true
                                // Remote or externally chosen text: never markup.
                                textFormat: Text.PlainText
                                text: accountRow.modelData.userId || ""
                                color: AppTheme.stormTextMuted
                                font.family: AppTheme.monoFont
                                font.pixelSize: AppTheme.fontMonoXS
                                elide: Label.ElideMiddle
                            }
                            NoteLabel {
                                objectName: "labsAccountState"
                                visible: text.length > 0
                                text: stateWords.stateLabel
                            }
                        }
                        Label {
                            text: qsTr("Always signed in")
                            color: AppTheme.stormTextSecondary
                            font.pixelSize: AppTheme.textMeta
                            Accessible.ignored: true
                        }
                        AppSwitch {
                            objectName: "labsKeepRunning_" + (accountRow.modelData.userId || "")
                            checked: accountRow.modelData.keepRunning === true
                            Accessible.name: qsTr("Keep %1 always signed in")
                                             .arg(accountRow.modelData.userId || "")
                            onToggled: root.setKeepRunning(
                                accountRow.modelData.userId || "",
                                !(accountRow.modelData.keepRunning === true))
                        }
                    }
                    Label {
                        objectName: "labsKeepRunningRefusal"
                        visible: root.refusalUserId.length > 0
                                 && root.refusalUserId === (accountRow.modelData.userId || "")
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
