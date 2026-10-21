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
    // Capped to the window; the content scrolls (Add a rule and Done were
    // unreachable below the window edge).
    height: Math.min(implicitHeight, parent ? parent.height - AppTheme.spacing24 * 2
                                              : implicitHeight)

    /// The entity awaiting a second, explicit Publish: a glob that matches
    /// everything. Cleared on publish, cancel and reopen.
    property string pendingBroadEntity: ""

    /// True when a rule's entity matches EVERY entity of its kind: at least
    /// one `*`, and nothing literal besides globs and the sigils and
    /// separators every id has (`*`, `@*:*`, `*.*`, `!*:*`). Legitimate in a
    /// Mjolnir-style list, so it is confirmed, never refused.
    function entityMatchesEverything(entity) {
        var text = String(entity || "").trim()
        return text.indexOf("*") >= 0
            && text.replace(/[*?@!#:.]/g, "").length === 0
    }

    /// Publish the typed rule. A broad entity waits for `confirmed`; returns
    /// whether the rule went out.
    function publishRule(confirmed) {
        var entity = entityField.text.trim()
        if (entity.length === 0 || !root.policy)
            return false
        if (!confirmed && root.entityMatchesEverything(entity)) {
            root.pendingBroadEntity = entity
            return false
        }
        var kinds = ["user", "server", "room"]
        root.policy.addRule(kinds[kindBox.currentIndex], entity, reasonField.text)
        root.pendingBroadEntity = ""
        entityField.text = ""
        reasonField.text = ""
        return true
    }

    function openFor(id, name) {
        root.roomId = id
        root.roomName = name
        root.pendingBroadEntity = ""
        entityField.text = ""
        reasonField.text = ""
        kindBox.currentIndex = 0
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

    // Qt's ScrollView never scrolls a focused child into view, so Tab in a
    // window shorter than the dialog landed on controls nobody could see.
    function ensureVisible(item) {
        var flick = scroller.contentItem
        if (!item || !flick || !flick.contentHeight)
            return
        for (var p = item; p && p !== bodyColumn; p = p.parent) {}
        if (!p)
            return
        var top = item.mapToItem(bodyColumn, 0, 0).y
        var bottom = top + item.height
        var y = flick.contentY
        if (top < y)
            y = top
        else if (bottom > y + flick.height)
            y = bottom - flick.height
        flick.contentY = Math.max(0, Math.min(y, flick.contentHeight - flick.height))
    }
    Connections {
        target: scroller.Window.window
        function onActiveFocusItemChanged() {
            root.ensureVisible(scroller.Window.window.activeFocusItem)
        }
    }

    contentItem: ScrollView {
        id: scroller
        objectName: "policyScroller"
        contentWidth: availableWidth
        implicitHeight: bodyColumn.implicitHeight
        clip: true

    ColumnLayout {
        id: bodyColumn
        width: scroller.availableWidth
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

        // The list and its middle-click autoscroll, which must be a sibling
        // of the view over the same area.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 220
            ListView {
                id: ruleList
                objectName: "policyRuleList"
                anchors.fill: parent
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
                            objectName: "policyRemoveButton"
                            text: qsTr("Remove")
                            kind: "ghost"
                            size: "sm"
                            // Each button names its rule: eight identical
                            // "Remove" are unusable with a screen reader.
                            Accessible.name: qsTr("Remove rule %1").arg(ruleRow.entity)
                            visible: root.policy && root.policy.canWrite
                            // Tab onto a row that is scrolled out of view brings
                            // it into view; a keyboard removal is never of a
                            // rule nobody can see.
                            onActiveFocusChanged: if (activeFocus)
                                ruleList.positionViewAtIndex(ruleRow.index,
                                                             ListView.Contain)
                            onClicked: root.policy.removeRule(ruleRow.kind,
                                                              ruleRow.entity,
                                                              ruleRow.stateKey)
                        }
                    }
                }
            }
            MiddleClickScroller {
                objectName: "policyMiddleClickScroller"
                anchors.fill: parent
                z: 1
                view: ruleList
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
                    Accessible.name: qsTr("Kind of rule")
                    Layout.preferredWidth: 130
                    model: [qsTr("User"), qsTr("Server"), qsTr("Room")]
                }
                AppTextField {
                    id: entityField
                    objectName: "policyEntityField"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    placeholderText: qsTr("@someone:example.org or *.example.org")
                    // Return publishes, as the button does (a broad entity
                    // still waits for its explicit second confirmation).
                    onAccepted: if (text.trim().length > 0
                                    && !broadConfirm.visible)
                        root.publishRule(false)
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
                             && !broadConfirm.visible
                    onClicked: root.publishRule(false)
                }
            }
            // A rule matching everyone (`*`) is a ban on the whole kind for
            // every follower of this list: say so and ask once more.
            ColumnLayout {
                id: broadConfirm
                objectName: "policyBroadConfirm"
                Layout.fillWidth: true
                spacing: AppTheme.spacing4
                // Editing the entity away from the pending one withdraws it.
                visible: root.pendingBroadEntity.length > 0
                         && root.pendingBroadEntity === entityField.text.trim()
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    color: AppTheme.danger
                    font.pixelSize: AppTheme.textMeta
                    text: {
                        var what = kindBox.currentIndex === 1
                            ? qsTr("every server")
                            : (kindBox.currentIndex === 2 ? qsTr("every room")
                                                          : qsTr("every user"))
                        return qsTr("“%1” matches %2. Everyone who follows this "
                                    + "list will be told it bans all of them.")
                            .arg(root.pendingBroadEntity).arg(what)
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing8
                    Item { Layout.fillWidth: true }
                    AppButton {
                        text: qsTr("Cancel")
                        kind: "ghost"
                        size: "sm"
                        onClicked: root.pendingBroadEntity = ""
                    }
                    AppButton {
                        objectName: "policyBroadPublish"
                        text: qsTr("Publish anyway")
                        kind: "danger"
                        size: "sm"
                        onClicked: root.publishRule(true)
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
}
