import QtQuick
import QtQuick.Controls
import MatrixClient

// A selectable preview of one room-list navigation layout.
//
// A card rather than another segmented control, because the choice is about
// SHAPE and a two-word label cannot convey shape. The preview is drawn from
// AppTheme tokens — it is a diagram of the layout, deliberately not a live
// instance of it: a real RoomListClassicPresenter in a settings card would
// need a room list, fetch avatars, and change while you look at it.
//
// The two diagrams differ in exactly the ways the layouts differ, which is
// the whole job: Classic shows tall rows with a preview line under each name;
// Channels shows a category header with short single-line rows indented
// beneath it.
AbstractButton {
    id: root

    property string title: ""
    property string subtitle: ""
    /// "classic" | "channels"
    property string variant: "classic"
    property bool current: false

    implicitWidth: 200
    implicitHeight: 152
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    Accessible.role: Accessible.RadioButton
    Accessible.name: root.title
    Accessible.description: root.subtitle
    Accessible.checked: root.current

    background: Rectangle {
        radius: AppTheme.radiusMd
        // stormSelection / hover are the theme-ROUTED pair. There is no
        // `stormSelected` or `stormHover` token, and naming one silently
        // yields an undefined colour rather than an error — caught only by
        // the no-QML-warnings gate.
        color: root.current ? AppTheme.stormSelection : (root.hovered || root.activeFocus ? AppTheme.hover : AppTheme.stormPanel)
        border.width: root.current || root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? AppTheme.focusRing : (root.current ? AppTheme.accentBorder : AppTheme.stormBorder)
        Behavior on color {
            ColorAnimation {
                duration: 90
            }
        }
    }

    contentItem: Column {
        spacing: AppTheme.spacing8
        padding: AppTheme.spacing12

        // ── The diagram ──────────────────────────────────────────────────
        Rectangle {
            width: root.width - AppTheme.spacing12 * 2
            height: 76
            radius: AppTheme.radiusSm
            color: AppTheme.sidebar
            clip: true

            // Classic: four tall rows, each a name bar over a dimmer
            // preview bar, with a leading avatar disc.
            Column {
                visible: root.variant === "classic"
                anchors.fill: parent
                anchors.margins: 6
                spacing: 3
                Repeater {
                    model: 4
                    delegate: Row {
                        required property int index
                        spacing: 5
                        Rectangle {
                            width: 12
                            height: 12
                            radius: 6
                            color: AppTheme.channelCategoryText
                            opacity: 0.45
                        }
                        Column {
                            spacing: 2
                            Rectangle {
                                width: 74 - index * 6
                                height: 4
                                radius: 2
                                color: AppTheme.text
                                opacity: index === 0 ? 0.85 : 0.55
                            }
                            Rectangle {
                                width: 92 - index * 9
                                height: 3
                                radius: 1.5
                                color: AppTheme.channelCategoryText
                                opacity: 0.5
                            }
                        }
                    }
                }
            }

            // Channels: a category header, then short indented rows — and a
            // second category, so the STRUCTURE is what the card shows.
            Column {
                visible: root.variant === "channels"
                anchors.fill: parent
                anchors.margins: 6
                spacing: 4

                Repeater {
                    model: 2
                    delegate: Column {
                        required property int index
                        spacing: 3
                        // The all-caps category bar: short and dim, the way
                        // the real header is the quietest thing in the list.
                        Rectangle {
                            width: 40
                            height: 3
                            radius: 1.5
                            color: AppTheme.channelCategoryText
                            opacity: 0.8
                        }
                        Repeater {
                            model: index === 0 ? 3 : 2
                            delegate: Row {
                                required property int index
                                spacing: 4
                                // Indent, matching the real row's depth.
                                Item {
                                    width: 8
                                    height: 1
                                }
                                Rectangle {
                                    width: 4
                                    height: 4
                                    radius: 1
                                    color: AppTheme.channelCategoryText
                                    opacity: 0.7
                                }
                                Rectangle {
                                    width: 66 - index * 10
                                    height: 4
                                    radius: 2
                                    color: index === 0 ? AppTheme.text : AppTheme.channelText
                                    opacity: index === 0 ? 0.9 : 0.6
                                }
                            }
                        }
                    }
                }
            }
        }

        // Behind a Loader: these are set by the host and are empty in the
        // state this card is created in.
        Loader {
            active: root.title.length > 0
            sourceComponent: Label {
                text: root.title
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightMedium
            }
        }
        Loader {
            active: root.subtitle.length > 0
            sourceComponent: Label {
                width: root.width - AppTheme.spacing12 * 2
                text: root.subtitle
                wrapMode: Text.WordWrap
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
            }
        }
    }
}
