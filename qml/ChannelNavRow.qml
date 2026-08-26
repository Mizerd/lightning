import QtQuick
import QtQuick.Controls
import MatrixClient

// A navigation entry at the top of the Channels layout — "Lobby" and
// "Message Search".
//
// Deliberately NOT a channel row and NOT a category. Neither of these is a
// Matrix room: they have no id, no unread state, no context menu and nothing
// to collapse. Giving them a room row's affordances would offer a mute toggle
// on a search box.
ItemDelegate {
    id: root

    property string label: ""
    property string iconName: ""
    /// True for Lobby while the shell is actually showing the home surface.
    /// NOT named `active`: this row is always created by a Loader, which has
    /// an `active` of its own, and one name for two things a few lines apart
    /// is a reading hazard.
    property bool current: false
    /// Shown, greyed and inert, with a trailing "Coming soon".
    ///
    /// NOT `enabled: false` and NOT removed. A disabled Qt Quick control
    /// receives no hover events, so it cannot explain itself; and removing
    /// the row would leave the user wondering whether the feature exists at
    /// all. The row stays, says what it is, and does nothing when pressed.
    property bool comingSoon: false

    // A Loader-hosted row: the Channels presenter picks between five row
    // kinds, so this is loaded rather than declared inline, and the Loader
    // takes its height from this value.
    height: 32
    padding: 0
    hoverEnabled: true

    Accessible.role: Accessible.Button
    Accessible.name: root.comingSoon
                     ? qsTr("%1 — coming soon").arg(root.label) : root.label
    ToolTip.text: qsTr("Not available yet")
    ToolTip.visible: root.comingSoon && root.hovered
    ToolTip.delay: 400

    background: Rectangle {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        anchors.topMargin: 1
        anchors.bottomMargin: 1
        radius: AppTheme.radiusSm
        // Hover FILLS; focus RINGS (below). Sharing one fill made the row
        // that merely holds keyboard focus — the first one in the column, by
        // default — indistinguishable from the row you are actually in.
        color: root.comingSoon ? "transparent"
               : root.current ? AppTheme.channelSelected
                            : (root.hovered || root.activeFocus
                               ? AppTheme.channelHover : "transparent")
        border.width: root.activeFocus ? 2 : 0
        border.color: AppTheme.focusRing
        Behavior on color {
            ColorAnimation {
                duration: 90
            }
        }
    }

    contentItem: Item {
        anchors.fill: parent

        Icon {
            id: glyph
            anchors.verticalCenter: parent.verticalCenter
            x: 14
            name: root.iconName
            size: 16
            color: root.comingSoon ? AppTheme.channelCategoryText
                   : root.current ? AppTheme.channelSelectedText
                                  : AppTheme.channelText
        }

        // Both Loaders: a never-laid-out empty Text keeps
        // ItemObservesViewport forever, and this row is instantiated per
        // model row like every other delegate in the column.
        Loader {
            active: root.comingSoon
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: Label {
                id: soonLabel
                text: qsTr("Coming soon")
                font.pixelSize: AppTheme.textMicro
                color: AppTheme.channelCategoryText
            }
        }

        Loader {
            active: root.label.length > 0
            anchors.left: glyph.right
            anchors.leftMargin: 8
            anchors.right: parent.right
            // Leaves room for the trailing badge rather than running under
            // it: the label elides, so without this the two overlap at any
            // column width the name does not already fit.
            anchors.rightMargin: root.comingSoon ? 84 : 14
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: Label {
                text: root.label
                elide: Text.ElideRight
                maximumLineCount: 1
                font.pixelSize: AppTheme.textBody
                color: root.comingSoon ? AppTheme.channelCategoryText
                       : root.current ? AppTheme.channelSelectedText
                                      : AppTheme.channelText
            }
        }
    }
}
