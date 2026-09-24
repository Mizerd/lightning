import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Compact room-state annotation shared by the timeline delegate and tests.
// Entries are typed maps from TimelineModel; nothing is reconstructed from the
// summary label.
Item {
    id: root
    property string groupId: ""
    property var entries: []
    property bool expanded: false
    signal toggleRequested()

    // A call is not a room update. TimelineModel breaks groups at call rows and
    // CallEventDelegate draws them, so this normally removes nothing; it guards
    // fixtures and backends that phrase their own state rows.
    function entryIsCall(entry) {
        if (!entry)
            return false;
        var kind = entry.eventKind || "";
        return kind === "m.call" || kind === "m.call.video";
    }
    readonly property var entriesWithoutCalls: {
        if (!entries || entries.length === 0)
            return [];
        var kept = [];
        for (var i = 0; i < entries.length; ++i) {
            if (!root.entryIsCall(entries[i]))
                kept.push(entries[i]);
        }
        return kept;
    }
    readonly property int entryCount: entriesWithoutCalls.length
    readonly property bool canExpand: entryCount > 0

    // A collapsed group draws one summary line. The model breaks state runs at
    // date dividers, so a multi-day range only appears for backends without
    // dividers (mock/HTTP fixtures); same-day groups keep the plain count.
    // Entries carry timestamps in timeline order, so the range is first and
    // last.
    function sameCalendarDay(a, b) {
        return a.getFullYear() === b.getFullYear()
               && a.getMonth() === b.getMonth()
               && a.getDate() === b.getDate()
    }
    // Duck-typed rather than instanceof Date: a value from a QVariantMap may
    // arrive wrapped.
    function validDate(value) {
        if (!value || typeof value.getTime !== "function")
            return false
        return !isNaN(value.getTime())
    }
    readonly property string dateRangeLabel: {
        if (root.entryCount < 2)
            return ""
        var first = root.entriesWithoutCalls[0].timestamp
        var last = root.entriesWithoutCalls[root.entryCount - 1].timestamp
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
                // From the icon font, like every other disclosure control.
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
                    // The summary can carry a member-chosen display name: never
                    // markup.
                    textFormat: Text.PlainText
                    text: {
                        if (root.entryCount === 0)
                            return qsTr("Room updated")
                        var summary = qsTr("%n room update(s)",
                                           "collapsed state-event group",
                                           root.entryCount)
                        if (root.dateRangeLabel.length > 0)
                            return qsTr("%1 · %2")
                                .arg(summary)
                                .arg(root.dateRangeLabel)
                        return summary
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
                model: expandedColumn.visible ? root.entriesWithoutCalls : []
                RowLayout {
                    id: entryRow
                    required property var modelData
                    objectName: "stateActivityEntry"
                    width: expandedColumn.width
                    spacing: 6

                    // A glyph per action, derived from the closed set the
                    // bridge sends (never from the translated sentence).
                    // Unknown actions get a neutral mark. Every name must be in
                    // the bundled Material Symbols subset (IconChromeTest).
                    readonly property string entryGlyph: {
                        var kind = entryRow.modelData.eventKind || ""
                        if (kind === "membership") {
                            var change = entryRow.modelData.membershipChange || ""
                            if (change === "joined")
                                return "keyboard_tab"
                            if (change === "left")
                                return "logout"
                            if (change === "invited")
                                return "person_add"
                            if (change === "kicked" || change === "revoked")
                                return "person_remove"
                            if (change === "banned")
                                return "block"
                            if (change === "unbanned")
                                return "check_circle"
                            return "group"
                        }
                        if (kind === "member_profile")
                            return "edit_square"
                        if (kind === "m.room.name" || kind === "m.room.topic")
                            return "edit_square"
                        if (kind === "m.room.avatar")
                            return "image"
                        if (kind === "m.room.encryption")
                            return "lock"
                        if (kind === "m.room.pinned_events")
                            return "push_pin"
                        if (kind === "m.room.canonical_alias")
                            return "link"
                        if (kind === "m.room.power_levels")
                            return "shield"
                        return "info"
                    }

                    Icon {
                        name: entryRow.entryGlyph
                        size: AppTheme.scaled(AppTheme.textMeta)
                        color: AppTheme.textMuted
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: 2
                        // Decorative: the sentence carries the meaning.
                        Accessible.ignored: true
                    }

                    Label {
                    Layout.fillWidth: true
                    height: Math.max(16, implicitHeight)
                    text: entryRow.modelData.description || ""
                    // Mandatory, a security control: the sentence embeds remote
                    // display names, and AutoText would promote markup to
                    // StyledText, so an <img> in a name would make every viewer
                    // fetch its URL. Plain text fetches nothing.
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
}
