import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

ColumnLayout {
    id: root
    objectName: "searchIndexEmptyState"
    property bool storm: false
    spacing: AppTheme.spacing8

    readonly property var search: app.messageSearch

    function ago(ms) {
        var minutes = Math.max(0, Math.floor((Date.now() - ms) / 60000))
        if (minutes < 1)
            return qsTr("just now")
        if (minutes < 60)
            return qsTr("%n minute(s) ago", "", minutes)
        var hours = Math.floor(minutes / 60)
        if (hours < 24)
            return qsTr("%n hour(s) ago", "", hours)
        return qsTr("%n day(s) ago", "", Math.floor(hours / 24))
    }

    Label {
        objectName: "searchIndexEmptyTitle"
        Layout.fillWidth: true
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        text: qsTr("No messages found.")
        color: root.storm ? AppTheme.stormTextMuted : AppTheme.textMuted
        font.pixelSize: AppTheme.textBody
    }
    Label {
        objectName: "searchIndexLastUpdated"
        Layout.fillWidth: true
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: {
            var s = root.search
            if (s.indexAllActive)
                return (s.indexAllTotal > 0
                    ? qsTr("Indexing older messages… %1 of %2 rooms")
                        .arg(s.indexAllPosition).arg(s.indexAllTotal)
                    : qsTr("Indexing older messages…"))
                    + "\n" + qsTr("Results may be incomplete.")
            if (s.sweeping || s.indexing)
                return qsTr("Indexing new messages…") + "\n"
                    + qsTr("Results may be incomplete.")
            if (s.indexAllFinishedMs > 0)
                return qsTr("Older messages last indexed %1")
                    .arg(root.ago(s.indexAllFinishedMs))
            return qsTr("Older messages are not indexed yet.")
        }
        color: root.storm ? AppTheme.stormTextMuted : AppTheme.textMuted
        font.pixelSize: AppTheme.textMeta
    }
    AppProgressBar {
        objectName: "searchIndexProgress"
        visible: root.search.indexAllActive && root.search.indexAllTotal > 0
        Layout.fillWidth: true
        Layout.maximumWidth: 240
        Layout.alignment: Qt.AlignHCenter
        value: root.search.indexAllTotal > 0
               ? root.search.indexAllPosition / root.search.indexAllTotal : 0
    }
    AppButton {
        objectName: "searchIndexOlderButton"
        visible: !root.search.indexAllActive
        Layout.alignment: Qt.AlignHCenter
        storm: root.storm
        kind: "ghost"
        size: "sm"
        text: root.search.indexAllResumable ? qsTr("Resume indexing")
                                            : qsTr("Index older messages")
        onClicked: root.search.indexAllRooms()
    }
}
