import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// One rebindable action on the Settings → Keyboard page: what it does, the
// key it is on, a Change control that CAPTURES the next combination, and a
// Reset that only exists once the action has been moved off its default.
//
// WHY CAPTURE AND NOT A TEXT FIELD. A text field would need the user to
// spell "Ctrl+Shift+X" the way QKeySequence spells it, and would silently
// store nothing for every other spelling. Capturing the real press is the
// only input method that cannot be typed wrong.
//
// THE ONE QT MECHANISM THIS DEPENDS ON. While capturing, this item accepts
// Qt's ShortcutOverride event. Qt sends that event to the FOCUS ITEM before
// it dispatches a shortcut, and accepting it turns the shortcut back into an
// ordinary key press delivered here. Without it, pressing Ctrl+K to rebind
// the quick switcher would OPEN the quick switcher — a Shortcut is consumed
// before the focused item ever sees the key (the same Qt fact that keeps
// timeline paging on a Keys handler rather than a Shortcut). Every shortcut
// in the application is therefore capturable, including the ones that are
// currently bound to something.
//
// ── TWO SHORTCUTS THAT RENDER AS THE SAME STRING ARE ONE CONTROL ──────
//
// The action name was the only cell with `fillWidth`, so it absorbed the
// whole shortfall while the 132 px keycap and the Change button kept their
// full width, and it ELIDED. Measured at 640x520 — the app's own declared
// minimum width plus a little — "Open the quick switcher" and "Open the
// quick switcher in command mode" BOTH rendered as "Open the …", so there
// was no way to tell which shortcut the Change button beside them was about
// to rebind. Others collapsed to "Quit Light…", "Open Setti…", "Show the k…".
//
// The collision class is much wider than that one pair — four rows begin
// "Show or hide", three "Mark the…", four "Open the…" — so rewriting the
// descriptions to differ in their first ten characters is not a fix, it is
// a request that English read like a database key. The description WRAPS
// instead, and at a narrow width the row STACKS so the wrap has room:
// nothing is ever shortened, at any width, in any translation.
//
// `compact` is pushed in by the host rather than read from this item's own
// width on purpose. A `columns:` bound to `row.width` inside a layout that
// derives the row's width from `columns` is a loop waiting for a window
// dragged across the threshold; the settings pane knows its own width and
// nothing downstream of this changes it.
GridLayout {
    id: row

    // Stack the name above the controls instead of beside them.
    property bool compact: false

    columns: row.compact ? 1 : 2
    rowSpacing: AppTheme.spacing4
    columnSpacing: AppTheme.spacing8

    // Set by the delegate from the model's roles.
    property string actionId: ""
    property string description: ""
    property string currentSequence: ""
    property string defaultSequence: ""
    property bool isDefault: true
    property string conflictsWith: ""
    property string shadowNote: ""

    // Live capture state.
    property bool capturing: false
    property string captureError: ""

    objectName: "shortcutRow_" + actionId

    function beginCapture() {
        row.captureError = ""
        row.capturing = true
        captureSink.forceActiveFocus()
    }
    function endCapture() {
        row.capturing = false
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 2
        Label {
            objectName: "shortcutName_" + row.actionId
            Layout.fillWidth: true
            text: row.description
            color: AppTheme.stormText
            font.pixelSize: AppTheme.textBody
            // NOT elide — see the file header. A name shortened to the
            // point where two rows read alike is worse than a tall row.
            wrapMode: Text.WordWrap
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
        }
        // Three different messages can appear under one row and they mean
        // very different things, so they are three Loaders rather than one
        // Label with a ternary: a conflict means BOTH actions are dead, a
        // shadow means both still work, and a capture error means nothing
        // was stored. Loaders (not `visible:`) because a Label whose text
        // can be "" keeps ItemObservesViewport forever — the single most
        // expensive QML mistake this codebase has recorded. This row is not
        // in the timeline, but the habit is the point.
        Loader {
            Layout.fillWidth: true
            active: row.conflictsWith !== ""
            visible: active
            sourceComponent: Label {
                objectName: "shortcutConflict_" + row.actionId
                width: parent ? parent.width : implicitWidth
                wrapMode: Text.WordWrap
                text: row.conflictsWith
                color: AppTheme.stormDanger
                font.pixelSize: AppTheme.textMeta
            }
        }
        Loader {
            Layout.fillWidth: true
            active: row.conflictsWith === "" && row.shadowNote !== ""
            visible: active
            sourceComponent: Label {
                width: parent ? parent.width : implicitWidth
                wrapMode: Text.WordWrap
                text: row.shadowNote
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
            }
        }
        Loader {
            Layout.fillWidth: true
            active: row.captureError !== ""
            visible: active
            sourceComponent: Label {
                objectName: "shortcutCaptureError_" + row.actionId
                width: parent ? parent.width : implicitWidth
                wrapMode: Text.WordWrap
                text: row.captureError
                color: AppTheme.stormDanger
                font.pixelSize: AppTheme.textMeta
            }
        }
    }

    // The keycap and its buttons travel together: they are ONE cell of the
    // grid, so `columns: 1` stacks them under the name instead of breaking
    // the four of them across four rows.
    RowLayout {
        id: controlGroup
        spacing: AppTheme.spacing8
        Layout.alignment: row.compact ? Qt.AlignLeft | Qt.AlignVCenter
                                      : Qt.AlignRight | Qt.AlignVCenter

        // The key itself, or the capture prompt in its place. One rectangle for
        // both states so the row does not change width when capture starts.
        Rectangle {
            id: chip
            objectName: "shortcutChip_" + row.actionId
            Layout.preferredWidth: Math.max(132, chipLabel.implicitWidth
                                            + AppTheme.spacing12 * 2)
            Layout.preferredHeight: 28
            radius: AppTheme.radiusControl
            color: row.capturing ? AppTheme.stormSelection : AppTheme.stormPanel
            border.width: row.capturing ? 2 : 1
            border.color: row.capturing
                          ? AppTheme.bolt
                          : (row.conflictsWith !== "" ? AppTheme.stormDanger
                                                      : AppTheme.stormBorder)
            Label {
                id: chipLabel
                anchors.centerIn: parent
                text: row.capturing ? qsTr("Press a combination…")
                                    : (row.currentSequence !== ""
                                       ? row.currentSequence : qsTr("Not set"))
                color: row.capturing ? AppTheme.stormText : AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textMeta
                font.family: AppTheme.monoFont
            }
        }

        // The focus sink that does the capturing. It is a zero-size Item rather
        // than a focusable button, so nothing about the visible chip depends on
        // Qt's focus ring.
        Item {
            id: captureSink
            objectName: "shortcutCaptureSink_" + row.actionId
            width: 0
            height: 0
            focus: false
            activeFocusOnTab: false

            // See the header note: this is what lets a key that is ALREADY a
            // live shortcut reach us instead of firing.
            Keys.onShortcutOverride: (event) => {
                if (row.capturing)
                    event.accepted = true
            }
            Keys.onPressed: (event) => {
                if (!row.capturing) {
                    event.accepted = false
                    return
                }
                event.accepted = true
                if (event.key === Qt.Key_Escape) {
                    row.endCapture()
                    return
                }
                var seq = app.shortcuts.sequenceFromKeyEvent(event.key,
                                                             event.modifiers)
                // Empty means the user is still holding modifiers and has not
                // reached a real key yet. Stay open — that is most of the time a
                // capture is running, not an error.
                if (seq === "")
                    return
                var err = app.shortcuts.setBinding(row.actionId, seq)
                if (err !== "") {
                    // NOTHING was stored. Keep capturing so the next press is a
                    // correction rather than requiring another click on Change.
                    row.captureError = err
                    return
                }
                row.captureError = ""
                row.endCapture()
            }
            onActiveFocusChanged: {
                // Clicking anywhere else abandons the capture. Without this the
                // row would keep swallowing shortcut overrides after the user
                // had visibly moved on.
                if (!activeFocus)
                    row.endCapture()
            }
        }

        AppButton {
            storm: true
            size: "sm"
            objectName: "shortcutChange_" + row.actionId
            text: row.capturing ? qsTr("Cancel") : qsTr("Change")
            onClicked: row.capturing ? row.endCapture() : row.beginCapture()
            Accessible.description: row.capturing
                ? qsTr("Cancel capturing a new shortcut for %1").arg(row.description)
                : qsTr("Capture a new shortcut for %1").arg(row.description)
        }
        AppButton {
            storm: true
            size: "sm"
            kind: "ghost"
            objectName: "shortcutReset_" + row.actionId
            // Hidden rather than disabled when already default: a permanently
            // greyed control on every row is noise, and there is nothing to
            // explain about it.
            visible: !row.isDefault
            text: qsTr("Reset")
            onClicked: {
                row.captureError = ""
                row.endCapture()
                app.shortcuts.resetToDefault(row.actionId)
            }
            Accessible.description: qsTr("Reset %1 to %2")
                .arg(row.description).arg(row.defaultSequence)
        }
    }
}
