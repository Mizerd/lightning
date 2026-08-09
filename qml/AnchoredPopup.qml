import QtQuick
import QtQuick.Controls
import MatrixClient

// v0.6.7: an overlay Popup pinned to the control it belongs to, resizable by a
// corner grip.
//
// ── Placement: let Qt do it ─────────────────────────────────────────────
//
// This file went through three wrong answers before the right one, all of them
// variations on "compute an absolute overlay position ourselves":
//
//   1. Place once in onAboutToShow from a snapshotted overlay point. The
//      coordinate-mapping call walks ancestor geometry without establishing a
//      dependency on it, so a window resize left the popup behind while its
//      button moved away.
//   2. Re-place from a deferred call on every reflow. Fixed the drift, but
//      cost an event-loop turn (visible LAG) and ran once per trigger, so a
//      late-settling anchor left it a few pixels off.
//   3. Bindings on placedX/placedY. Killed the lag, but missed an anchor moved
//      by an ANCESTOR (~400px off, permanently, on the thread panel), so a
//      deferred correction pass had to be added back — which is what the
//      maintainer then saw as the popup snapping back into place after a
//      resize.
//
// The right answer is not to compute an absolute position at all, and not to
// map coordinates anywhere. A Popup positions itself relative to its `parent`
// item, and QQuickPopupPositioner already listens for geometry changes on that
// item AND every ancestor, so it repositions synchronously and exactly.
// Setting `parent: anchorItem` and expressing x/y in the ANCHOR's coordinates
// therefore makes the popup rigid with respect to the control it belongs to:
// it does not move relative to that control at all, so there is nothing to lag
// or correct.
//
// ── Layout ──────────────────────────────────────────────────────────────
//
// The popup sits directly on top of its anchor with a hairline gap, and its
// right edge lines up with the anchor's right edge — never past it. Both
// bindings keep the BOTTOM-RIGHT corner pinned, which is why growing the popup
// extends it up and to the left, and why the resize grip is at the top-left.
//
// A caller with no stable item to anchor to (the reaction pickers, which open
// at a point inside a scrolling message row) sets `anchorPoint` instead: those
// are placed once from that point in overlay coordinates and afterwards only
// clamped back inside a shrinking window, never re-placed toward what is by
// then a stale point.
Popup {
    id: root

    // The control this popup belongs to. It becomes the popup's PARENT, which
    // is what makes Qt keep the two rigid with respect to each other.
    property Item anchorItem: null
    // Explicit anchor in OVERLAY coordinates, for callers with no anchorItem.
    property point anchorPoint: Qt.point(0, 0)
    // Hairline separation between the popup's bottom and the anchor's top.
    property real anchorGap: 2

    // Size. defaultWidth/defaultHeight are the component's design size;
    // userWidth/userHeight (0 = never resized) win when set. Both are always
    // clamped to what the anchor and the window can actually show.
    property real defaultWidth: 320
    property real defaultHeight: 480
    property real minWidth: 260
    property real minHeight: 280
    property real userWidth: 0
    property real userHeight: 0
    // Short id under which the dragged size is remembered across restarts
    // (SettingsManager::pickerWidth/pickerHeight, whitelisted there). Empty
    // means "do not persist".
    property string sizeSettingsKey: ""

    readonly property Item overlayItem: Overlay.overlay

    parent: anchorItem ? anchorItem : overlayItem

    // Never wider than the anchor — "on the right go no further than the text
    // box" — and never wider than the window when there is no anchor.
    readonly property real maxWidth: {
        if (anchorItem)
            return anchorItem.width
        return overlayItem ? overlayItem.width - AppTheme.spacingM * 2
                           : defaultWidth
    }
    // Never taller than the room above the anchor. The anchor sits at the
    // bottom of the window, so reserving its own height plus the margins is
    // the bound, and it needs no coordinate mapping.
    readonly property real maxHeight: {
        if (!overlayItem)
            return defaultHeight
        var room = overlayItem.height - AppTheme.spacingM * 2
        if (anchorItem)
            room -= anchorItem.height + anchorGap
        return room
    }

    width: Math.min(maxWidth, Math.max(minWidth,
                                       userWidth > 0 ? userWidth : defaultWidth))
    height: Math.min(maxHeight, Math.max(minHeight,
                                         userHeight > 0 ? userHeight : defaultHeight))

    // ── Anchored placement: pure bindings in the ANCHOR's coordinates ────
    // Bottom-right pinned. No coordinate mapping, no revision counter, no
    // deferred correction — Qt's popup positioner handles every ancestor
    // movement, synchronously.
    readonly property real anchoredX: anchorItem ? Math.max(0, anchorItem.width - width) : 0
    readonly property real anchoredY: -height - anchorGap

    Binding {
        target: root
        property: "x"
        value: root.anchoredX
        when: root.anchorItem !== null
        restoreMode: Binding.RestoreNone
    }
    Binding {
        target: root
        property: "y"
        value: root.anchoredY
        when: root.anchorItem !== null
        restoreMode: Binding.RestoreNone
    }

    // ── Point placement, for callers with no anchor item ─────────────────
    // Clamped fully inside the overlay: horizontally centred on the point,
    // vertically below it when it fits and above it when it does not.
    function placeAtPoint() {
        if (!overlayItem)
            return
        x = Math.max(AppTheme.spacingS,
                     Math.min(anchorPoint.x - width / 2,
                              overlayItem.width - width - AppTheme.spacingS))
        var below = anchorPoint.y + AppTheme.spacingXS
        y = below + height <= overlayItem.height - AppTheme.spacingS
            ? below
            : Math.max(AppTheme.spacingS,
                       anchorPoint.y - height - AppTheme.spacingXS)
    }

    // The one correction a point-placed popup gets: keep it inside a window
    // that shrank. Never a re-place — the captured point is already stale, so
    // re-placing would slide an edge-clamped popover somewhere arbitrary or
    // flip one that opened above its point to below.
    function clampInsideWindow() {
        if (!visible || !overlayItem || anchorItem)
            return
        x = Math.max(AppTheme.spacingS,
                     Math.min(x, overlayItem.width - width - AppTheme.spacingS))
        y = Math.max(AppTheme.spacingS,
                     Math.min(y, overlayItem.height - height - AppTheme.spacingS))
    }
    Connections {
        target: root.visible ? root.overlayItem : null
        function onWidthChanged() { root.clampInsideWindow() }
        function onHeightChanged() { root.clampInsideWindow() }
    }

    // ── Resize, driven by PopupResizeGrip ───────────────────────────────
    //
    // There is no "detach" here, and there deliberately is not: the placement
    // bindings keep the bottom-right corner pinned, so a bigger size grows the
    // popup up and to the left on its own. That is also why the grip lives at
    // the TOP-LEFT — it is the only free corner — and why dragging it away
    // from the anchor is what makes the popup bigger.
    function resizeTo(w, h) {
        userWidth = Math.max(minWidth, Math.min(w, maxWidth))
        userHeight = Math.max(minHeight, Math.min(h, maxHeight))
    }

    // Persists the user's INTENT, not the clamped effective size: storing the
    // clamped value meant resizing inside a small window wrote a pair below
    // the store's sanity floor, which is treated as "forget it" and erased a
    // size chosen earlier on a bigger window.
    function endResize() {
        if (sizeSettingsKey.length > 0)
            app.settings.setPickerSize(sizeSettingsKey,
                                       Math.round(userWidth),
                                       Math.round(userHeight))
    }

    // Carry the remembered size in on every open. Reading it here rather than
    // binding it keeps this the one point where a persisted size can enter, so
    // a mid-session drag is never fighting the store.
    //
    // A Connections object rather than an inline handler: both derived pickers
    // assign their own onAboutToShow, and this gives the base an independent
    // connection that runs after theirs and that no subclass can displace.
    Connections {
        target: root
        function onAboutToShow() {
            if (root.sizeSettingsKey.length > 0) {
                root.userWidth = app.settings.pickerWidth(root.sizeSettingsKey)
                root.userHeight = app.settings.pickerHeight(root.sizeSettingsKey)
            }
            if (!root.anchorItem)
                root.placeAtPoint()
        }
    }
}
