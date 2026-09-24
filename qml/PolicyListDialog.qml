import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Policy lists: Mjolnir-style ban lists published as room state. Following one
// does not make Lightning act on it; the app tells you a followed list covers
// someone and you decide, and the dialog says so. Publishing a rule needs the
// room's power level and writes to the room this dialog opened on.
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

        // Following
        ColumnLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing4
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                CheckBox {
                    objectName: "policyFollowCheck"
                    text: qsTr("Follow this list")
                    // The notifying list, not isSubscribed(), which is a
                    // Q_INVOKABLE with no binding dependency.
                    checked: root.policy
                             && root.policy.subscriptions.indexOf(root.roomId)
                                >= 0
                    // Restore the binding: a user toggle assigns `checked` and
                    // destroys it, and setSubscribed is asynchronous and can
                    // fail.
                    onToggled: {
                        root.policy.setSubscribed(root.roomId, checked)
                        checked = Qt.binding(function() {
                            return root.policy
                                && root.policy.subscriptions
                                       .indexOf(root.roomId) >= 0
                        })
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
                // The key sentence in this dialog.
                text: qsTr("Following a list does not block anyone by itself. "
                           + "Lightning will tell you when someone is covered "
                           + "by a list you follow, and you decide what to do "
                           + "— this is somebody else's judgement, not a "
                           + "setting.")
            }
        }

        // The rules
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
                // `|| 0`: a non-number plural argument is an error, and a null
                // model during teardown is a real state.
                var n = (root.policy.rules ? root.policy.rules.count : 0) || 0
                if (root.policy.truncated) {
                    // A bounded read says it was bounded.
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
                // The rule's own state key, which removal writes to.
                required property string stateKey

                width: ListView.view.width
                height: 52
                // Not `enabled: false`: enabled propagates and would disable
                // the Remove button inside. The row has no onClicked and
                // suppresses its hover highlight.
                hoverEnabled: false
                background: null

                contentItem: RowLayout {
                    spacing: AppTheme.spacing8
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 0
                        Label {
                            Layout.fillWidth: true
                            // Written by whoever controls the policy room:
                            // never markup.
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
                                // A non-ban rule is shown but marked: Lightning
                                // acts on none of it.
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
                                                          ruleRow.entity,
                                                          ruleRow.stateKey)
                    }
                }
            }
        }

        // Publishing
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
