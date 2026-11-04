import QtQuick
import QtQuick.Controls
import MatrixClient

Item {
    id: root

    property ListView list: null
    property var attentionModel: null

    property real pinnedHeaderInset: 34
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
        readonly property bool expanded: hovered || visualFocus
        readonly property int barWidth: 64
        readonly property int chipWidth: 24
        readonly property int dot: 6
        readonly property color fill: mention ? AppTheme.mentionBadge
                                    : down ? AppTheme.accentPressed
                                    : hovered ? AppTheme.accentHover : AppTheme.accent
        readonly property color ink: mention ? AppTheme.mentionBadgeText
                                             : AppTheme.accentText
        anchors.horizontalCenter: parent.horizontalCenter
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        implicitWidth: expanded
            ? Math.max(barWidth + 4, label.implicitWidth + 16 + 14 + 4
                       + (mention ? dot + 4 : 0))
            : barWidth + 4 + (mention ? dot + 2 : 0)
        implicitHeight: expanded ? 22 : (mention ? 16 : 14)
        text: below ? (mention ? qsTr("Mention below") : qsTr("Unread below"))
                    : (mention ? qsTr("Mention above") : qsTr("Unread above"))
        Accessible.role: Accessible.Button
        Accessible.name: text
        Accessible.description: qsTr("Scrolls the conversation list; "
                                     + "opens nothing")
        onClicked: if (root.list)
                       root.list.positionViewAtIndex(targetRow, ListView.Center)
        background: Item {
            Rectangle {
                id: bar
                objectName: "attentionBar"
                visible: !pill.expanded
                x: 2
                anchors.verticalCenter: parent.verticalCenter
                width: pill.barWidth
                height: pill.mention ? 4 : 3
                radius: height / 2
                color: pill.fill
            }
            Rectangle {
                objectName: "attentionChip"
                visible: !pill.expanded
                x: bar.x + (bar.width - width) / 2
                anchors.verticalCenter: parent.verticalCenter
                width: pill.chipWidth
                height: pill.height
                radius: height / 2
                color: pill.fill
                Icon {
                    anchors.centerIn: parent
                    name: pill.below ? "expand_more" : "expand_less"
                    size: 12
                    color: pill.ink
                }
            }
            Rectangle {
                visible: pill.mention && !pill.expanded
                x: bar.x + bar.width + 2
                anchors.verticalCenter: parent.verticalCenter
                width: pill.dot
                height: width
                radius: width / 2
                color: AppTheme.mentionBadge
            }
            Rectangle {
                objectName: "attentionPillFill"
                visible: pill.expanded
                anchors.fill: parent
                radius: height / 2
                color: pill.fill
            }
        }
        contentItem: Item {
            Row {
                visible: pill.expanded
                anchors.centerIn: parent
                spacing: 4
                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: pill.below ? "expand_more" : "expand_less"
                    size: 14
                    color: pill.ink
                }
                Label {
                    id: label
                    objectName: "attentionLabel"
                    anchors.verticalCenter: parent.verticalCenter
                    text: pill.text
                    color: pill.ink
                    font.pixelSize: AppTheme.textMeta
                    font.weight: AppTheme.weightStrong
                }
                Rectangle {
                    visible: pill.mention
                    anchors.verticalCenter: parent.verticalCenter
                    width: pill.dot
                    height: width
                    radius: width / 2
                    color: pill.ink
                }
            }
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            radius: height / 2
            color: "transparent"
            border.color: AppTheme.focusRing
            border.width: 2
            visible: pill.visualFocus
        }
    }
    AttentionPill {
        objectName: "roomListAttentionAbove"
        anchors.top: parent.top
        anchors.topMargin: root.pinnedHeaderInset
        targetRow: root.attentionAboveRow
        mention: root.attentionAboveIsMention
    }
    AttentionPill {
        objectName: "roomListAttentionBelow"
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 0
        below: true
        targetRow: root.attentionBelowRow
        mention: root.attentionBelowIsMention
    }
}
