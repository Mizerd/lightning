import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root

    property string kindLabel: ""
    property string detailText: ""
    property string iconName: "attach_file"
    property bool expanded: false
    property real maximumWidth: 0
    property bool interactive: true

    signal toggleRequested()

    objectName: "collapsedEmbedRow"

    readonly property string summaryText:
        root.detailText.length > 0
            ? root.kindLabel + " · " + root.detailText
            : root.kindLabel

    readonly property real naturalWidth:
        contentRow.implicitWidth + AppTheme.spacing8 * 2
    implicitWidth: root.maximumWidth > 0
                   ? Math.max(1, Math.min(root.maximumWidth, naturalWidth))
                   : Math.max(1, naturalWidth)
    implicitHeight: contentRow.implicitHeight + AppTheme.spacing4 * 2

    Rectangle {
        anchors.fill: parent
        radius: AppTheme.radiusSm
        color: rowHover.hovered && root.interactive
               ? AppTheme.embedSurface : "transparent"
        border.width: 1
        border.color: rowHover.hovered && root.interactive
                      ? AppTheme.border : "transparent"
    }

    RowLayout {
        id: contentRow
        anchors.fill: parent
        anchors.leftMargin: AppTheme.spacing8
        anchors.rightMargin: AppTheme.spacing8
        spacing: AppTheme.spacing6

        Item {
            objectName: "collapsedEmbedChevronPlate"
            Layout.preferredWidth: 16
            Layout.preferredHeight: 16
            Layout.alignment: Qt.AlignVCenter
            Icon {
                objectName: "collapsedEmbedChevron"
                anchors.centerIn: parent
                name: root.expanded ? "expand_more" : "chevron_right"
                size: 16
                color: rowHover.hovered && root.interactive
                       ? AppTheme.text : AppTheme.textMuted
            }
        }

        Icon {
            objectName: "collapsedEmbedKindIcon"
            Layout.alignment: Qt.AlignVCenter
            name: root.iconName
            size: 14
            color: AppTheme.textMuted
        }

        Label {
            objectName: "collapsedEmbedLabel"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.alignment: Qt.AlignVCenter
            textFormat: Text.PlainText
            text: root.summaryText
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
            elide: Label.ElideMiddle
            maximumLineCount: 1
        }
    }

    HoverHandler {
        id: rowHover
        enabled: root.interactive
        cursorShape: Qt.PointingHandCursor
    }
    TapHandler {
        objectName: "collapsedEmbedTap"
        enabled: root.interactive
        // WithinBounds: the default DragThreshold only grabs passively, so the
        // ancestor row's tap would also fire (toggling a selection or opening
        // the message).
        gesturePolicy: TapHandler.WithinBounds
        onTapped: root.toggleRequested()
    }

    activeFocusOnTab: root.interactive
    Accessible.role: Accessible.Button
    Accessible.name: root.expanded
        ? qsTr("Hide %1").arg(root.summaryText)
        : qsTr("Show %1").arg(root.summaryText)
    Accessible.description: root.expanded
        ? qsTr("This attachment is expanded. Activate to collapse it back to one line.")
        : qsTr("This attachment is collapsed to one line. Activate to show it.")
    Accessible.onPressAction: {
        if (root.interactive)
            root.toggleRequested()
    }
    Keys.onPressed: event => {
        if (!root.interactive)
            return
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
            || event.key === Qt.Key_Enter) {
            root.toggleRequested()
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Right && !root.expanded) {
            root.toggleRequested()
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Left && root.expanded) {
            root.toggleRequested()
            event.accepted = true
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: root.activeFocus
        radius: AppTheme.radiusSm
        color: "transparent"
        border.width: 2
        border.color: AppTheme.focusRing
    }
}
