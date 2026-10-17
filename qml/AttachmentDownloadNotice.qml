import QtQuick
import MatrixClient

// Uses only space the message already leaves empty. Geometry flows from the
// row into this notice, never back into its layout. A notice that cannot fit
// releases its claim so DownloadNotices sends the item to PaneDownloadsCard.
DownloadsCard {
    id: notice
    objectName: "attachmentDownloadNotice"
    required property Item attachment
    property Item row: null
    property Item toolbar: null
    property Item receipts: null
    // In Bubbles this is the bubble; elsewhere, the visible content children
    // (the transparent full-width column itself is not occupied space).
    property var contentItems: []
    // Other painted/interactive items, including chips outside the content
    // column. Use their mapped bounds, not the transparent Flow's width.
    property var collisionItems: []
    property bool ownBubble: false
    property string mediaKey: ""
    property string surface: "main"
    property bool eligible: true

    // mapToItem alone does not subscribe to intermediate ancestors' offsets.
    // Read them explicitly so headers, captions and layout changes stay bound.
    function rectInRow(item) {
        if (!item || !row) return Qt.rect(0, 0, 0, 0)
        for (var ancestor = item; ancestor && ancestor !== row;
             ancestor = ancestor.parent) {
            ancestor.x; ancestor.y; ancestor.width; ancestor.height
        }
        return item.mapToItem(row, 0, 0, item.width, item.height)
    }
    readonly property rect attachmentRect: rectInRow(attachment)
    readonly property rect toolbarRect: rectInRow(toolbar)
    readonly property rect receiptRect: rectInRow(receipts)
    readonly property rect contentRect: {
        var left = attachmentRect.x
        var right = left + attachmentRect.width
        for (var i = 0; i < contentItems.length; ++i) {
            var item = contentItems[i]
            if (!item || !item.visible || item.width <= 0 || item.height <= 0)
                continue
            var r = rectInRow(item)
            left = Math.min(left, r.x)
            right = Math.max(right, r.x + r.width)
        }
        return Qt.rect(left, attachmentRect.y, right - left, attachmentRect.height)
    }

    width: 300
    x: ownBubble ? contentRect.x - AppTheme.spacing12 - width
                 : contentRect.x + contentRect.width + AppTheme.spacing12
    // The toolbar is lazy. Reserve its whole top band even before hover,
    // using its 28px buttons and 2px padding when it has not been created.
    y: Math.max(attachmentRect.y, toolbar
                ? toolbarRect.y + Math.max(32, toolbarRect.height) + AppTheme.spacing4
                : attachmentRect.y)
    readonly property bool fits: {
        if (!row || attachmentRect.width <= 0 || attachmentRect.height <= 0)
            return false
        var margin = AppTheme.spacing4
        if (x < margin || x + width + margin > row.width
            || y < margin || y + height + margin > row.height)
            return false
        for (var i = 0; i < collisionItems.length; ++i) {
            var item = collisionItems[i]
            if (!item || !item.visible || item.width <= 0 || item.height <= 0)
                continue
            var r = rectInRow(item)
            if (x < r.x + r.width && x + width > r.x
                && y < r.y + r.height && y + height > r.y)
                return false
        }
        // Receipt faces lift 4px and scale 5% on hover. Keep their painted
        // band clear too, without making the pile or the row any larger.
        var gap = AppTheme.spacing8
        if (receipts && receipts.visible && receiptRect.width > 0
            && receiptRect.height > 0
            && x < receiptRect.x + receiptRect.width + gap
            && x + width > receiptRect.x - gap
            && y < receiptRect.y + receiptRect.height + gap
            && y + height > receiptRect.y - gap)
            return false
        return true
    }

    readonly property string token: "" + notice
    readonly property bool wants:
        eligible && fits && items.length > 0 && mediaKey !== ""
    readonly property bool owned:
        wants && DownloadNotices.owner(mediaKey) === token
    onWantsChanged: DownloadNotices.setClaim(mediaKey, token, surface, wants)
    Component.onCompleted: DownloadNotices.setClaim(mediaKey, token, surface, wants)
    Component.onDestruction: DownloadNotices.setClaim(mediaKey, token, surface, false)
    visible: owned
}
