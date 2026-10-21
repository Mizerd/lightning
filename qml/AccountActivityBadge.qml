import QtQuick
import QtQuick.Controls
import MatrixClient

// One account's activity mark on an account surface (switcher row, quick
// switcher entry): Labs, concurrent accounts. The ONE place the badge rule
// lives, so the surfaces cannot drift apart:
//
// - a NUMBER only for mentions and highlights ("N+" when the count is a lower
//   bound: encrypted rooms counted client-side may miss some);
// - otherwise a dot when the account has unread rooms or invites;
// - nothing unless the account is running in the background (a paused or
//   non-live account's counts are not current, so they are not shown);
// - nothing on the active account's row (its rooms are already on screen).
//
// The inputs are plain values the owner copies out of AccountActivityFeed.
// With Labs off every input stays at its default and nothing renders.
UnreadBadge {
    id: root

    property int mentionCount: 0
    property bool mentionsLowerBound: false
    property bool hasUnread: false
    // AccountActivityModel.LiveState; 0 (NotLive) is the default.
    property int liveState: 0
    property bool activeAccount: false

    readonly property bool running: root.liveState === AccountActivityModel.Running
    readonly property bool shown: !root.activeAccount && root.running
                                  && (root.mentionCount > 0 || root.hasUnread)

    count: root.shown ? Math.max(0, root.mentionCount) : 0
    mention: root.count > 0
    dot: root.shown && root.mentionCount <= 0
    text: root.dotOnly ? ""
          : root.count > 99 ? "99+"
          : root.mentionsLowerBound ? qsTr("%1+").arg(root.count)
          : String(root.count)

    // The words for a live state, for accessible names and the Labs card.
    // NotLive has none.
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
    // What the badge says, for the row's accessible description.
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
