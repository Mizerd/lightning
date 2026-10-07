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
// only clamped inside a shrinking window, never re-placed. Such a popup is
// pinned on the side facing its point and resizes from the far corner
// (`gripCorner`), so its grip is never on a corner that cannot move.
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
    // (SettingsManager::pickerWidthShare, whitelisted there). Popups that share
    // a key resize together, which is coherent only when their available
    // spaces are the same kind: the composer's GIF and emoji pickers are both a
    // share of the composer card, and the reaction picker, a share of the
    // whole window, has its own key. Empty disables persistence.
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
    // A default in pixels instead of a share (0 = use the share), for a popup
    // whose design size does not grow with the window: the reaction picker,
    // whose available space is the whole window.
    property real autoWidth: 0
    property real autoHeight: 0
    // A content-sized stage (0 = none), e.g. the reaction picker's quick bar.
    // Overrides every share while set, and is never persisted.
    property real fixedWidth: 0
    property real fixedHeight: 0

    readonly property real effectiveWidthFraction:
        userWidthFraction > 0 ? userWidthFraction : widthFraction
    readonly property real effectiveHeightFraction:
        userHeightFraction > 0 ? userHeightFraction : heightFraction

    width: {
        if (fixedWidth > 0)
            return Math.min(fixedWidth, maxWidth)
        var cap = userWidthFraction > 0 ? maxWidth
                                        : Math.min(maxWidth, maxAutoWidth)
        var want = userWidthFraction <= 0 && autoWidth > 0
                ? autoWidth : maxWidth * effectiveWidthFraction
        // The floor is capped too: in a very narrow window the available space
        // wins, or the picker would overhang its anchor.
        return Math.max(Math.min(want, cap), Math.min(minWidth, cap))
    }
    height: {
        if (fixedHeight > 0)
            return Math.min(fixedHeight, maxHeight)
        var cap = userHeightFraction > 0 ? maxHeight
                                         : Math.min(maxHeight, maxAutoHeight)
        var want = userHeightFraction <= 0 && autoHeight > 0
                ? autoHeight : maxHeight * effectiveHeightFraction
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
    // Which side of the point the last placement chose; decides the free
    // corner (gripCorner).
    property bool placedAbove: false
    function placeAtPoint() {
        if (!overlayItem)
            return
        x = Math.max(AppTheme.spacingS,
                     Math.min(anchorPoint.x - width / 2,
                              overlayItem.width - width - AppTheme.spacingS))
        var below = anchorPoint.y + AppTheme.spacingXS
        var above = anchorPoint.y - height - AppTheme.spacingXS
        if (preferAbove) {
            placedAbove = above >= AppTheme.spacingS
            y = placedAbove
                ? above
                : Math.min(below,
                           overlayItem.height - height - AppTheme.spacingS)
            return
        }
        placedAbove = below + height > overlayItem.height - AppTheme.spacingS
        y = placedAbove ? Math.max(AppTheme.spacingS, above) : below
    }

    // The corner a resize grip belongs on: the one that can move. An anchored
    // popup is pinned by its bottom-right corner (the bindings above). A
    // point-placed one is pinned on the side facing its point, so it grows
    // away from the message that opened it: from the bottom-right when it sits
    // below the point, from the top-right when above.
    readonly property string gripCorner:
        anchorItem ? "topLeft" : (placedAbove ? "topRight" : "bottomRight")

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
    // The grip's corner follows the pointer and the opposite corner stays put.
    // Anchored, the bindings pin the bottom-right corner, so a larger size
    // grows up and left. Point-placed, x/y are plain values: the pinned edges
    // are captured at the press and re-applied after every size change.
    property bool _resizing: false
    property real _pinLeft: 0
    property real _pinTop: 0
    property real _pinRight: 0
    property real _pinBottom: 0
    function beginResize() {
        _pinLeft = x
        _pinTop = y
        _pinRight = x + width
        _pinBottom = y + height
        _resizing = true
    }
    function resizeTo(w, h) {
        var pointPlaced = !anchorItem && overlayItem
        // A one-off call (no grip gesture) pins the current rect.
        if (!_resizing) {
            _pinLeft = x
            _pinTop = y
            _pinRight = x + width
            _pinBottom = y + height
        }
        var fromLeft = gripCorner === "topLeft" || gripCorner === "bottomLeft"
        var fromTop = gripCorner === "topLeft" || gripCorner === "topRight"
        if (pointPlaced) {
            // Never past the window edge on the moving side: the window clamp
            // would then shove the pinned corner instead.
            var m = AppTheme.spacingS
            var roomW = fromLeft ? _pinRight - m
                                 : overlayItem.width - m - _pinLeft
            var roomH = fromTop ? _pinBottom - m
                                : overlayItem.height - m - _pinTop
            w = Math.max(Math.min(w, roomW), Math.min(minWidth, roomW))
            h = Math.max(Math.min(h, roomH), Math.min(minHeight, roomH))
        }
        if (maxWidth > 0)
            userWidthFraction = Math.max(0.08, Math.min(1, w / maxWidth))
        if (maxHeight > 0)
            userHeightFraction = Math.max(0.08, Math.min(1, h / maxHeight))
        if (pointPlaced) {
            x = fromLeft ? _pinRight - width : _pinLeft
            y = fromTop ? _pinBottom - height : _pinTop
        }
    }

    // Persisted as per-mille of the available space, under this popup's key.
    function endResize() {
        _resizing = false
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
