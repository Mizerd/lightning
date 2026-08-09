import QtQuick
import QtQuick.Controls
import MatrixClient

// v0.6.7: an overlay Popup that stays attached to the control that opened it,
// and that the user can resize by its corner grip.
//
// ── Placement ───────────────────────────────────────────────────────────
//
// x/y are BINDINGS, not assignments. Two earlier versions got this wrong in
// different ways:
//
//   1. Placement ran once, from onAboutToShow, against a point the caller had
//      snapshotted with mapToItem(). mapToItem() walks ancestor geometry
//      without establishing any dependency on it, so nothing re-ran on a
//      window resize — the popup kept its absolute x/y while the button it
//      belonged to moved out from under it, and a picker opened from the
//      composer ended up floating mid-window.
//
//   2. Re-placing from a deferred call on every reflow. That fixed the drift
//      but introduced two visible defects, both reported: it costs a full
//      event-loop turn, which reads as the popup LAGGING behind the window
//      edge, and it runs exactly ONCE per trigger — so when the anchor's own
//      layout settled after that call, the popup was left a few pixels off
//      with nothing left to correct it.
//
// A binding has neither problem: it re-evaluates on every dependency change,
// in the same frame, however many times it takes to settle. `anchorX`/
// `anchorY` below therefore read the dependencies EXPLICITLY (the overlay's
// size, the anchor's own geometry) before calling mapToItem, because the call
// alone would register none of them.
//
// Bindings alone are NOT sufficient, though, and the third mistake was
// believing they were. They cover an anchor that moves within its own parent;
// they do not cover an anchor whose overlay position changed because an
// ANCESTOR moved, since none of the named dependencies change and
// `parent.width` is already final by then. That is a production shape, not a
// hypothetical — the thread panel is a fixed-width item at the end of a
// RowLayout, so a window resize slides the whole panel while
// threadEmojiButton.x never moves; measured at ~200px of permanent error. The
// deferred `settle()` pass below closes it WITHOUT restoring the lag, because
// the binding still does the visible work in the same frame and the bump only
// re-reads afterwards. See
// GifPickerSelectionQmlTest::openPickerFollowsAnAnchorMovedByItsAncestor.
//
// ── Resizing ────────────────────────────────────────────────────────────
//
// A PopupResizeGrip drives userWidth/userHeight. While a drag is in progress
// the popup is `detached`, which disengages the x/y bindings so the top-left
// stays pinned and the dragged corner tracks the pointer exactly — the
// behaviour of a window, which is what was asked for. It stays detached after
// the drag (still clamped inside the window on a resize), and re-attaches to
// its anchor on the next open(), keeping the new size.
Popup {
    id: root

    // The control this popup belongs to — the composer's GIF/emoji button, a
    // toolbar keycap, etc. Anchoring uses its top-centre, so the popup hangs
    // below it (or flips above when there is no room). Optional.
    property Item anchorItem: null
    // Explicit anchor in `parent` (overlay) coordinates, used when there is no
    // anchorItem (a reaction picker opened at a point inside a message row).
    property point anchorPoint: Qt.point(0, 0)

    // Size. `defaultWidth`/`defaultHeight` are the component's own design
    // size; userWidth/userHeight (0 = never resized) win when set, and both
    // are always clamped to what the window can actually show.
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

    // True from the moment a resize starts until the next open(). While it is
    // set, the popup keeps the position it has instead of tracking its anchor.
    property bool detached: false

    parent: Overlay.overlay

    width: {
        var desired = userWidth > 0 ? userWidth : defaultWidth
        var avail = parent ? parent.width - AppTheme.spacingM * 2 : desired
        return Math.min(avail, Math.max(minWidth, desired))
    }
    height: {
        var desired = userHeight > 0 ? userHeight : defaultHeight
        var avail = parent ? parent.height - AppTheme.spacingM * 2 : desired
        return Math.min(avail, Math.max(minHeight, desired))
    }

    // Bumped whenever the anchor may have moved for a reason the property
    // reads below cannot see. Chiefly: the scene graph only becomes valid as
    // the popup is shown, and mapToItem() answers 0 before the items are in a
    // scene together. Without this kick the placement binding would evaluate
    // once at component completion (getting 0, i.e. clamped hard to the left
    // edge) and never re-run, because none of its named dependencies change
    // afterwards — the geometry was already at its final value.
    property int placementRevision: 0

    // Anchor position in overlay coordinates. The `deps` reads are load-
    // bearing: mapToItem() establishes no dependency on the geometry it walks,
    // so without naming them this binding would never re-evaluate.
    readonly property real anchorX: {
        var rev = placementRevision
        if (!parent)
            return 0
        if (!anchorItem)
            return anchorPoint.x
        var deps = parent.width + anchorItem.x + anchorItem.width
        return anchorItem.mapToItem(parent, anchorItem.width / 2, 0).x
    }
    readonly property real anchorY: {
        var rev = placementRevision
        if (!parent)
            return 0
        if (!anchorItem)
            return anchorPoint.y
        var deps = parent.height + anchorItem.y + anchorItem.height
        return anchorItem.mapToItem(parent, 0, 0).y
    }

    // Clamped fully inside the overlay: horizontally centred on the anchor,
    // vertically below it when it fits and above it when it does not.
    readonly property real placedX:
        parent ? Math.max(AppTheme.spacingS,
                          Math.min(anchorX - width / 2,
                                   parent.width - width - AppTheme.spacingS))
               : 0
    readonly property real placedY: {
        if (!parent)
            return 0
        var below = anchorY + AppTheme.spacingXS
        return below + height <= parent.height - AppTheme.spacingS
            ? below
            : Math.max(AppTheme.spacingS,
                       anchorY - height - AppTheme.spacingXS)
    }

    // Engaged only while attached to an ANCHOR ITEM. RestoreNone so that
    // detaching (a resize) leaves x/y exactly where they were rather than
    // snapping back to some earlier value.
    //
    // v0.6.7 review (L1): the anchorItem test is deliberate. A popup opened at
    // a bare point (the reaction pickers, which have no stable item inside a
    // scrolling message row) must NOT be re-placed on a window resize: its
    // point is already stale, so re-placing against it slides an edge-clamped
    // popover back toward nothing in particular, and can flip one that had
    // opened above its anchor to below. Those get an initial placement at show
    // and a clamp afterwards — nothing more.
    Binding {
        target: root
        property: "x"
        value: root.placedX
        when: !root.detached && root.anchorItem !== null
        restoreMode: Binding.RestoreNone
    }
    Binding {
        target: root
        property: "y"
        value: root.placedY
        when: !root.detached && root.anchorItem !== null
        restoreMode: Binding.RestoreNone
    }

    // For a popup the placement bindings are not driving — one the user has
    // resized, or one with no anchor item — keep it inside a shrinking window
    // without moving it otherwise. Clamp only, never a re-place, which would
    // undo the position the user chose or the point the caller captured.
    function clampInsideWindow() {
        if (!visible || !parent)
            return
        if (anchorItem && !detached)
            return  // the bindings own this one
        x = Math.max(AppTheme.spacingS,
                     Math.min(x, parent.width - width - AppTheme.spacingS))
        y = Math.max(AppTheme.spacingS,
                     Math.min(y, parent.height - height - AppTheme.spacingS))
    }

    // v0.6.7 review (H1): a deferred SETTLE pass, and it is load-bearing.
    //
    // anchorX/anchorY name `anchorItem.x` and `parent.width` as dependencies,
    // which covers an anchor that moves within its own parent. It does NOT
    // cover an anchor whose overlay position changed because an ANCESTOR
    // moved — and that is a live configuration, not a hypothetical: the thread
    // panel is a fixed 340px item at the end of a RowLayout, so on a window
    // resize the whole panel slides by the full delta while its internal
    // layout does not reflow at all and threadEmojiButton.x never changes. The
    // binding then re-evaluates exactly once, synchronously, possibly before
    // the ancestor has been repositioned, and nothing re-triggers it. Measured
    // at 200px off, permanently.
    //
    // So: the binding still moves the popup in the SAME FRAME as the window
    // edge (this is what killed the reported lag), and this bump re-reads
    // afterwards, once layouts have settled. When the first reading was
    // already right it changes nothing.
    function settle() {
        if (visible)
            placementRevision++
    }
    Connections {
        target: root.visible ? root.parent : null
        function onWidthChanged() {
            root.clampInsideWindow()
            Qt.callLater(root.settle)
        }
        function onHeightChanged() {
            root.clampInsideWindow()
            Qt.callLater(root.settle)
        }
    }

    // ── Resize, driven by PopupResizeGrip ───────────────────────────────
    function beginResize() { detached = true }

    // Clamped so a drag can never push the popup past the window edge it is
    // growing toward, and never below the component's usable minimum.
    function resizeTo(w, h) {
        if (!parent)
            return
        var maxW = parent.width - x - AppTheme.spacingS
        var maxH = parent.height - y - AppTheme.spacingS
        userWidth = Math.max(minWidth, Math.min(w, maxW))
        userHeight = Math.max(minHeight, Math.min(h, maxH))
    }

    // v0.6.7 review (L3): persists the user's INTENT (userWidth/userHeight),
    // not the clamped effective width. Storing the clamped value meant that
    // resizing inside a very narrow window wrote a pair below the store's
    // sanity floor, which is treated as "forget it" and erased a size the user
    // had chosen earlier on a bigger window. The read path and the `width`
    // binding both clamp to the live window anyway, so intent is the right
    // thing to keep.
    function endResize() {
        if (sizeSettingsKey.length > 0)
            app.settings.setPickerSize(sizeSettingsKey,
                                       Math.round(userWidth),
                                       Math.round(userHeight))
    }

    // Re-attach to the anchor on every open, carrying the remembered size.
    // Reading the stored value here (rather than binding it) keeps this the
    // one point where a persisted size can enter, so a mid-session drag is
    // never fighting the store.
    //
    // A Connections object rather than an inline handler: both derived
    // pickers assign their own onAboutToShow, and a Connections gives this an
    // independent connection that runs after theirs and that no subclass can
    // displace. (Both DO run either way — Qt 6.11.1 chains base-inline,
    // derived-inline, base-Connections — so this is about ordering, not about
    // whether it runs at all.)
    Connections {
        target: root
        function onAboutToShow() {
            root.detached = false
            if (root.sizeSettingsKey.length > 0) {
                root.userWidth = app.settings.pickerWidth(root.sizeSettingsKey)
                root.userHeight = app.settings.pickerHeight(root.sizeSettingsKey)
            }
            // The scene is valid from here on, so force the placement binding
            // to re-read the anchor's real position. See placementRevision.
            root.placementRevision++
            // A popup with no anchor item is not driven by the bindings above,
            // so it takes its one placement here (and is only clamped after).
            if (!root.anchorItem) {
                root.x = root.placedX
                root.y = root.placedY
            }
        }
    }
}
