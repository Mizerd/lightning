import QtQuick
import QtQuick.Controls
import MatrixClient

// A selectable preview of one room-list navigation layout. A diagram drawn from
// AppTheme tokens, not a live instance (which would need a room list and
// avatars). Classic: tall rows with a preview line; Channels: a category header
// with short indented rows.
AbstractButton {
    id: root

    property string title: ""
    property string subtitle: ""
    /// "classic" | "channels"
    property string variant: "classic"
    property bool current: false
    /// A one-line tile: a small diagram beside the title and subtitle, for a
    /// settings row (Settings > Layout). The full card stacks them.
    property bool compact: false

    implicitWidth: 200
    // Grows with its text; 152 (56 compact) is a floor so both cards match
    // when the text fits.
    implicitHeight: Math.max(root.compact ? 56 : 152, cardBody.implicitHeight)
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    Accessible.role: Accessible.RadioButton
    Accessible.name: root.title
    Accessible.description: root.subtitle
    Accessible.checked: root.current

    background: Rectangle {
        radius: AppTheme.radiusMd
        // stormSelection / hover are the routed pair; a non-existent token name
        // yields an undefined colour silently.
        color: root.current ? AppTheme.stormSelection : (root.hovered || root.activeFocus ? AppTheme.hover : AppTheme.stormPanel)
        border.width: root.current || root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? AppTheme.focusRing : (root.current ? AppTheme.accentBorder : AppTheme.stormBorder)
        Behavior on color {
            ColorAnimation {
                duration: 90
            }
        }
    }

    contentItem: Grid {
        id: cardBody
        columns: root.compact ? 2 : 1
        spacing: AppTheme.spacing8
        padding: root.compact ? AppTheme.spacing8 : AppTheme.spacing12
        verticalItemAlignment: Grid.AlignVCenter

        // The diagram
        Rectangle {
            width: root.compact ? 72 : root.width - AppTheme.spacing12 * 2
            height: root.compact ? 40 : 76
            radius: AppTheme.radiusSm
            color: AppTheme.sidebar
            clip: true

            // Classic: four tall rows, each a name bar over a preview bar, with
            // an avatar disc.
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

            // Channels: a category header, short indented rows, and a second
            // category.
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
                        // The category bar: short and dim, like the real
                        // header.
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

        Column {
            id: cardTexts
            spacing: AppTheme.spacing4
            width: root.compact
                   ? root.width - 72 - cardBody.spacing - cardBody.padding * 2
                   : root.width - AppTheme.spacing12 * 2
            // Behind a Loader: set by the host and empty at creation.
            Loader {
                active: root.title.length > 0
                sourceComponent: Label {
                    // Untrusted text: never markup.
                    textFormat: Text.PlainText
                    text: root.title
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightMedium
                }
            }
            Loader {
                active: root.subtitle.length > 0
                sourceComponent: Label {
                    // Untrusted text: never markup.
                    textFormat: Text.PlainText
                    width: cardTexts.width
                    text: root.subtitle
                    wrapMode: Text.WordWrap
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textMeta
                }
            }
        }
    }
}
