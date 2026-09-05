import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// POLICY LISTS — Mjolnir-style ban lists published as room state.
//
// # What this dialog is careful about
//
// A policy list is somebody else's judgement about who should be banned.
// Following one does NOT make Lightning act on it: the app can tell you that
// a list you follow covers someone, and you decide. That is stated on screen
// rather than left for the user to discover, because the opposite behaviour
// is what people reasonably expect from the word "subscribe" and being wrong
// about it means silently not seeing messages.
//
// Publishing a rule is a different act with a different gate: it needs the
// room's power level, and the room this dialog opened on is the one it
// writes to.
Dialog {
    id: root
    objectName: "policyListDialog"

    readonly property var policy: app.policy
    /// The room whose rules are shown. Set by openFor().
    property string roomId: ""
    property string roomName: ""

    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(560, parent ? parent.width - AppTheme.spacing24 * 2 : 560)
    padding: AppTheme.spacing16

    function openFor(id, name) {
        root.roomId = id
        root.roomName = name
        entityField.text = ""
        reasonField.text = ""
        if (root.policy) {
            root.policy.openRoom(id)
            root.policy.refreshSubscriptions()
        }
        open()
    }

    background: Rectangle {
        color: AppTheme.surface
        border.color: AppTheme.border
        radius: AppTheme.radiusLg
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        Label {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            text: root.roomName.length > 0
                  ? qsTr("Moderation rules in %1").arg(root.roomName)
                  : qsTr("Moderation rules")
            color: AppTheme.textPrimary
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightStrong
            elide: Label.ElideRight
        }

        // ── Following ────────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing4
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                CheckBox {
                    objectName: "policyFollowCheck"
                    text: qsTr("Follow this list")
                    checked: root.policy
                             && root.policy.isSubscribed(root.roomId)
                    onToggled: root.policy.setSubscribed(root.roomId, checked)
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
                // The load-bearing sentence in this dialog.
                text: qsTr("Following a list does not block anyone by itself. "
                           + "Lightning will tell you when someone is covered "
                           + "by a list you follow, and you decide what to do "
                           + "— this is somebody else's judgement, not a "
                           + "setting.")
            }
        }

        // ── The rules ────────────────────────────────────────────────────
        Label {
            objectName: "policyCoverageLine"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textMeta
            text: {
                if (!root.policy)
                    return ""
                if (root.policy.loading)
                    return qsTr("Reading the room's rules…")
                if (root.policy.lastError.length > 0)
                    return root.policy.lastError
                // Coerced with `|| 0`: a plural argument that is not a
                // number is a QML error, and a null model during teardown is
                // a real state rather than a hypothetical one.
                var n = (root.policy.rules ? root.policy.rules.count : 0) || 0
                if (root.policy.truncated) {
                    // A bounded read must SAY it was bounded.
                    return qsTr("%n rule(s) — this list is long and only the "
                                + "first were read.", "", n)
                }
                return qsTr("%n rule(s).", "", n)
            }
        }

        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: 220
            clip: true
            model: root.policy ? root.policy.rules : null
            ScrollBar.vertical: AppScrollBar {}
            SmoothWheelArea {}

            delegate: ItemDelegate {
                id: ruleRow
                required property int index
                required property string kind
                required property string entity
                required property bool isBan
                required property string reason

                width: ListView.view.width
                height: 52
                enabled: false
                opacity: 1

                contentItem: RowLayout {
                    spacing: AppTheme.spacing8
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 0
                        Label {
                            Layout.fillWidth: true
                            // A rule's entity is written by whoever controls
                            // the policy room: never markup.
                            textFormat: Text.PlainText
                            text: ruleRow.entity
                            color: AppTheme.textPrimary
                            elide: Label.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            text: {
                                var what = ruleRow.kind === "server"
                                    ? qsTr("everyone on this server")
                                    : (ruleRow.kind === "room"
                                       ? qsTr("this room") : qsTr("this user"))
                                // A rule that is not a ban is shown, not
                                // hidden — the room published it — but it is
                                // marked, because Lightning acts on none of it
                                // and a reader should not assume otherwise.
                                var rec = ruleRow.isBan
                                    ? qsTr("ban") : qsTr("other recommendation")
                                return ruleRow.reason.length > 0
                                    ? qsTr("%1 · %2 · %3").arg(what).arg(rec)
                                          .arg(ruleRow.reason)
                                    : qsTr("%1 · %2").arg(what).arg(rec)
                            }
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.textMeta
                            elide: Label.ElideRight
                        }
                    }
                    AppButton {
                        text: qsTr("Remove")
                        kind: "ghost"
                        size: "sm"
                        visible: root.policy && root.policy.canWrite
                        onClicked: root.policy.removeRule(ruleRow.kind,
                                                          ruleRow.entity)
                    }
                }
            }
        }

        // ── Publishing ───────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing4
            visible: root.policy && root.policy.canWrite

            Label {
                text: qsTr("Add a rule")
                color: AppTheme.textPrimary
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightStrong
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                AppComboBox {
                    id: kindBox
                    Layout.preferredWidth: 130
                    model: [qsTr("User"), qsTr("Server"), qsTr("Room")]
                }
                AppTextField {
                    id: entityField
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    placeholderText: qsTr("@someone:example.org or *.example.org")
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                AppTextField {
                    id: reasonField
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    placeholderText: qsTr("Reason (published with the rule)")
                }
                AppButton {
                    text: qsTr("Publish")
                    kind: "primary"
                    size: "sm"
                    enabled: entityField.text.trim().length > 0
                    onClicked: {
                        var kinds = ["user", "server", "room"]
                        root.policy.addRule(kinds[kindBox.currentIndex],
                                            entityField.text, reasonField.text)
                        entityField.text = ""
                        reasonField.text = ""
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
                text: qsTr("`*` matches any run of characters and `?` matches "
                           + "one. A rule and its reason are public to "
                           + "everyone who can read this room.")
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: root.policy && !root.policy.canWrite
                     && !root.policy.loading
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textMeta
            text: qsTr("You do not have permission to publish rules in this "
                       + "room, so this is a read-only view of its list.")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Done")
                kind: "ghost"
                onClicked: root.close()
            }
        }
    }
}
