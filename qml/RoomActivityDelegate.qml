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

    // A CALL is not a room update, and it must never come back through here.
    //
    // It used to: the bridge phrased a call as a state event (state_kind
    // "m.call", body the literal words "call event"), so one call rendered as
    // "1 room update" expanding to "call event" — the reported bleak row.
    // TimelineModel now breaks the group at a call row and CallEventDelegate
    // draws it, so this filter normally removes nothing. It is here because
    // this component is also fed by fixtures and by backends that phrase
    // their own state rows, and because a silent second home for calls is
    // exactly how the first one survived: "1 room update" is a wrong
    // sentence, not a small one.
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

    // A collapsed group draws ONE summary line. Since 2026-09-01 the model
    // breaks a state run at every date divider, so on the Rust backend a
    // group can no longer span calendar days and this range label is a
    // SAFETY NET: it only renders on a backend that delivers a multi-day run
    // without dividers between the days (mock/HTTP fixtures can). Same-day
    // groups keep the plain count: a date on every group is noise, not
    // information.
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

                    // A GLYPH PER ACTION, as Sable draws them. Joining, being
                    // removed and being banned are not the same event, and a
                    // column of identical grey sentences says they are.
                    //
                    // Derived from the CLOSED SET the bridge sends beside the
                    // sentence, never from the sentence: that is translated,
                    // so a glyph parsed out of it would be right in exactly
                    // one language. An unknown action gets the neutral mark
                    // rather than a guess — a wrong glyph is a wrong claim
                    // about what somebody did.
                    //
                    // Every name here is in the bundled Material Symbols
                    // SUBSET (IconChromeTest owns that rule); an unmapped one
                    // renders as tofu.
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
                        // Decorative: the sentence beside it carries the
                        // whole meaning, and a screen reader announcing an
                        // unnamed glyph before every line is noise.
                        Accessible.ignored: true
                    }

                    Label {
                    Layout.fillWidth: true
                    height: Math.max(16, implicitHeight)
                    text: entryRow.modelData.description || ""
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
}
