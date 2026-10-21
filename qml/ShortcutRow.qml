import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// One rebindable action on Settings → Keyboard: its description, its key, a
// Change control that captures the next combination, and a Reset shown only
// when moved off the default. Capture rather than a text field, so a sequence
// cannot be spelled wrong. While capturing, this item accepts ShortcutOverride,
// which Qt sends to the focus item before dispatching a shortcut; accepting it
// turns the shortcut into a plain key press here. Without it, pressing Ctrl+K
// to rebind the quick switcher would open it. The description wraps, and at
// narrow widths the row stacks, so it is never elided: many descriptions share
// their first words and elided rows became indistinguishable. `compact` is
// pushed in by the host: a `columns:` bound to this row's own width inside a
// layout that sizes the row from `columns` would loop.
GridLayout {
    id: row

    // Stack the name above the controls.
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
        // The capture must be visible to the person who armed it.
        row.revealInFlickable(chip)
    }
    function endCapture() {
        row.capturing = false
        // Escape, Cancel and focus loss all end here: a rejected chord's
        // message belongs to the capture that is over.
        row.captureError = ""
    }

    // A capture that ends by the user's own act (Escape, a stored chord,
    // Cancel) hands focus back to the Change button; the zero-size sink must
    // not keep it, or the next Tab presses land on nothing visible. Focus
    // loss and leaving the page use endCapture() alone and never steal focus.
    function finishCapture() {
        row.endCapture()
        changeButton.forceActiveFocus()
    }

    // Scrolls the nearest enclosing Flickable just enough to show `item`.
    // Tab can reach a control below the fold, and Qt does not follow focus.
    function revealInFlickable(item) {
        var f = item.parent
        while (f && !(f.contentItem !== undefined && f.contentY !== undefined
                      && f.contentHeight !== undefined))
            f = f.parent
        if (!f)
            return
        var m = AppTheme.spacing12
        var top = item.mapToItem(f.contentItem, 0, 0).y
        var bottom = top + item.height
        if (top - m < f.contentY)
            f.contentY = top - m
        else if (bottom + m > f.contentY + f.height)
            f.contentY = bottom + m - f.height
        f.returnToBounds()
    }

    // A click anywhere outside this row's controls abandons a capture: blank
    // space and sidebar buttons take no focus, so onActiveFocusChanged alone
    // never sees them. A TapHandler on the window does NOT work for this: it
    // never fires when the click lands on an item that accepts the press (an
    // ItemDelegate, a Flickable's content), which is most of Settings. So,
    // while capturing, a window-sized MouseArea sits above everything, ends
    // the capture on the press and then declines it, letting the click reach
    // whatever was under it. A press inside this row's own controls is left
    // alone, or Cancel would end the capture and then re-arm it. Leaving the
    // page hides the row, which ends it too.
    onVisibleChanged: if (!visible && row.capturing) row.endCapture()
    MouseArea {
        id: outsideCatcher
        objectName: "shortcutOutsideCatcher_" + row.actionId
        parent: row.Window.window ? row.Window.window.contentItem : row
        width: parent ? parent.width : 0
        height: parent ? parent.height : 0
        z: 1000000
        visible: row.capturing
        enabled: row.capturing
        acceptedButtons: Qt.AllButtons
        onPressed: (mouse) => {
            var p = mapToItem(controlGroup, mouse.x, mouse.y)
            var inside = p.x >= 0 && p.y >= 0 && p.x <= controlGroup.width
                         && p.y <= controlGroup.height
            if (!inside)
                row.endCapture()
            mouse.accepted = false
        }
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
            // Wrap, not elide (see the header).
            wrapMode: Text.WordWrap
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
        }
        // Three messages with different meanings, so three Loaders: a conflict
        // (both actions dead), a shadow (both still work), a capture error
        // (nothing stored). Loaders, since an empty Label stays a viewport
        // observer.
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

    // The keycap and buttons are one grid cell, so `columns: 1` stacks them
    // under the name as a group.
    RowLayout {
        id: controlGroup
        spacing: AppTheme.spacing8
        Layout.alignment: row.compact ? Qt.AlignLeft | Qt.AlignVCenter
                                      : Qt.AlignRight | Qt.AlignVCenter

        // The key, or the capture prompt in its place; one rectangle so the
        // width does not change when capture starts.
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

        // The capturing focus sink: a zero-size Item, independent of focus
        // rings.
        Item {
            id: captureSink
            objectName: "shortcutCaptureSink_" + row.actionId
            width: 0
            height: 0
            focus: false
            activeFocusOnTab: false

            // Lets a key that is already a live shortcut reach us (see the
            // header).
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
                // A held Enter that started this capture must not become
                // its chord.
                if (event.isAutoRepeat)
                    return
                if (event.key === Qt.Key_Escape) {
                    row.finishCapture()
                    return
                }
                var seq = app.shortcuts.sequenceFromKeyEvent(event.key,
                                                             event.modifiers)
                // Empty means modifiers are still held; keep capturing.
                if (seq === "")
                    return
                var err = app.shortcuts.setBinding(row.actionId, seq)
                if (err !== "") {
                    // Nothing was stored; keep capturing so the next press is a
                    // correction.
                    row.captureError = err
                    return
                }
                row.captureError = ""
                row.finishCapture()
            }
            onActiveFocusChanged: {
                // Focus elsewhere abandons the capture, so this stops
                // swallowing overrides.
                if (!activeFocus)
                    row.endCapture()
            }
        }

        AppButton {
            id: changeButton
            storm: true
            size: "sm"
            objectName: "shortcutChange_" + row.actionId
            text: row.capturing ? qsTr("Cancel") : qsTr("Change")
            onClicked: row.capturing ? row.finishCapture() : row.beginCapture()
            onActiveFocusChanged: if (activeFocus) row.revealInFlickable(changeButton)
            // Qt buttons activate on Space only; Enter must too.
            Keys.onReturnPressed: (event) => { event.accepted = true; if (!event.isAutoRepeat) changeButton.clicked() }
            Keys.onEnterPressed: (event) => { event.accepted = true; if (!event.isAutoRepeat) changeButton.clicked() }
            Accessible.description: row.capturing
                ? qsTr("Cancel capturing a new shortcut for %1").arg(row.description)
                : qsTr("Capture a new shortcut for %1").arg(row.description)
        }
        AppButton {
            storm: true
            size: "sm"
            id: resetButton
            kind: "ghost"
            objectName: "shortcutReset_" + row.actionId
            // Hidden rather than disabled when already default.
            visible: !row.isDefault
            onActiveFocusChanged: if (activeFocus) row.revealInFlickable(resetButton)
            Keys.onReturnPressed: (event) => { event.accepted = true; if (!event.isAutoRepeat) resetButton.clicked() }
            Keys.onEnterPressed: (event) => { event.accepted = true; if (!event.isAutoRepeat) resetButton.clicked() }
            text: qsTr("Reset")
            onClicked: {
                row.captureError = ""
                row.endCapture()
                row.captureError = app.shortcuts.resetToDefault(row.actionId)
            }
            Accessible.description: qsTr("Reset %1 to %2")
                .arg(row.description).arg(row.defaultSequence)
        }
    }
}
