import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings -> About -> Developer tools. One switch (app.settings.developerTools,
// off by default) reveals two read-only views:
//   * the homeserver's spec versions and every optional feature it lists;
//   * an account-data browser, global or for one room, as pretty JSON.
//
// Nothing here writes. Account data whose content is a secret (secret storage,
// cross-signing keys, the backup key) is shown as its TYPE NAME ONLY: the
// content is replaced in Rust before it reaches the UI (CLAUDE.md §6), so
// nothing on this page can reveal it. The plaintext shown for any other type
// stays in this component and is dropped when the tools are switched off.
ColumnLayout {
    id: root
    objectName: "developerToolsSection"

    Layout.fillWidth: true
    spacing: AppTheme.spacing12

    readonly property var about: (typeof app !== "undefined" && app) ? app.about : null
    readonly property bool enabledTools:
        (typeof app !== "undefined" && app) ? app.settings.developerTools : false

    component DevCard: Pane {
        Layout.fillWidth: true
        background: Rectangle {
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            radius: AppTheme.radiusMd
        }
    }

    onEnabledToolsChanged: {
        if (!root.about) return
        if (root.enabledTools) {
            if (root.about.homeserverState === "idle")
                root.about.refreshHomeserver()
        } else {
            root.about.clearAccountData()
        }
    }

    DevCard {
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8
            Label {
                text: qsTr("Developer")
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightStrong
            }
            SettingsCheckBox {
                objectName: "developerToolsSwitch"
                palette.windowText: AppTheme.stormText
                text: qsTr("Developer tools")
                checked: root.enabledTools
                onToggled: app.settings.developerTools = checked
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: qsTr("Shows read-only views of what the homeserver reports "
                           + "and what your account stores. Nothing here changes "
                           + "your account.")
            }
        }
    }

    // Homeserver versions and features
    DevCard {
        visible: root.enabledTools
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8
            Label {
                text: qsTr("Homeserver features")
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightStrong
            }
            Label {
                objectName: "developerHomeserverStatus"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                visible: text !== ""
                text: {
                    const s = root.about ? root.about.homeserverState : "idle"
                    if (s === "loading") return qsTr("Asking the homeserver...")
                    if (s === "failed") return root.about.homeserver.error || ""
                    if (s === "unsupported")
                        return qsTr("This backend cannot ask a homeserver.")
                    return ""
                }
            }
            Label {
                objectName: "developerSpecVersions"
                Layout.fillWidth: true
                wrapMode: Text.WrapAnywhere
                textFormat: Text.PlainText
                color: AppTheme.stormText
                font.family: AppTheme.monoFont
                font.pixelSize: AppTheme.textMeta
                visible: root.about && root.about.homeserverState === "ready"
                text: qsTr("Spec versions: %1")
                      .arg(((root.about ? root.about.homeserver.versions : []) || []).join(", "))
            }
            Repeater {
                objectName: "developerFeatureList"
                model: root.about && root.about.homeserverState === "ready"
                       ? (root.about.homeserver.features || []) : []
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing8
                    Label {
                        text: parent.modelData.enabled ? qsTr("on") : qsTr("off")
                        Layout.preferredWidth: 28
                        color: parent.modelData.enabled
                               ? AppTheme.stormText : AppTheme.stormTextMuted
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.textMeta
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WrapAnywhere
                        textFormat: Text.PlainText
                        text: parent.modelData.name
                        color: parent.modelData.enabled
                               ? AppTheme.stormText : AppTheme.stormTextMuted
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.textMeta
                    }
                }
            }
            AppButton {
                storm: true
                text: qsTr("Refresh")
                enabled: root.about && root.about.homeserverState !== "loading"
                onClicked: root.about.refreshHomeserver()
            }
        }
    }

    // Account data browser
    DevCard {
        visible: root.enabledTools
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8
            Label {
                text: qsTr("Account data")
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightStrong
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: qsTr("Read only. Leave the room ID empty for your account's "
                           + "own data, or enter a room ID such as !abc:example.org. "
                           + "Keys and secrets are never shown: those entries "
                           + "appear as their type name only.")
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                TextField {
                    id: roomField
                    objectName: "developerAccountDataRoom"
                    Layout.fillWidth: true
                    placeholderText: qsTr("Room ID (empty for account-wide)")
                    selectByMouse: true
                    onAccepted: loadButton.clicked()
                }
                AppButton {
                    id: loadButton
                    storm: true
                    objectName: "developerAccountDataLoad"
                    text: qsTr("Load")
                    enabled: root.about && root.about.accountDataState !== "loading"
                    onClicked: root.about.loadAccountData(roomField.text.trim())
                }
            }
            Label {
                objectName: "developerAccountDataStatus"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: AppTheme.textMeta
                visible: text !== ""
                color: root.about && root.about.accountDataState === "failed"
                       ? AppTheme.danger : AppTheme.stormTextMuted
                text: {
                    if (!root.about) return ""
                    const s = root.about.accountDataState
                    if (s === "loading") return qsTr("Loading...")
                    if (s === "unsupported")
                        return qsTr("This backend cannot list account data.")
                    if (s === "failed") return root.about.accountDataError
                    if (s === "ready") {
                        const n = root.about.accountDataEntries.length
                        if (n === 0) return qsTr("No account data found.")
                        return (root.about.accountDataTruncated
                            ? qsTr("%n entries shown; more exist but are not listed.", "", n)
                            : qsTr("%n entries.", "", n))
                    }
                    return ""
                }
            }
            Repeater {
                objectName: "developerAccountDataList"
                model: root.about ? root.about.accountDataEntries : []
                delegate: ColumnLayout {
                    id: entry
                    required property var modelData
                    property bool expanded: false
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing4
                    ItemDelegate {
                        Layout.fillWidth: true
                        padding: AppTheme.spacing4
                        // A redacted entry has nothing to expand.
                        enabled: !entry.modelData.redacted
                        onClicked: entry.expanded = !entry.expanded
                        Accessible.name: entry.modelData.type
                        contentItem: RowLayout {
                            spacing: AppTheme.spacing8
                            Label {
                                text: entry.modelData.redacted ? "•"
                                      : (entry.expanded ? "▾" : "▸")
                                color: AppTheme.stormTextMuted
                            }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WrapAnywhere
                                textFormat: Text.PlainText
                                text: entry.modelData.type
                                color: AppTheme.stormText
                                font.family: AppTheme.monoFont
                                font.pixelSize: AppTheme.textMeta
                            }
                            Label {
                                visible: entry.modelData.redacted
                                text: qsTr("hidden")
                                color: AppTheme.stormTextMuted
                                font.pixelSize: AppTheme.textMeta
                            }
                        }
                        background: Rectangle {
                            color: parent.hovered ? AppTheme.stormInset : "transparent"
                            radius: AppTheme.radiusSm
                        }
                    }
                    TextEdit {
                        visible: entry.expanded && !entry.modelData.redacted
                        Layout.fillWidth: true
                        Layout.leftMargin: AppTheme.spacing20
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.WrapAnywhere
                        textFormat: TextEdit.PlainText
                        text: entry.modelData.json
                              + (entry.modelData.cut ? "\n" + qsTr("... cut") : "")
                        color: AppTheme.stormText
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.textMeta
                    }
                }
            }
        }
    }
}
