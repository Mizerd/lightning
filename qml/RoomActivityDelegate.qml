import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Compact room-state annotation shared by the real timeline delegate and
// QML-facing tests. Entries are typed maps produced by TimelineModel; this
// component never reconstructs events from the summary label.
Item {
    id: root
    property string groupId: ""
    property var entries: []
    property bool expanded: false
    signal toggleRequested()

    readonly property int entryCount: entries ? entries.length : 0
    readonly property bool canExpand: entryCount > 0

    // A collapsed group draws ONE summary line, and the date dividers that
    // used to sit around it are now suppressed when everything they
    // introduce is hidden — so a group spanning several days would otherwise
    // lose its date entirely. Same-day groups keep the plain count: a date on
    // every group is noise, not information.
    //
    // Entries carry their own timestamp (TimelineModel::stateGroupEntriesFrom)
    // and arrive in timeline order, so the range is the first and the last —
    // never a scan, and never a guess when a timestamp is missing.
    function sameCalendarDay(a, b) {
        return a.getFullYear() === b.getFullYear()
               && a.getMonth() === b.getMonth()
               && a.getDate() === b.getDate()
    }
    // Duck-typed rather than `instanceof Date`: the entries come from a
    // QVariantMap, and a value that arrives wrapped instead of converted
    // would silently fail an identity check and drop the range for every
    // group.
    function validDate(value) {
        if (!value || typeof value.getTime !== "function")
            return false
        return !isNaN(value.getTime())
    }
    readonly property string dateRangeLabel: {
        if (root.entryCount < 2)
            return ""
        var first = root.entries[0].timestamp
        var last = root.entries[root.entryCount - 1].timestamp
        if (!root.validDate(first) || !root.validDate(last))
            return ""
        if (root.sameCalendarDay(first, last))
            return ""
        var locale = Qt.locale()
        return qsTr("%1 – %2").arg(locale.toString(first, "d MMM"))
                              .arg(locale.toString(last, "d MMM"))
    }
    readonly property int renderedEntryCount: activityRepeater.count
    readonly property real expandedContentHeight: expandedColumn.height
    implicitHeight: visible ? activityColumn.implicitHeight : 0

    Column {
        id: activityColumn
        width: parent.width
        height: implicitHeight
        spacing: 1

        Control {
            id: summaryRow
            objectName: "stateActivitySummary"
            width: parent.width
            height: implicitHeight
            implicitHeight: summaryContent.implicitHeight + AppTheme.spacingXS * 2
            enabled: root.canExpand
            hoverEnabled: true
            focusPolicy: root.canExpand ? Qt.StrongFocus : Qt.NoFocus
            Accessible.role: Accessible.Button
            Accessible.name: summaryLabel.text
            Accessible.description: root.expanded ? qsTr("Collapse room updates")
                                                  : qsTr("Expand room updates")

            background: Rectangle {
                radius: AppTheme.radiusSm
                color: summaryRow.hovered || summaryRow.activeFocus
                       ? AppTheme.hover : "transparent"
            }
            contentItem: RowLayout {
                id: summaryContent
                spacing: AppTheme.spacingXS
                // Was a geometric-shapes Unicode triangle at 10px — the
                // only chevron in the app not drawn from the icon font, so
                // it sat at a different weight and baseline from every
                // other disclosure control.
                Icon {
                    objectName: "stateActivityChevron"
                    visible: root.canExpand
                    name: root.expanded ? "expand_more" : "chevron_right"
                    size: AppTheme.scaled(AppTheme.textSubtitle)
                    color: AppTheme.textMuted
                }
                Label {
                    id: summaryLabel
                    Layout.fillWidth: true
                    // Same reason as the entry label below: the summary can
                    // carry a member-chosen display name, and AutoText would
                    // let it become markup.
                    textFormat: Text.PlainText
                    text: {
                        if (root.entryCount === 0)
                            return qsTr("Room updated")
                        if (root.entryCount === 1)
                            return qsTr("1 room update")
                        if (root.dateRangeLabel.length > 0)
                            return qsTr("%1 room updates · %2")
                                .arg(root.entryCount)
                                .arg(root.dateRangeLabel)
                        return qsTr("%1 room updates").arg(root.entryCount)
                    }
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    elide: Label.ElideRight
                }
            }

            TapHandler {
                enabled: root.canExpand
                onTapped: {
                    summaryRow.forceActiveFocus(Qt.MouseFocusReason)
                    root.toggleRequested()
                }
            }
            Keys.onPressed: (event) => {
                if (root.canExpand
                    && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                        || event.key === Qt.Key_Space)) {
                    root.toggleRequested()
                    event.accepted = true
                }
            }
        }

        Column {
            id: expandedColumn
            objectName: "stateActivityExpandedContent"
            x: AppTheme.spacingM
            width: Math.max(0, parent.width - x)
            height: visible ? implicitHeight : 0
            spacing: 1
            visible: root.expanded && root.canExpand

            Repeater {
                id: activityRepeater
                objectName: "stateActivityRepeater"
                model: expandedColumn.visible ? root.entries : []
                Label {
                    required property var modelData
                    objectName: "stateActivityEntry"
                    width: expandedColumn.width
                    height: Math.max(16, implicitHeight)
                    text: modelData.description || ""
                    // MANDATORY, and it is a security control, not styling.
                    // This sentence embeds THREE strings a remote member
                    // chose: the actor's resolved display name (at offset 0)
                    // and the old and new display names. Qt's AutoText
                    // default runs mightBeRichText() over the result, so a
                    // display name beginning with markup promotes the whole
                    // row to StyledText — and a name carrying
                    // <img src="https://…"> would then make every viewer's
                    // client fetch that URL. That is an unconsented remote
                    // beacon (IP, timing) fired inside a room where link
                    // previews are deliberately off by default. Plain text
                    // renders the characters and fetches nothing.
                    textFormat: Text.PlainText
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    wrapMode: Text.WordWrap
                    Accessible.name: text
                }
            }
        }
    }
}
