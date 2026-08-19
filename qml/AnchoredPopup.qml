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

    // Smallest usable size for the component; the share-based sizing below
    // never goes under it unless the window itself is smaller.
    property real minWidth: 260
    property real minHeight: 280
    // Id under which the dragged share is remembered across restarts
    // (SettingsManager::pickerWidthShare, whitelisted there). BOTH pickers use
    // the same id on purpose: resizing one resizes the other. Empty means "do
    // not persist".
    property string sizeSettingsKey: ""

    readonly property Item overlayItem: Overlay.overlay

    parent: anchorItem ? anchorItem : overlayItem

    // Never wider than the anchor — "on the right go no further than the text
    // box" — and never wider than the window when there is no anchor.
    readonly property real maxWidth: {
        if (anchorItem)
            return anchorItem.width
        return overlayItem ? overlayItem.width - AppTheme.spacingM * 2
                           : minWidth
    }
    // Never taller than the room above the anchor. The anchor sits at the
    // bottom of the window, so reserving its own height plus the margins is
    // the bound, and it needs no coordinate mapping.
    readonly property real maxHeight: {
        if (!overlayItem)
            return minHeight
        var room = overlayItem.height - AppTheme.spacingM * 2
        if (anchorItem)
            room -= anchorItem.height + anchorGap
        return room
    }

    // v0.6.7: the picker is sized as a SHARE of the space available to it,
    // never as a pixel count. That single decision covers three separate
    // reports at once:
    //
    //   * resizing the window scales the picker continuously, instead of it
    //     sitting at one fixed size until the window got too small to hold it;
    //   * a size dragged on one screen still makes sense on another;
    //   * and because a share is unitless, the GIF and emoji pickers can share
    //     one remembered value even though their design sizes differ.
    //
    // widthFraction/heightFraction are the defaults; userWidthFraction /
    // userHeightFraction (0 = never resized) override them and are what gets
    // persisted. maxAutoWidth/maxAutoHeight bound only the DEFAULT share, so an
    // untouched picker never becomes absurd on a very large display — a share
    // the user chose by hand is honoured to the full available space.
    property real widthFraction: 0.38
    property real heightFraction: 0.62
    property real maxAutoWidth: 560
    property real maxAutoHeight: 760
    property real userWidthFraction: 0
    property real userHeightFraction: 0

    readonly property real effectiveWidthFraction:
        userWidthFraction > 0 ? userWidthFraction : widthFraction
    readonly property real effectiveHeightFraction:
        userHeightFraction > 0 ? userHeightFraction : heightFraction

    width: {
        var cap = userWidthFraction > 0 ? maxWidth
                                        : Math.min(maxWidth, maxAutoWidth)
        var want = maxWidth * effectiveWidthFraction
        // The floor itself is capped: in a window too narrow for minWidth the
        // available space wins, or the picker would overhang its anchor.
        return Math.max(Math.min(want, cap), Math.min(minWidth, cap))
    }
    height: {
        var cap = userHeightFraction > 0 ? maxHeight
                                         : Math.min(maxHeight, maxAutoHeight)
        var want = maxHeight * effectiveHeightFraction
        return Math.max(Math.min(want, cap), Math.min(minHeight, cap))
    }

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
    // preferAbove flips that order — for popovers whose trigger sits at
    // the BOTTOM of its own content (the read-receipt chips), opening
    // upward is the natural direction and keeps a card that grows after
    // placement away from the window's bottom edge.
    property bool preferAbove: false
    function placeAtPoint() {
        if (!overlayItem)
            return
        x = Math.max(AppTheme.spacingS,
                     Math.min(anchorPoint.x - width / 2,
                              overlayItem.width - width - AppTheme.spacingS))
        var below = anchorPoint.y + AppTheme.spacingXS
        var above = anchorPoint.y - height - AppTheme.spacingXS
        if (preferAbove) {
            y = above >= AppTheme.spacingS
                ? above
                : Math.min(below,
                           overlayItem.height - height - AppTheme.spacingS)
            return
        }
        y = below + height <= overlayItem.height - AppTheme.spacingS
            ? below
            : Math.max(AppTheme.spacingS, above)
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
    // A content-sized popup can GROW after placement: placeAtPoint() runs
    // in onAboutToShow, before a list's delegates have materialized, so
    // the placement decision is made against the header-only height and
    // the settled card can extend past the window's bottom edge
    // (2026-08-19, reader card — screenshot-confirmed on the desktop;
    // the offscreen fixture is rescued by Qt's own popup positioner, so
    // this clamp is the explicit guarantee). Same clamp, keyed on the
    // popup's own size — still never a re-place.
    onWidthChanged: clampInsideWindow()
    onHeightChanged: clampInsideWindow()

    // ── Resize, driven by PopupResizeGrip ───────────────────────────────
    //
    // There is no "detach" here, and there deliberately is not: the placement
    // bindings keep the bottom-right corner pinned, so a bigger size grows the
    // popup up and to the left on its own. That is also why the grip lives at
    // the TOP-LEFT — it is the only free corner — and why dragging it away
    // from the anchor is what makes the popup bigger.
    function resizeTo(w, h) {
        if (maxWidth > 0)
            userWidthFraction = Math.max(0.08, Math.min(1, w / maxWidth))
        if (maxHeight > 0)
            userHeightFraction = Math.max(0.08, Math.min(1, h / maxHeight))
    }

    // Persisted as per-mille of the available space, under a key BOTH pickers
    // share — resizing either one resizes the other, which is what was asked
    // for and is only coherent because the stored value is a share rather than
    // a pixel size.
    function endResize() {
        if (sizeSettingsKey.length > 0)
            app.settings.setPickerShare(sizeSettingsKey,
                                        Math.round(userWidthFraction * 1000),
                                        Math.round(userHeightFraction * 1000))
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
                root.userWidthFraction =
                    app.settings.pickerWidthShare(root.sizeSettingsKey) / 1000
                root.userHeightFraction =
                    app.settings.pickerHeightShare(root.sizeSettingsKey) / 1000
            }
            if (!root.anchorItem)
                root.placeAtPoint()
        }
    }
}
