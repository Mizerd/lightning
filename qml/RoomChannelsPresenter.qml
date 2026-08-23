import QtQuick
import QtQuick.Controls
import MatrixClient

// The Channels navigation layout's list body.
//
// A PRESENTER: it owns no header, no search field and no dialogs — RoomsPanel
// (the host) owns those and swaps this in place of the Classic list. The
// split exists so the two layouts cannot fork the surrounding chrome; every
// user of either layout gets the same workspace header and the same ⌘K hint.
//
// Design reference is Sable first, as directed: hierarchy order, categories
// as quiet all-caps headers, channels as single 32px rows with a glyph and a
// name. Discord informed only the interaction (chevron rotation, the collapsed
// group still reporting activity). No Discord or Sable code, asset, colour or
// wording is used — every value comes from AppTheme.
//
// What this layout deliberately CANNOT show, and why the host keeps Classic
// reachable: this is the active Space's hierarchy, so DMs that belong to no
// Space, invites, and favourites are not in it. At Home there is no
// hierarchy at all — the host falls back to Classic there rather than
// rendering an empty column, because "your Space has no channels" and "you
// are not in a Space" are different facts.
Item {
    id: root

    /// The room the timeline is showing, so the active row can be marked.
    property string currentRoomId: ""

    // The filter chips above this presenter write the SAME per-account
    // preference the Classic list follows. Without this binding they were
    // visible and inert in Channels mode — reported as "in channels mode all
    // list doesn't show people and in people list doesn't show people".
    Binding {
        target: app.spaceChannels
        property: "filterMode"
        value: app.settings.roomFilterMode
    }

    signal roomActivated(string roomId)
    /// A category was opened as a Space in its own right (long-press / the
    /// context action), which re-roots the model at it.
    signal spaceActivated(string spaceId)

    // Empty state. Two distinct ones, because conflating them tells the user
    // the wrong thing about what to do next.
    Loader {
        anchors.centerIn: parent
        width: parent.width - AppTheme.spacing24 * 2
        active: app.spaceChannels.emptyHierarchy
                && app.spaceChannels.count === 0
        visible: active
        sourceComponent: Label {
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textBody
            text: qsTr("This space has no channels yet. Rooms added to it " + "will show up here.")
        }
    }

    ListView {
        id: channelList
        objectName: "channelList"
        anchors.fill: parent
        clip: true
        model: app.spaceChannels
        currentIndex: -1
        spacing: 0
        // Rows are 30-32px, so one extra screen is a much smaller number of
        // delegates than the Classic list needs.
        cacheBuffer: 400
        // Recycling is safe: ChannelDelegate keeps no per-instance state that
        // outlives its roomId, and it re-queries the mute mode on every id
        // change precisely so a recycled row cannot inherit one.
        reuseItems: true

        ScrollBar.vertical: AppScrollBar {
            policy: ScrollBar.AsNeeded
        }

        // No section.property. The MODEL is already ordered and grouped by the
        // hierarchy, and a ListView section header on top of the category rows
        // would draw the same grouping twice.
        header: Item {
            width: channelList.width
            height: AppTheme.spacing8
        }
        footer: Item {
            width: channelList.width
            height: AppTheme.spacing12
        }

        delegate: Loader {
            id: rowLoader
            width: channelList.width
            required property var model
            required property int index

            // One Loader choosing between two components, rather than one
            // delegate with everything in it behind visibility flags: a
            // category and a channel share no geometry and no controls, and a
            // combined delegate would instantiate both for every row.
            sourceComponent: rowLoader.model.kind === "category" ? categoryComponent : channelComponent

            Component {
                id: channelComponent
                ChannelDelegate {
                    width: channelList.width
                    roomId: rowLoader.model.roomId
                    channelName: rowLoader.model.name
                    isDirect: rowLoader.model.isDirect
                    encrypted: rowLoader.model.encrypted
                    unreadCount: rowLoader.model.unreadCount
                    highlightCount: rowLoader.model.highlightCount
                    hasUnread: rowLoader.model.hasUnread
                    depth: rowLoader.model.depth
                    active: rowLoader.model.roomId === root.currentRoomId
                    onClicked: root.roomActivated(rowLoader.model.roomId)
                }
            }

            Component {
                id: categoryComponent
                ChannelCategoryHeader {
                    width: channelList.width
                    categoryId: rowLoader.model.roomId
                    categoryName: rowLoader.model.name
                    collapsed: rowLoader.model.collapsed
                    hiddenUnread: rowLoader.model.hiddenUnread
                    hiddenHighlight: rowLoader.model.hiddenHighlight
                    // The primary action is COLLAPSE, not "open this space".
                    // A category header that navigated on click would make
                    // every attempt to tidy the column also change rooms.
                    onClicked: app.spaceChannels.toggleCategory(rowLoader.model.roomId)
                    // Opening the subspace as a Space is the secondary
                    // action, on its own affordance.
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: root.spaceActivated(rowLoader.model.roomId)
                    }
                }
            }
        }
    }
}
