import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Corner card for personal chat backgrounds kept on the homeserver
// (app.backdrops, ChatBackdropController). Two one-time questions, never
// both at once:
//
//   * Before pictures chosen earlier (before sync existed, or while it was
//     off) are uploaded for the first time: "Lightning will now keep your
//     chat backgrounds on your homeserver, encrypted." OK uploads them;
//     "Don't keep them on my homeserver" turns the account-wide switch off.
//   * A picture only this device has, while the homeserver holds a different
//     one for the same place: both are kept until the user picks one here.
//
// Non-modal and in the chat shell only, like the other corner prompts.
Rectangle {
    id: root

    property bool blocked: false

    readonly property bool available: typeof app !== "undefined" && app
                                      && !!app.backdrops
    readonly property bool noticeShown: available
                                        && app.backdrops.migrationNoticeNeeded
    readonly property var conflict: available
                                    && app.backdrops.syncConflicts.length > 0
                                    ? app.backdrops.syncConflicts[0] : null
    // A conflict between two pictures that look alike: "Use the synced one"
    // is the pre-selected (primary) answer.
    readonly property bool syncedPreselected: !noticeShown && conflict !== null
                                              && conflict.looksSame === true
    readonly property bool shouldShow: available && !blocked
                                       && app.currentScreen === 1
                                       && (noticeShown || conflict !== null)

    objectName: "backgroundSyncPrompt"
    visible: opacity > 0
    opacity: shouldShow ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 140 } }

    width: 316
    implicitHeight: promptColumn.implicitHeight + AppTheme.spacing16 * 2
    height: implicitHeight
    radius: AppTheme.radiusLg
    color: AppTheme.stormPanel
    border.color: AppTheme.stormBorder
    border.width: 1

    Accessible.role: Accessible.AlertMessage
    Accessible.name: titleLabel.text

    ColumnLayout {
        id: promptColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: AppTheme.spacing16
        anchors.rightMargin: AppTheme.spacing16
        spacing: AppTheme.spacing8

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Icon {
                name: "image"
                size: 18
                color: AppTheme.stormTextMuted
                Layout.alignment: Qt.AlignVCenter
            }
            Label {
                id: titleLabel
                objectName: "backgroundSyncPromptTitle"
                Layout.fillWidth: true
                textFormat: Text.PlainText
                text: root.noticeShown
                      ? qsTr("Chat backgrounds on your homeserver")
                      : qsTr("Two different backgrounds")
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightBold
                // Wraps: the notice's title does not fit 284 px (seen
                // elided live), and a heading must not lose its last word.
                wrapMode: Text.WordWrap
            }
        }

        Label {
            objectName: "backgroundSyncPromptBody"
            Layout.fillWidth: true
            textFormat: Text.PlainText
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            text: {
                if (root.noticeShown)
                    return qsTr("Lightning will now keep your chat backgrounds "
                                + "on your homeserver, encrypted, so they follow "
                                + "you to your other devices. The key to each "
                                + "picture is kept in your account data, which "
                                + "your homeserver's administrator can read.")
                if (!root.conflict)
                    return ""
                // Room names are remote text: plain text only (textFormat).
                // Only a picture that really differs gets here: the same
                // picture coming back is recognised and adopted silently.
                const body = root.conflict.scope === ""
                       ? qsTr("A different background for every room came "
                              + "from your other device.")
                       : qsTr("A different background for %1 came from your "
                              + "other device.")
                         .arg(root.conflict.name !== "" ? root.conflict.name
                                                        : qsTr("a room"))
                // Alike pixels are a hint, never a decision: the synced one
                // is pre-selected, the user still picks.
                return root.syncedPreselected
                       ? body + " " + qsTr("They look the same.")
                       : body
            }
        }

        // The notice's second label is long: its buttons stack, full width,
        // rather than squeeze two into a 284 px row (seen clipped live).
        GridLayout {
            objectName: "backgroundSyncPromptButtons"
            Layout.fillWidth: true
            columns: root.noticeShown ? 1 : 2
            rowSpacing: AppTheme.spacing8
            columnSpacing: AppTheme.spacing8
            AppButton {
                objectName: "backgroundSyncPromptPrimary"
                storm: true
                kind: root.syncedPreselected ? "secondary" : "primary"
                Layout.fillWidth: true
                text: root.noticeShown ? qsTr("OK") : qsTr("Use this one")
                onClicked: {
                    if (root.noticeShown)
                        app.backdrops.acknowledgeMigration(false)
                    else if (root.conflict)
                        app.backdrops.resolveConflict(root.conflict.scope, true)
                }
            }
            AppButton {
                objectName: "backgroundSyncPromptSecondary"
                storm: true
                kind: root.syncedPreselected ? "primary" : "secondary"
                Layout.fillWidth: true
                // Off is account-wide: the label says so (review R5).
                text: root.noticeShown ? qsTr("Don't keep them on my homeserver")
                                       : qsTr("Use the synced one")
                onClicked: {
                    if (root.noticeShown)
                        app.backdrops.acknowledgeMigration(true)
                    else if (root.conflict)
                        app.backdrops.resolveConflict(root.conflict.scope, false)
                }
            }
        }
    }
}
