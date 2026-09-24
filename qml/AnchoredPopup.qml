import QtQuick
import QtQuick.Controls
import MatrixClient

// An overlay Popup pinned to the control it belongs to, resizable by a corner
// grip.
//
// Placement: `parent: anchorItem` with x/y in the anchor's coordinates. A
// Popup positions itself relative to its parent, and QQuickPopupPositioner
// tracks geometry changes on that item and all ancestors synchronously, so
// the popup stays rigid relative to its control with no lag or correction
// pass. Computing absolute overlay positions ourselves (once, deferred, or
// via bindings) lagged, drifted or missed ancestor moves.
//
// Layout: the popup sits directly above its anchor with a hairline gap, its
// right edge aligned with the anchor's. The bottom-right corner is pinned, so
// growing extends up and left, and the resize grip is top-left.
//
// Callers with no stable anchor item (reaction pickers opening at a point in
// a scrolling row) set `anchorPoint`: placed once in overlay coordinates, then
// only clamped inside a shrinking window, never re-placed.
Popup {
    id: root

    // The control this popup belongs to; it becomes the popup's parent, which
    // keeps the two rigid relative to each other.
    property Item anchorItem: null
    // Explicit anchor in overlay coordinates, for callers with no anchorItem.
    property point anchorPoint: Qt.point(0, 0)
    // Gap between the popup's bottom and the anchor's top.
    property real anchorGap: 2

    // Smallest usable size; share-based sizing never goes below it unless the
    // window is smaller.
    property real minWidth: 260
    property real minHeight: 280
    // Key under which the dragged share is remembered
    // (SettingsManager::pickerWidthShare, whitelisted there). Both pickers use
    // the same key, so resizing one resizes the other. Empty disables
    // persistence.
    property string sizeSettingsKey: ""

    readonly property Item overlayItem: Overlay.overlay

    parent: anchorItem ? anchorItem : overlayItem

    // Never wider than the anchor, or than the window when there is no anchor.
    readonly property real maxWidth: {
        if (anchorItem)
            return anchorItem.width
        return overlayItem ? overlayItem.width - AppTheme.spacingM * 2
                           : minWidth
    }
    // Never taller than the room above the anchor, which sits at the bottom of
    // the window.
    readonly property real maxHeight: {
        if (!overlayItem)
            return minHeight
        var room = overlayItem.height - AppTheme.spacingM * 2
        if (anchorItem)
            room -= anchorItem.height + anchorGap
        return room
    }

    // Sized as a share of the available space, not in pixels: it scales with
    // the window, transfers between screens, and lets the GIF and emoji pickers
    // share one remembered value despite different design sizes.
    //
    // widthFraction/heightFraction are defaults; userWidthFraction /
    // userHeightFraction (0 = never resized) override them and are persisted.
    // maxAutoWidth/maxAutoHeight cap only the default share; a share the user
    // chose is honoured up to the full space.
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
        // The floor is capped too: in a very narrow window the available space
        // wins, or the picker would overhang its anchor.
        return Math.max(Math.min(want, cap), Math.min(minWidth, cap))
    }
    height: {
        var cap = userHeightFraction > 0 ? maxHeight
                                         : Math.min(maxHeight, maxAutoHeight)
        var want = maxHeight * effectiveHeightFraction
        return Math.max(Math.min(want, cap), Math.min(minHeight, cap))
    }

    // ── Anchored placement: bindings in the anchor's coordinates ──
    // Bottom-right pinned; Qt's popup positioner handles ancestor movement.
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

    // ── Point placement, for callers with no anchor item ──
    // Clamped inside the overlay: centred on the point horizontally, below it
    // if it fits, else above. preferAbove flips that order, e.g. for triggers
    // at the bottom of their content (read-receipt chips), which also keeps a
    // card that grows later away from the window's bottom edge.
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

    // A point-placed popup is only ever clamped inside a shrinking window,
    // never re-placed: the captured point is stale by then.
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
    // A content-sized popup can grow after placement (placeAtPoint() runs
    // before a list's delegates exist), so clamp on its own size changes too.
    onWidthChanged: clampInsideWindow()
    onHeightChanged: clampInsideWindow()

    // ── Resize, driven by PopupResizeGrip ──
    // The bottom-right corner is pinned by the bindings, so a larger size grows
    // up and left; hence the grip at the top-left.
    function resizeTo(w, h) {
        if (maxWidth > 0)
            userWidthFraction = Math.max(0.08, Math.min(1, w / maxWidth))
        if (maxHeight > 0)
            userHeightFraction = Math.max(0.08, Math.min(1, h / maxHeight))
    }

    // Persisted as per-mille of the available space, under the shared key.
    function endResize() {
        if (sizeSettingsKey.length > 0)
            app.settings.setPickerShare(sizeSettingsKey,
                                        Math.round(userWidthFraction * 1000),
                                        Math.round(userHeightFraction * 1000))
    }

    // Load the remembered size on every open, the one point a persisted size
    // enters, so a mid-session drag never fights the store. A Connections
    // object so derived pickers' own onAboutToShow can't displace it.
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
