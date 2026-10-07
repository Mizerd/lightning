import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// "Chat background…" from the places people look for it: the room header's
// More menu, a room's right-click menu and Home. It hosts the same
// ChatBackgroundEditor that Room information, Space settings and Settings ->
// Appearance host, so nothing here writes on its own: the editor's Apply does.
//
//   openForRoom(id, name)  the room's background (shared or only me);
//   openForDefault()       this account's own picture behind every room.
//
// The editor is rebuilt on every open (Loader on `visible`), so a picture
// picked and abandoned last time is not still pending the next time.
AppDialog {
    id: root
    objectName: "chatBackgroundDialog"
    // A themed surface: the editor uses the theme's inks, not Storm's.
    storm: false
    parent: Overlay.overlay
    width: Math.min(560, parent ? parent.width - AppTheme.spacing24 * 2 : 560)
    standardButtons: Dialog.Close

    /// "room" or "default" (ChatBackgroundEditor's scopeKind).
    property string scopeKind: "room"
    property string roomId: ""
    property string roomName: ""

    readonly property bool available: typeof app !== "undefined" && app
                                      && !!app.backdrops

    function openForRoom(id, name) {
        root.scopeKind = "room"
        root.roomId = id || ""
        root.roomName = name || ""
        open()
    }
    function openForDefault() {
        root.scopeKind = "default"
        root.roomId = ""
        root.roomName = ""
        open()
    }

    title: root.scopeKind === "default" || root.roomName.length === 0
           ? qsTr("Chat background")
           // The room's name is remote text; AppDialog's title is plain text.
           : qsTr("Chat background for %1").arg(root.roomName)

    contentItem: Flickable {
        id: flick
        objectName: "chatBackgroundDialogScroll"
        clip: true
        contentWidth: width
        contentHeight: column.implicitHeight
        // Scrolls rather than growing past the window.
        implicitHeight: Math.min(column.implicitHeight,
                                 root.parent ? root.parent.height * 0.7 : 520)
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: column
            width: flick.width
            spacing: AppTheme.spacing12

            Label {
                Layout.fillWidth: true
                visible: !root.available
                wrapMode: Text.WordWrap
                text: qsTr("Chat backgrounds aren't available with this "
                           + "account.")
                color: AppTheme.textSecondary
                font.pixelSize: AppTheme.textBody
            }

            Loader {
                id: editorLoader
                objectName: "chatBackgroundDialogEditor"
                Layout.fillWidth: true
                active: root.visible && root.available
                        && (root.scopeKind === "default" || root.roomId !== "")
                sourceComponent: ChatBackgroundEditor {
                    scopeKind: root.scopeKind
                    scopeId: root.scopeKind === "default" ? "" : root.roomId
                    showTitle: false
                }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
                text: root.scopeKind === "default"
                      ? qsTr("Shown behind every conversation, only to you. A "
                             + "room can still have a background of its own.")
                      : qsTr("Pick a picture, adjust it, then Apply. Colours "
                             + "and gradients for the whole window are themes.")
            }

            // The two neighbouring places: one picture for every room, and
            // themes (where colours and gradients live).
            Flow {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                AppButton {
                    objectName: "chatBackgroundDialogEveryRoom"
                    visible: root.scopeKind === "room"
                    kind: "ghost"
                    size: "sm"
                    iconName: "image"
                    text: qsTr("One picture for every room…")
                    onClicked: root.openForDefault()
                }
                AppButton {
                    objectName: "chatBackgroundDialogThemes"
                    kind: "ghost"
                    size: "sm"
                    iconName: "palette"
                    text: qsTr("Themes, colours and gradients…")
                    onClicked: {
                        root.close()
                        app.showSettingsSection("appearance")
                    }
                }
            }
        }
    }
}
