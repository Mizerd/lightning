import QtQuick
import QtQuick.Layouts
import MatrixClient

ColumnLayout {
    id: root
    objectName: "userClientsInfo"

    property string userId: ""
    property string deviceId: ""

    readonly property bool devMode:
        (typeof app !== "undefined" && app) ? app.settings.developerTools : false
    readonly property var about: (typeof app !== "undefined" && app) ? app.about : null
    readonly property var info: {
        if (!about || userId.length === 0) return ({ state: "idle", devices: [] })
        const revision = about.userClientsRevision
        return about.userClients(userId)
    }
    readonly property var shown: {
        const all = info.devices || []
        if (deviceId.length === 0) return all
        return all.filter(function (d) { return d.deviceId === deviceId })
    }

    visible: devMode && userId.length > 0
    Layout.fillWidth: true
    spacing: AppTheme.spacing4

    function ask() {
        if (devMode && about && userId.length > 0)
            about.requestUserClients(userId)
    }
    onDevModeChanged: ask()
    onUserIdChanged: ask()
    onVisibleChanged: if (visible) ask()
    Component.onCompleted: ask()

    Text {
        text: root.deviceId.length > 0 ? qsTr("Session") : qsTr("Sessions")
        color: AppTheme.stormTextSecondary
        font.pixelSize: AppTheme.textMeta
        font.weight: AppTheme.weightStrong
    }
    Text {
        objectName: "userClientsState"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        visible: text !== ""
        text: {
            if (root.info.state === "loading") return qsTr("Looking up...")
            if (root.info.state === "failed") return qsTr("Unknown: sessions could not be read.")
            if (root.info.state === "ready" && root.shown.length === 0)
                return root.deviceId.length > 0
                    ? qsTr("Unknown: this device has no record of session %1.").arg(root.deviceId)
                    : qsTr("Unknown: this device has no session list for this user.")
            return ""
        }
    }
    Repeater {
        model: root.shown
        delegate: Text {
            required property var modelData
            Layout.fillWidth: true
            wrapMode: Text.WrapAnywhere
            textFormat: Text.PlainText
            color: AppTheme.stormText
            font.family: AppTheme.monoFont
            font.pixelSize: AppTheme.textMeta
            text: modelData.deviceId + "  "
                  + (modelData.name.length > 0 ? modelData.name : qsTr("(no name set)"))
        }
    }
    Text {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        visible: root.info.state === "ready" && root.shown.length > 0
        text: qsTr("Names are chosen by the account's owner and are not checked. "
                   + "Lightning does not guess which app a session is.")
    }
}
