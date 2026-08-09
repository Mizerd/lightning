import QtQuick
import QtQuick.Controls
import MatrixClient

// v0.6.7: an overlay Popup that stays attached to the control that opened it.
//
// Every one of these pickers is parented to Overlay.overlay and positioned
// imperatively, because a Popup's own x/y are plain numbers and the clamping
// ("hang below the button unless that would run off the bottom, then hang
// above") is not expressible as a binding. The bug that motivated extracting
// this: the placement ran ONCE, from onAboutToShow, against a `anchorPoint`
// the caller had snapshotted with mapToItem() a moment earlier. mapToItem()
// establishes no binding dependency on the ancestor geometry it walks, so
// nothing re-ran when the window was resized — the popup kept its absolute
// x/y while the button it belonged to moved out from under it, and a picker
// opened from the composer ended up floating in the middle of the window.
//
// The fix is `anchorItem`: hold the ITEM, not a point, and recompute the point
// on every reflow. Callers that genuinely have no item to anchor to (a
// reaction picker opened at a point inside a scrolling message row) may still
// set `anchorPoint` alone — they simply get the clamp re-applied on resize
// instead of full re-anchoring, which is still strictly better than drifting
// outside the window.
//
// Re-anchoring is deferred through Qt.callLater rather than run inline: a
// resize changes the overlay's size before the layouts underneath have
// repositioned the anchor item, so an inline recompute would read the button's
// PREVIOUS position. Qt.callLater also coalesces the width- and height-change
// pair a single resize produces into one placement pass.
Popup {
    id: root

    // The control this popup belongs to — the composer's GIF/emoji button, a
    // toolbar keycap, etc. Anchoring uses its top-centre, so the popup hangs
    // below it (or flips above when there is no room). Optional.
    property Item anchorItem: null
    // Explicit anchor in `parent` (overlay) coordinates. Recomputed from
    // anchorItem whenever one is set; otherwise used exactly as given.
    property point anchorPoint: Qt.point(0, 0)

    parent: Overlay.overlay

    // Clamp the popup fully inside the overlay: horizontally centred on the
    // anchor, vertically below it when it fits and above it when it does not.
    function placeInsideWindow() {
        if (!parent)
            return
        x = Math.max(AppTheme.spacingS,
                     Math.min(anchorPoint.x - width / 2,
                              parent.width - width - AppTheme.spacingS))
        var below = anchorPoint.y + AppTheme.spacingXS
        y = below + height <= parent.height - AppTheme.spacingS
            ? below
            : Math.max(AppTheme.spacingS,
                       anchorPoint.y - height - AppTheme.spacingXS)
    }

    // Full placement: re-derive the anchor point from anchorItem (when there
    // is one) and re-clamp. Used for the initial placement and for every
    // reflow of an item-anchored popup.
    //
    // v0.6.7 review (L4): re-checks `visible` at CALL time, not only at
    // schedule time — a deferred call can land after the popup has closed.
    function reanchor() {
        if (!visible || !parent)
            return
        if (anchorItem)
            anchorPoint = anchorItem.mapToItem(parent, anchorItem.width / 2, 0)
        placeInsideWindow()
    }

    // What a reflow does. With an anchorItem it is a full re-placement. WITHOUT
    // one it is a clamp only.
    //
    // v0.6.7 review (L6): re-running the full placement for a popup that has
    // no anchor item would MOVE a popup that is still correctly positioned —
    // the reaction pickers open at a point inside a message row, and on a
    // window GROWING a previously edge-clamped popup would slide back toward
    // its stale point, or one that had flipped above its anchor would flip
    // below. Correcting a popup the resize pushed out of bounds is the part
    // that is unambiguously an improvement; keep only that part.
    function reflow() {
        if (!visible || !parent)
            return
        if (anchorItem) {
            reanchor()
            return
        }
        x = Math.max(AppTheme.spacingS,
                     Math.min(x, parent.width - width - AppTheme.spacingS))
        y = Math.max(AppTheme.spacingS,
                     Math.min(y, parent.height - height - AppTheme.spacingS))
    }

    // Deferred/coalesced variant used by every reflow trigger below.
    function scheduleReflow() {
        if (visible)
            Qt.callLater(root.reflow)
    }

    // Every trigger below goes through Connections rather than an inline
    // handler property.
    //
    // v0.6.7 review (L1): an earlier version of this comment claimed a
    // derived component's inline handler OVERRIDES the base's assignment to
    // the same handler property, so an inline handler here "would never run".
    // That is false, and was disproved by probe on Qt 6.11.1: both run, in
    // the order base-inline -> derived-inline -> base-Connections. The real
    // reason to use Connections is ordering and robustness — the base gets an
    // independent connection that runs AFTER a subclass's own handler (so
    // GifPicker.qml's onAboutToShow has already set `tab` before placement
    // reads the resulting geometry) and that no subclass can displace by
    // assigning the same property.
    Connections {
        target: root
        // Placement must be right on the FIRST frame — inline, not deferred.
        function onAboutToShow() { root.reanchor() }
        // Subclasses size themselves against the overlay, so a window resize
        // changes the popup's own dimensions too, and the clamp depends on
        // them.
        function onWidthChanged() { root.scheduleReflow() }
        function onHeightChanged() { root.scheduleReflow() }
    }

    // The window resized (the overlay always tracks the window), so both the
    // clamp bounds and the anchor item's position have moved. This is the
    // trigger that fixes the reported bug, and the one the regression test
    // (GifPickerSelectionQmlTest::openPickerFollowsItsAnchorAcrossAWindowResize)
    // exercises.
    Connections {
        target: root.visible ? root.parent : null
        function onWidthChanged() { root.scheduleReflow() }
        function onHeightChanged() { root.scheduleReflow() }
    }
    // The anchor's OWN geometry changed within its parent.
    //
    // v0.6.7 review (L5): this deliberately does not claim to cover every way
    // a button can move. Anything that shifts an ANCESTOR without changing the
    // anchor's own x/y inside its immediate parent — a side panel opening, the
    // room list collapsing — is not observed here, because mapToItem() walks
    // that chain without establishing a dependency on it. The window-resize
    // case above covers the reported bug; these cover a composer growing a
    // line and similar local reflows.
    Connections {
        target: root.visible ? root.anchorItem : null
        function onXChanged() { root.scheduleReflow() }
        function onYChanged() { root.scheduleReflow() }
        function onWidthChanged() { root.scheduleReflow() }
        function onHeightChanged() { root.scheduleReflow() }
    }
}
