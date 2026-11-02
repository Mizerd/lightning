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
        var locale = Qt.locale(AppLocale.name)
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
                        var summary = qsTr("%n room update(s)",
                                           "collapsed state-event group",
                                           root.entryCount)
                        if (root.hasSecurityChanges)
                            summary = qsTr("%1 · permissions or security changed")
                                .arg(summary)
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
