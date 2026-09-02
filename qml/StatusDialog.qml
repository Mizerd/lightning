import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.9 (phase 10): set / edit / clear the account's personal status. The
// text is published as the spec presence status_msg (with the emoji as its
// first characters, so it federates to every client); the expiry is a
// Lightning-side convenience — PresenceManager clears the status when the
// deadline passes, on a timer while running and on the next start
// otherwise. Nothing here pretends the expiry itself federates.
Dialog {
    id: root
    objectName: "statusDialog"
    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    width: Math.min(440, parent ? parent.width - AppTheme.spacing24 * 2 : 440)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    readonly property var presence: app.presence
    property string expiryChoice: "none"

    function openForEdit() {
        emojiField.text = presence ? presence.ownStatusEmoji : ""
        textField.text = presence ? presence.ownStatusText.replace(
                                        presence.ownStatusEmoji, "").trim() : ""
        // An existing deadline shows as "custom" with its time filled in.
        if (presence && presence.ownStatusExpiresAtMs > 0) {
            root.expiryChoice = "custom"
            var d = new Date(presence.ownStatusExpiresAtMs)
            customDate.text = Qt.formatDate(d, "yyyy-MM-dd")
            customTime.text = Qt.formatTime(d, "HH:mm")
        } else {
            root.expiryChoice = "none"
            customDate.text = Qt.formatDate(new Date(), "yyyy-MM-dd")
            customTime.text = Qt.formatTime(new Date(), "HH:mm")
        }
        open()
        Qt.callLater(function () { textField.forceActiveFocus() })
    }

    // The chosen deadline in wall-clock ms, or 0. Local time throughout —
    // the picker says so in its labels.
    function expiresAtMs() {
        var now = new Date()
        switch (root.expiryChoice) {
        case "30m": return now.getTime() + 30 * 60 * 1000
        case "1h": return now.getTime() + 60 * 60 * 1000
        case "4h": return now.getTime() + 4 * 60 * 60 * 1000
        case "today": {
            var end = new Date(now.getFullYear(), now.getMonth(), now.getDate(),
                               23, 59, 59, 0)
            return end.getTime()
        }
        case "custom": {
            var parsed = Date.fromLocaleString(Qt.locale(),
                                               customDate.text + " " + customTime.text,
                                               "yyyy-MM-dd HH:mm")
            if (isNaN(parsed.getTime()) || parsed.getTime() <= now.getTime())
                return -1
            return parsed.getTime()
        }
        default: return 0
        }
    }
    readonly property bool customInvalid: root.expiryChoice === "custom"
                                          && root.expiresAtMs() < 0

    background: Rectangle {
        radius: AppTheme.radiusLg
        color: AppTheme.stormPanel
        border.color: AppTheme.stormBorder
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12
        Label {
            text: qsTr("Set a status")
            color: AppTheme.stormText
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightBold
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            AppTextField {
                id: emojiField
                objectName: "statusEmojiField"
                storm: true
                Layout.preferredWidth: 64
                placeholderText: "🙂"
                maximumLength: 8
                horizontalAlignment: TextInput.AlignHCenter
                font: app.textFontWithEmoji(AppTheme.uiFont, AppTheme.scaled(16))
                Accessible.name: qsTr("Status emoji")
            }
            AppTextField {
                id: textField
                objectName: "statusTextField"
                storm: true
                Layout.fillWidth: true
                placeholderText: qsTr("Working from home")
                maximumLength: 200
                onAccepted: setButton.clicked()
                Accessible.name: qsTr("Status text")
            }
        }
        Label {
            text: qsTr("Clear after (your local time)")
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
        }
        SegmentedControl {
            id: expirySegments
            objectName: "statusExpirySegments"
            storm: true
            dense: true
            fitWidth: true
            Layout.fillWidth: true
            model: [
                { value: "none", label: qsTr("Never") },
                { value: "30m", label: qsTr("30 min") },
                { value: "1h", label: qsTr("1 hour") },
                { value: "4h", label: qsTr("4 hours") },
                { value: "today", label: qsTr("Today") },
                { value: "custom", label: qsTr("Custom") }
            ]
            current: root.expiryChoice
            onActivated: (value) => root.expiryChoice = value
        }
        RowLayout {
            Layout.fillWidth: true
            visible: root.expiryChoice === "custom"
            spacing: AppTheme.spacing8
            AppTextField {
                id: customDate
                objectName: "statusCustomDate"
                storm: true
                Layout.fillWidth: true
                placeholderText: qsTr("yyyy-mm-dd")
                inputMask: "9999-99-99"
            }
            AppTextField {
                id: customTime
                objectName: "statusCustomTime"
                storm: true
                Layout.preferredWidth: 90
                placeholderText: qsTr("hh:mm")
                inputMask: "99:99"
            }
        }
        Label {
            visible: root.customInvalid
            text: qsTr("Pick a date and time in the future.")
            color: AppTheme.stormDanger
            font.pixelSize: AppTheme.textMeta
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            text: qsTr("Everyone on Matrix sees the emoji and text as your "
                       + "presence status. The clear-after time is handled by "
                       + "Lightning on this account.")
        }
        RowLayout {
            Layout.fillWidth: true
            AppButton {
                objectName: "statusClearButton"
                text: qsTr("Clear status")
                kind: "ghost"
                visible: root.presence && root.presence.ownStatusText.length > 0
                onClicked: {
                    root.presence.clearOwnStatus()
                    root.close()
                }
            }
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Cancel")
                kind: "ghost"
                onClicked: root.close()
            }
            AppButton {
                id: setButton
                objectName: "statusSetButton"
                text: qsTr("Set status")
                kind: "primary"
                enabled: !root.customInvalid
                         && (emojiField.text.trim().length > 0
                             || textField.text.trim().length > 0)
                onClicked: {
                    var at = root.expiresAtMs()
                    if (at < 0)
                        return
                    root.presence.setOwnStatus(emojiField.text, textField.text, at)
                    root.close()
                }
            }
        }
    }
}
