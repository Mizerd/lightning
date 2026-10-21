import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings → Labs: "Listening to" music activity, Discord's "Display Spotify
// as your status". Experimental and OFF by default. While Spotify plays on
// this computer, the song's title and artist become the presence status line;
// a game, or a status the user typed, takes the line first. Nothing is sent
// while presence sharing is off. Detection and publishing live in
// MusicActivity / PresenceManager; this is policy over app.settings and a
// local preview.
ColumnLayout {
    id: root
    objectName: "musicActivitySettings"
    spacing: AppTheme.spacing8

    readonly property bool sharing: !!app.settings && app.settings.sharePresence
    readonly property bool featureOn: !!app.settings && app.settings.musicActivityEnabled
    readonly property var activity: app.musicActivity ? app.musicActivity : null
    readonly property bool supported: !!activity && activity.supported
    readonly property bool listening: !!activity && activity.listening
    // The server answers presence at all (supported, not refused).
    readonly property bool presenceWorks: !!app.presence && app.presence.active

    CheckBox {
        objectName: "musicActivityCheck"
        palette.windowText: AppTheme.stormText
        text: qsTr("Show what I'm listening to on Spotify")
        checked: root.featureOn
        onToggled: app.settings.musicActivityEnabled = checked
        // Keyboard focus ring: the stock CheckBox draws none that is visible.
        Rectangle {
            objectName: "activityCheckFocusRing"
            anchors.fill: parent
            anchors.margins: -2
            radius: AppTheme.radiusSm
            color: "transparent"
            border.width: 2
            border.color: AppTheme.storm ? AppTheme.bolt : AppTheme.focusRing
            visible: parent.visualFocus
        }
    }
    Label {
        Layout.fillWidth: true
        Layout.leftMargin: AppTheme.spacing4
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        lineHeight: AppTheme.lineHeightBody
        lineHeightMode: Text.ProportionalHeight
        color: AppTheme.stormTextSecondary
        font.pixelSize: AppTheme.textMeta
        text: qsTr("An experiment. While Spotify is playing on this computer, "
                   + "your status reads “Listening to <song> by <artist>” for "
                   + "the people who can see your online status. Only the song "
                   + "and the artist are sent. A new song shows, and a paused "
                   + "one goes away, within about half a minute. Music played "
                   + "on your phone or another device is not seen. The last "
                   + "song can stay in your status until this account is next "
                   + "used in Lightning if Lightning crashes, is closed by force, "
                   + "is quit just after your status changed, or switches to "
                   + "another account at a busy moment.")
    }
    Label {
        objectName: "musicActivityPrivateSessionNote"
        Layout.fillWidth: true
        Layout.leftMargin: AppTheme.spacing4
        visible: root.featureOn
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        text: qsTr("Spotify's Private Session is not detected: songs are "
                   + "still shown. Turn this off to keep a song to yourself.")
    }
    Label {
        objectName: "musicActivityPriorityNote"
        Layout.fillWidth: true
        Layout.leftMargin: AppTheme.spacing4
        visible: root.featureOn
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        text: qsTr("A game you are playing, or a status you set yourself, is "
                   + "shown instead while there is one.")
    }
    Label {
        objectName: "musicActivitySharingNote"
        Layout.fillWidth: true
        Layout.leftMargin: AppTheme.spacing4
        visible: root.featureOn && !root.sharing
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        text: qsTr("Nothing is sent while “Share my online status” is off.")
    }
    Label {
        objectName: "musicActivityServerNote"
        Layout.fillWidth: true
        Layout.leftMargin: AppTheme.spacing4
        visible: root.featureOn && root.sharing && root.supported && !root.presenceWorks
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        text: qsTr("Your server does not share online status, so nothing is shown.")
    }
    Label {
        objectName: "musicActivityUnsupportedNote"
        Layout.fillWidth: true
        Layout.leftMargin: AppTheme.spacing4
        visible: !!root.activity && !root.supported
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        text: qsTr("This build cannot see what Spotify is playing, so nothing "
                   + "will be shown.")
    }

    // Local preview of the user's OWN activity: richer than what crosses
    // (the album never leaves this computer). Matrix carries one line of
    // text, so there is no album art or progress to show others.
    Rectangle {
        objectName: "musicActivityPreview"
        Layout.fillWidth: true
        Layout.leftMargin: AppTheme.spacing4
        visible: root.featureOn && root.sharing && root.supported && root.presenceWorks
        implicitHeight: previewColumn.implicitHeight + 2 * AppTheme.spacing8
        radius: AppTheme.radiusSm
        color: "transparent"
        border.width: 1
        border.color: AppTheme.stormBorder

        ColumnLayout {
            id: previewColumn
            anchors.fill: parent
            anchors.margins: AppTheme.spacing8
            spacing: AppTheme.spacing4

            Label {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textMeta
                font.weight: AppTheme.weightStrong
                text: root.listening ? qsTr("Listening to Spotify")
                                     : qsTr("Spotify is not playing right now.")
            }
            Label {
                objectName: "musicActivityPreviewTitle"
                Layout.fillWidth: true
                visible: root.listening
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightStrong
                text: root.listening ? root.activity.title : ""
            }
            Label {
                Layout.fillWidth: true
                visible: root.listening && root.activity.artist.length > 0
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textMeta
                text: root.listening ? qsTr("by %1").arg(root.activity.artist) : ""
            }
            Label {
                Layout.fillWidth: true
                visible: root.listening && root.activity.album.length > 0
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: root.listening ? qsTr("on %1").arg(root.activity.album) : ""
            }
            Label {
                objectName: "musicActivityPreviewLine"
                Layout.fillWidth: true
                Layout.topMargin: AppTheme.spacing4
                visible: root.listening
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: !root.listening ? ""
                      : root.activity.shownAsStatus
                        ? qsTr("Your status will show: %1").arg(root.activity.statusLine)
                        : qsTr("Not shown right now: your game or your own status "
                               + "takes the line.")
            }
        }
    }
}
