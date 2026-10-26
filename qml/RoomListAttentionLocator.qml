import QtQuick
import QtQuick.Controls
import MatrixClient

// Q3: unread rooms scrolled out of sight above or below a room list. A pill
// on that edge scrolls the LIST to the nearest one -- it opens nothing, so the
// open conversation, its draft and its read state are untouched
// (next-unread-room navigation is the separate command that opens one).
// Recomputed on a short debounce after scrolling or model changes, never per
// frame; the model answers with one bounded pass (attentionRowFrom, on both
// RoomListModel and SpaceChannelModel). Placed over the list it serves, it
// takes no input outside its two pills.
Item {
    id: root

    // The ListView whose rows the pills point at.
    property ListView list: null
    // Its model: anything with attentionRowFrom(fromRow, step, mentionsOnly).
    property var attentionModel: null

    property int attentionAboveRow: -1
    property int attentionBelowRow: -1
    property bool attentionAboveIsMention: false
    property bool attentionBelowIsMention: false

    function refreshAttentionLocator() {
        if (!list || !attentionModel || list.count <= 0 || list.height <= 0) {
            attentionAboveRow = -1
            attentionBelowRow = -1
            return
        }
        var first = list.indexAt(list.width / 2, list.contentY + 1)
        var last = list.indexAt(list.width / 2, list.contentY + list.height - 2)
        if (first < 0)
            first = 0
        if (last < 0)
            last = list.count - 1
        var aboveMention = first > 0
            ? attentionModel.attentionRowFrom(first - 1, -1, true) : -1
        var above = aboveMention >= 0 ? aboveMention
            : (first > 0 ? attentionModel.attentionRowFrom(first - 1, -1, false)
                         : -1)
        var belowMention = attentionModel.attentionRowFrom(last + 1, 1, true)
        var below = belowMention >= 0 ? belowMention
            : attentionModel.attentionRowFrom(last + 1, 1, false)
        attentionAboveRow = above
        attentionAboveIsMention = aboveMention >= 0
        attentionBelowRow = below
        attentionBelowIsMention = belowMention >= 0
    }
    Timer {
        id: attentionDebounce
        interval: 120
        onTriggered: root.refreshAttentionLocator()
    }
    Connections {
        target: root.list
        function onContentYChanged() { attentionDebounce.restart() }
        function onHeightChanged() { attentionDebounce.restart() }
        function onCountChanged() { attentionDebounce.restart() }
    }
    Connections {
        target: root.attentionModel
        ignoreUnknownSignals: true
        function onDataChanged() { attentionDebounce.restart() }
        function onModelReset() { attentionDebounce.restart() }
        function onLayoutChanged() { attentionDebounce.restart() }
        function onRowsInserted() { attentionDebounce.restart() }
        function onRowsRemoved() { attentionDebounce.restart() }
    }
    Component.onCompleted: attentionDebounce.restart()

    component AttentionPill: AbstractButton {
        id: pill
        property int targetRow: -1
        property bool mention: false
        property bool below: false
        // Never over an empty list: the row it was computed for is gone the
        // moment the list empties (seen live during an account switch, an
        // "Unread below" pill over "Sign in to see rooms"), and the refresh
        // is debounced.
        visible: targetRow >= 0 && root.list !== null && root.list.count > 0
        z: 30
        anchors.horizontalCenter: parent.horizontalCenter
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        implicitHeight: 26
        implicitWidth: pillRow.implicitWidth + 20
        text: below ? (mention ? qsTr("Mention below") : qsTr("Unread below"))
                    : (mention ? qsTr("Mention above") : qsTr("Unread above"))
        Accessible.role: Accessible.Button
        Accessible.name: text
        Accessible.description: qsTr("Scrolls the conversation list; "
                                     + "opens nothing")
        onClicked: if (root.list)
                       root.list.positionViewAtIndex(targetRow, ListView.Center)
        background: Rectangle {
            radius: height / 2
            color: pill.mention ? AppTheme.mentionBadge
                 : pill.down ? AppTheme.accentPressed
                 : pill.hovered ? AppTheme.accentHover : AppTheme.accent
        }
        contentItem: Row {
            id: pillRow
            spacing: AppTheme.spacingXS
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                name: pill.below ? "expand_more" : "expand_less"
                size: 16
                color: pill.mention ? AppTheme.mentionBadgeText
                                    : AppTheme.accentText
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: pill.text
                color: pill.mention ? AppTheme.mentionBadgeText
                                    : AppTheme.accentText
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                font.weight: AppTheme.weightBold
            }
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: (parent.height + 6) / 2
            color: "transparent"
            border.color: AppTheme.focusRing
            border.width: 2
            visible: pill.visualFocus
        }
    }
    AttentionPill {
        objectName: "roomListAttentionAbove"
        anchors.top: parent.top
        anchors.topMargin: AppTheme.spacing6
        targetRow: root.attentionAboveRow
        mention: root.attentionAboveIsMention
    }
    AttentionPill {
        objectName: "roomListAttentionBelow"
        anchors.bottom: parent.bottom
        anchors.bottomMargin: AppTheme.spacing6
        below: true
        targetRow: root.attentionBelowRow
        mention: root.attentionBelowIsMention
    }
}
