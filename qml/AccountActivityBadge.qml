import QtQuick
import QtQuick.Controls
import MatrixClient

UnreadBadge {
    id: root

    property int mentionCount: 0
    property bool mentionsLowerBound: false
    property bool hasUnread: false
    property int liveState: 0
    property bool activeAccount: false

    readonly property bool running: root.liveState === AccountActivityModel.Running
    readonly property bool shown: !root.activeAccount && root.running
                                  && (root.mentionCount > 0 || root.hasUnread)

    count: root.shown ? Math.max(0, root.mentionCount) : 0
    mention: root.count > 0
    dot: root.shown && root.mentionCount <= 0
    text: root.dotOnly ? ""
          : root.count > 99 ? "@99+"
          : root.mentionsLowerBound ? "@" + qsTr("%1+").arg(root.count)
          : "@" + String(root.count)

    readonly property string stateLabel: {
        switch (root.liveState) {
        case AccountActivityModel.Active: return qsTr("Active")
        case AccountActivityModel.Running: return qsTr("Running")
        case AccountActivityModel.Starting: return qsTr("Starting")
        case AccountActivityModel.Paused: return qsTr("Paused (limit reached)")
        case AccountActivityModel.NeedsSignIn: return qsTr("Needs sign-in")
        case AccountActivityModel.KeyringLocked: return qsTr("Keyring locked")
        case AccountActivityModel.Stopped: return qsTr("Stopped (error)")
        case AccountActivityModel.StoreBusy: return qsTr("Store busy (restart to reopen)")
        case AccountActivityModel.EncryptionDataMissing: return qsTr("Encryption data missing")
        case AccountActivityModel.OpenToFinishSetup: return qsTr("Open to finish setup")
        case AccountActivityModel.Stopping: return qsTr("Stopping")
        default: return ""
        }
    }
    readonly property string activityLabel: !root.shown ? ""
        : root.count > 0
          ? (root.mentionsLowerBound
             ? qsTr("At least %n mention(s)", "", root.count)
             : qsTr("%n mention(s)", "", root.count))
          : qsTr("New messages")

    Accessible.role: Accessible.StaticText
    Accessible.name: root.activityLabel
    Accessible.ignored: !root.shown
}
