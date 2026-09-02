import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.7.x global message search: one modal card over the server's /search
// endpoint (MessageSearchController with an empty roomId scope).
//
// HONESTY: the homeserver cannot search ciphertext, so results cover
// unencrypted rooms only — disclosed inline, always visible. Result rows
// label their room; activating one opens the room and jumps to the event
// through the existing navigation path.
Dialog {
    id: root
    objectName: "messageSearchDialog"
    modal: true
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: Math.min(560, parent ? parent.width - AppTheme.spacing24 * 2 : 560)
    height: Math.min(620,
                     parent ? parent.height - AppTheme.spacing24 * 2 : 620)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    function openDialog() {
        app.messageSearch.roomId = ""
        app.messageSearch.filters = ({})
        open()
        Qt.callLater(function() { globalSearchField.forceActiveFocus() })
    }

    onClosed: {
        app.messageSearch.query = ""
        app.messageSearch.clear()
    }

    function activateResult(row) {
        var r = app.messageSearch.rowAt(row)
        if (!r.roomId || !r.eventId)
            return
        // Same shape as notification click routing: switch room first, let
        // it settle one event-loop turn, then jump on the shared path.
        app.openRoom(r.roomId)
        var eventId = r.eventId
        Qt.callLater(function() { app.pagination.jumpToEvent(eventId) })
        root.close()
    }

    background: Rectangle {
        radius: AppTheme.radiusLg
        color: AppTheme.stormPanel
        border.color: AppTheme.stormBorder
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Label {
                text: qsTr("Search messages")
                color: AppTheme.stormText
                font.family: AppTheme.menuFont
                font.pixelSize: AppTheme.textTitle
                font.weight: AppTheme.weightBold
                Layout.fillWidth: true
            }
            IconButton {
                storm: true
                iconName: "close"
                iconSize: 18
                implicitWidth: 28; implicitHeight: 28
                Accessible.name: qsTr("Close")
                onClicked: root.close()
            }
        }

        AppTextField {
            id: globalSearchField
            objectName: "globalMessageSearchField"
            Layout.fillWidth: true
            storm: true
            searchIcon: true
            clearButton: true
            placeholderText: qsTr("Search your message history…")
            text: app.messageSearch.query
            onTextChanged: app.messageSearch.query = text
            onAccepted: app.messageSearch.search()
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Server-side search — messages in end-to-end "
                       + "encrypted rooms are not included.")
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.Wrap
        }

        ListView {
            id: resultsList
            objectName: "globalSearchResultsList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: AppTheme.spacing4
            model: app.messageSearch
            ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
            keyNavigationEnabled: true
            onAtYEndChanged: {
                if (atYEnd && app.messageSearch.canLoadMore)
                    app.messageSearch.loadMore()
            }

            delegate: Rectangle {
                id: resultRow
                required property int index
                required property string roomId
                required property string roomName
                required property string eventId
                required property string sender
                required property string senderDisplayName
                required property string senderAvatarUrl
                required property var timestampMs
                required property string body

                width: resultsList.width
                height: resultCol.implicitHeight + AppTheme.spacing8 * 2
                radius: AppTheme.radiusMd
                color: resultHover.hovered ? AppTheme.stormSelection
                                           : "transparent"
                HoverHandler { id: resultHover }
                TapHandler {
                    onTapped: root.activateResult(resultRow.index)
                }
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Open result in %1").arg(roomName)

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacing8
                    spacing: AppTheme.spacing8
                    Avatar {
                        mxc: resultRow.senderAvatarUrl
                        name: resultRow.senderDisplayName.length > 0
                              ? resultRow.senderDisplayName : resultRow.sender
                        size: 32
                        Layout.alignment: Qt.AlignTop
                    }
                    ColumnLayout {
                        id: resultCol
                        Layout.fillWidth: true
                        spacing: 2
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing6
                            Label {
                                // Remote or externally chosen text: never markup.
                                textFormat: Text.PlainText
                                text: resultRow.senderDisplayName.length > 0
                                      ? resultRow.senderDisplayName
                                      : resultRow.sender
                                // The identity ink, as in the timeline: a
                                // result row named the sender in the same
                                // grey as everything else, so the app's one
                                // real source of colour stopped at the
                                // timeline's edge.
                                color: AppTheme.userColor(resultRow.sender)
                                font.family: AppTheme.menuFont
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                                font.weight: AppTheme.weightStrong
                                elide: Label.ElideRight
                                Layout.maximumWidth: 180
                            }
                            Label {
                                text: qsTr("in %1").arg(resultRow.roomName)
                                textFormat: Text.PlainText
                                color: AppTheme.stormTextMuted
                                font.family: AppTheme.menuFont
                                // Was an unscaled 11 beside a scaled sender
                                // name: at 140% the row sheared, one label
                                // pinned while its neighbour grew.
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                                elide: Label.ElideRight
                                Layout.fillWidth: true
                            }
                            Label {
                                text: {
                                    var d = new Date(Number(
                                        resultRow.timestampMs))
                                    return d.toLocaleDateString(
                                        Qt.locale(), Locale.ShortFormat)
                                }
                                color: AppTheme.stormTextMuted
                                font.family: AppTheme.menuFont
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                            }
                        }
                        Label {
                            // Remote or externally chosen text: never markup.
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: resultRow.body
                            color: AppTheme.stormTextSecondary
                            font.family: AppTheme.menuFont
                            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Label.ElideRight
                        }
                    }
                }
            }

            Item {
                anchors.fill: parent
                visible: resultsList.count === 0
                AppBusyIndicator {
                    anchors.centerIn: parent
                    running: app.messageSearch.state === "loading"
                    visible: running
                    color: AppTheme.bolt
                }
                Label {
                    anchors.centerIn: parent
                    visible: app.messageSearch.state === "no_results"
                    text: qsTr("No messages found")
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textBody
                }
                ColumnLayout {
                    anchors.centerIn: parent
                    spacing: AppTheme.spacing8
                    visible: app.messageSearch.state === "error"
                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: qsTr("The search could not be completed.")
                        color: AppTheme.stormTextMuted
                        font.pixelSize: AppTheme.textBody
                    }
                    AppButton {
                        Layout.alignment: Qt.AlignHCenter
                        storm: true
                        text: qsTr("Retry")
                        onClicked: app.messageSearch.search()
                    }
                }
                Label {
                    anchors.centerIn: parent
                    visible: app.messageSearch.state === "idle"
                    text: qsTr("Type to search across your rooms")
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textBody
                }
            }
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            visible: app.messageSearch.state === "loading_more"
            text: qsTr("Loading more…")
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
        }
    }
}
