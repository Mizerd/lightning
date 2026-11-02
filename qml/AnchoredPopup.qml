import QtQuick
import QtQuick.Controls
import MatrixClient

Popup {
    id: root

    // Qt keeps the panel at least this far inside the window on every side.
    // An anchored panel is sized from the window height less the anchor's
    // height, which assumes the anchor sits a fixed margin off the bottom; the
    // composer sits higher than that (about 34 px more at the 360 px floor),
    // and the panel's top used to leave the window. The margin pulls it back
    // by the overshoot. It is a no-op while the panel fits, so the rigid
    // pinned-corner placement is unchanged at ordinary sizes.
    topMargin: AppTheme.spacingS
    leftMargin: AppTheme.spacingS
    rightMargin: AppTheme.spacingS
    bottomMargin: AppTheme.spacingS

    property Item anchorItem: null
    property point anchorPoint: Qt.point(0, 0)
    property real anchorGap: 2

    property real minWidth: 260
    property real minHeight: 280
    property string sizeSettingsKey: ""

    readonly property Item overlayItem: Overlay.overlay

    parent: anchorItem ? anchorItem : overlayItem

    readonly property real maxWidth: {
        if (anchorItem)
            return anchorItem.width
        return overlayItem ? overlayItem.width - AppTheme.spacingM * 2
                           : minWidth
    }
    readonly property real maxHeight: {
        if (!overlayItem)
            return minHeight
        var room = overlayItem.height - AppTheme.spacingM * 2
        if (anchorItem)
            room -= anchorItem.height + anchorGap
        return room
    }

    property real widthFraction: 0.38
    property real heightFraction: 0.62
    property real maxAutoWidth: 560
    property real maxAutoHeight: 760
    property real userWidthFraction: 0
    property real userHeightFraction: 0
    property real autoWidth: 0
    property real autoHeight: 0
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

    property bool preferAbove: false
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

    readonly property string gripCorner:
        anchorItem ? "topLeft" : (placedAbove ? "topRight" : "bottomRight")

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
        if (!_resizing) {
            _pinLeft = x
            _pinTop = y
            _pinRight = x + width
            _pinBottom = y + height
        }
        var fromLeft = gripCorner === "topLeft" || gripCorner === "bottomLeft"
        var fromTop = gripCorner === "topLeft" || gripCorner === "topRight"
        if (pointPlaced) {
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

    function endResize() {
        _resizing = false
        if (sizeSettingsKey.length > 0)
            app.settings.setPickerShare(sizeSettingsKey,
                                        Math.round(userWidthFraction * 1000),
                                        Math.round(userHeightFraction * 1000))
    }

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
