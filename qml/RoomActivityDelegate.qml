import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    property string groupId: ""
    property var entries: []
    property bool expanded: false
    signal toggleRequested()

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
    readonly property bool hasSecurityChanges: {
        for (var i = 0; i < entriesWithoutCalls.length; ++i) {
            var kind = entriesWithoutCalls[i].eventKind || ""
            if (kind === "m.room.power_levels"
                || kind === "m.room.encryption"
                || kind === "m.room.join_rules"
                || kind === "m.room.history_visibility"
                || kind === "m.room.server_acl")
                return true
        }
        return false
    }
    readonly property bool canExpand: entryCount > 0

    function sameCalendarDay(a, b) {
        return a.getFullYear() === b.getFullYear()
               && a.getMonth() === b.getMonth()
               && a.getDate() === b.getDate()
    }
    function validDate(value) {
        if (!value || typeof value.getTime !== "function")
            return false
        return !isNaN(value.getTime())
    }
    // Same wording as the timeline's date dividers (MessageDelegate.dayLabel).
    function dayLabel(ts) {
        if (!root.validDate(ts))
            return ""
        var stamp = new Date(ts.getFullYear(), ts.getMonth(), ts.getDate())
        var now = new Date(AppLocale.dayStartMs)
        var days = Math.round((now.getTime() - stamp.getTime()) / 86400000)
        if (days === 0) return qsTr("Today")
        if (days === 1) return qsTr("Yesterday")
        if (days > 1 && days < 7)
            return Qt.locale(AppLocale.name).toString(ts, "dddd")
        return Qt.locale(AppLocale.name).toString(
            ts, ts.getFullYear() === now.getFullYear() ? "d MMMM"
                                                       : "d MMMM yyyy")
    }
    readonly property bool spansDays: {
        if (root.entryCount < 2)
            return false
        var first = root.entriesWithoutCalls[0].timestamp
        var last = root.entriesWithoutCalls[root.entryCount - 1].timestamp
        return root.validDate(first) && root.validDate(last)
               && !root.sameCalendarDay(first, last)
    }
    readonly property int renderedEntryCount: activityRepeater.count
    readonly property real expandedContentHeight: expandedColumn.height
    implicitHeight: entryCount > 0 ? activityColumn.implicitHeight : 0

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
                border.width: summaryRow.visualFocus ? 2 : 0
                border.color: AppTheme.focusRing
            }
            contentItem: RowLayout {
                id: summaryContent
                spacing: AppTheme.spacingXS
                Icon {
                    objectName: "stateActivityChevron"
                    visible: root.canExpand
                    name: root.expanded ? "expand_more" : "chevron_right"
                    size: AppTheme.scaled(AppTheme.textSubtitle)
                    color: AppTheme.textMuted
                }
                Label {
                    id: summaryLabel
                    objectName: "stateActivitySummaryLabel"
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    text: {
                        if (root.entryCount === 0)
                            return qsTr("Room updated")
                        var summary = root.spansDays
                            ? qsTr("%n room update(s) since %1",
                                   "collapsed state-event group spanning days",
                                   root.entryCount)
                                .arg(root.dayLabel(root.entriesWithoutCalls[0].timestamp))
                            : qsTr("%n room update(s)",
                                   "collapsed state-event group",
                                   root.entryCount)
                        if (root.hasSecurityChanges)
                            summary = qsTr("%1 · permissions or security changed")
                                .arg(summary)
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
                Column {
                    id: entryBlock
                    required property var modelData
                    required property int index
                    width: expandedColumn.width
                    spacing: 1

                    Label {
                        objectName: "stateActivityDayLabel"
                        // The first day is already the summary's "since" day.
                        visible: root.spansDays
                                 && root.validDate(entryBlock.modelData.timestamp)
                                 && entryBlock.index > 0
                                 && (!root.validDate(root.entriesWithoutCalls[entryBlock.index - 1].timestamp)
                                     || !root.sameCalendarDay(
                                         root.entriesWithoutCalls[entryBlock.index - 1].timestamp,
                                         entryBlock.modelData.timestamp))
                        text: root.dayLabel(entryBlock.modelData.timestamp)
                        textFormat: Text.PlainText
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        font.weight: Font.DemiBold
                        topPadding: entryBlock.index === 0 ? 0 : AppTheme.spacingXS
                    }

                    RowLayout {
                        id: entryRow
                        readonly property var modelData: entryBlock.modelData
                        objectName: "stateActivityEntry"
                        width: entryBlock.width
                        spacing: 6

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
}
