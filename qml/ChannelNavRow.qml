import QtQuick
import QtQuick.Controls
import MatrixClient

// A navigation entry at the top of the Channels layout ("Lobby", "Message
// Search"). Not a channel row or category: these aren't rooms, so they have
// no id, unread state, context menu or collapse.
ItemDelegate {
    id: root

    property string label: ""
    property string iconName: ""
    /// True for Lobby while the shell shows the home surface. Not named
    /// `active`, which the hosting Loader already has.
    property bool current: false
    /// Shown greyed and inert with a trailing "Coming soon", rather than
    /// disabled (which gets no hover to explain itself) or removed.
    property bool comingSoon: false

    // Loaded by the Channels presenter (five row kinds); the Loader takes its
    // height from this value.
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
        // Hover fills; focus rings (below). A shared fill made the focused row
        // look like the current one.
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

        // Both behind Loaders: an empty Text keeps ItemObservesViewport, and
        // this row is a per-row delegate.
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
            // Leave room for the trailing badge so the elided label doesn't run
            // under it.
            anchors.rightMargin: root.comingSoon ? 84 : 14
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: Label {
                // Remote or externally chosen text: never markup.
                textFormat: Text.PlainText
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
