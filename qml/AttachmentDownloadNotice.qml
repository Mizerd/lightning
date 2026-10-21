import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// A download's notice in the empty space beside its file card, attached to the
// row so it scrolls with it. The row creates it only while it has a download
// (a Loader in MessageDelegate), so a row without one pays nothing for it.
// One line, shorter than the card it sits beside, so it fits a plain
// file-card row (the card is 54px; the row has nothing else beside it). The
// card names the file, so the notice does not repeat the name: status, Open,
// Show in folder, and Cancel/Dismiss.
//
// Uses only space the message already leaves empty. Geometry flows from the
// row into this notice, never back into its layout. A notice that cannot fit
// releases its claim so DownloadNotices sends the item to PaneDownloadsCard.
//
// A finished download stays here until it is dismissed or opened, the way a
// browser keeps a finished download in its list: it is quiet (it covers
// nothing) and the user decides when it goes. Off this row (scrolled away,
// another room) it is the pane card's, and it keeps that rule there: once a
// download has been shown here, DownloadNotices remembers it and the pane
// card never times it out (its ten-second expiry is for downloads that never
// had a card, such as a save from the image viewer).
//
// A file whose type can run programs is never claimed here: its whole
// download is on the pane card, which says in visible text why it has no
// Open (DownloadsCard's downloadRiskNotice). One line has no room for that
// sentence, and a tooltip would hide it from the keyboard and the screen
// reader. Risk is known from the start (name and type), so it never moves.
//
// Strings are DownloadsCard's (qsTranslate), so the two places read alike and
// share one translation entry. lupdate extracts them from this file too, so
// an edit on one side adds an entry rather than untranslating the other, and
// `localization` fails until the catalogs are resynced.
Rectangle {
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
    // Every download of this attachment, oldest first (app.downloads order).
    // The notice shows the newest; an older one of the same file stays on the
    // pane card (DownloadNotices.unclaimed), so each is shown once.
    property var items: []
    readonly property var item: items.length > 0 ? items[items.length - 1] : null

    readonly property bool saving: item !== null && item.state === "saving"
    readonly property bool done: item !== null && item.state === "done"
    readonly property bool failed: item !== null && item.state === "failed"
    readonly property bool risky: item !== null && item.risky === true
    // Which controls the state shows. Read these, never the buttons'
    // `visible`: that is the effective visibility, which follows the
    // notice's own, which follows `fits`, which follows the width.
    readonly property bool showsOpen: done && item.canOpen === true
    readonly property bool showsFolder: done
    readonly property bool showsRetry: failed
    // Never "" while the notice exists: MessageDelegate loads it only while
    // its row has a download (items non-empty), and every state below has
    // words. That keeps statusLabel from being BORN holding "", which would
    // make it a permanent viewport observer (docs/timeline-scrolling.md,
    // "Shipped 1"). No other Text here can be empty: the icons have fixed
    // names and the Open button carries its own label (below).
    readonly property string statusText: {
        if (item === null)
            return ""
        var folder = item.folderName || ""
        if (saving)
            return folder.length > 0
                ? qsTranslate("DownloadsCard", "Downloading to %1…").arg(folder)
                : qsTranslate("DownloadsCard", "Downloading…")
        if (failed)
            return item.message
                   || qsTranslate("DownloadsCard", "The download failed.")
        if (item.revealFailed)
            return qsTranslate("DownloadsCard",
                               "Saved, but its folder couldn't be opened.")
        return folder.length > 0
            ? qsTranslate("DownloadsCard", "Saved to %1").arg(folder)
            : qsTranslate("DownloadsCard", "Saved")
    }

    // mapToItem alone does not subscribe to intermediate ancestors' offsets.
    // Read them explicitly so headers, captions and layout changes stay bound.
    function rectInRow(target) {
        if (!target || !row) return Qt.rect(0, 0, 0, 0)
        for (var ancestor = target; ancestor && ancestor !== row;
             ancestor = ancestor.parent) {
            ancestor.x; ancestor.y; ancestor.width; ancestor.height
        }
        return target.mapToItem(row, 0, 0, target.width, target.height)
    }
    readonly property rect attachmentRect: rectInRow(attachment)
    readonly property rect toolbarRect: rectInRow(toolbar)
    readonly property rect receiptRect: rectInRow(receipts)
    readonly property rect contentRect: {
        var left = attachmentRect.x
        var right = left + attachmentRect.width
        for (var i = 0; i < contentItems.length; ++i) {
            var c = contentItems[i]
            if (!c || !c.visible || c.width <= 0 || c.height <= 0)
                continue
            var r = rectInRow(c)
            left = Math.min(left, r.x)
            right = Math.max(right, r.x + r.width)
        }
        return Qt.rect(left, attachmentRect.y, right - left, attachmentRect.height)
    }

    readonly property int margin: AppTheme.spacing4
    readonly property int gap: AppTheme.spacing12
    // Shorter than a file card (54), so it fits beside one in a row that has
    // nothing else in it. Constant across states: a download that finishes
    // never stops fitting.
    readonly property int stripHeight: 34
    readonly property int maxWidth: 320
    // The narrowest the finished layout can be (icon, Open, Show in folder,
    // Dismiss, no status text). Every state is measured against it, so the
    // claim does not move when a download finishes.
    readonly property real minWidth:
        strip.anchors.leftMargin + strip.anchors.rightMargin
        + statusBox.implicitWidth + openButton.implicitWidth
        + folderButton.implicitWidth + closeButton.implicitWidth
        + strip.spacing * 3
    // What the current state's controls need without the status text.
    readonly property real fixedWidth:
        strip.anchors.leftMargin + strip.anchors.rightMargin
        + statusBox.implicitWidth + closeButton.implicitWidth
        + (showsOpen ? openButton.implicitWidth + strip.spacing : 0)
        + (showsFolder ? folderButton.implicitWidth + strip.spacing : 0)
        + (showsRetry ? retryButton.implicitWidth + strip.spacing : 0)
        + strip.spacing
    readonly property real naturalWidth:
        Math.min(maxWidth, fixedWidth + strip.spacing + statusLabel.implicitWidth)

    height: stripHeight

    // The lazy hover toolbar's band, even before it exists (its 28px buttons
    // and 2px padding; barWidth is the Loader's own estimate).
    readonly property rect toolbarBand: {
        if (!toolbar)
            return Qt.rect(0, 0, 0, 0)
        var w = toolbarRect.width > 0 ? toolbarRect.width
              : (toolbar.barWidth !== undefined ? toolbar.barWidth : 200)
        return Qt.rect(toolbarRect.x + toolbarRect.width - w, toolbarRect.y,
                       w, Math.max(32, toolbarRect.height))
    }

    // Where the notice goes, and whether it fits there. Two candidate bands
    // beside the card: centred on it, and just below the toolbar's band (a
    // row with a sender header or a caption has room there, and the toolbar
    // then does not narrow it). The one that fits wins; when both do, the one
    // with more room, then the centred one. One binding over geometry the
    // notice does not affect, so no loop.
    function placeAt(top) {
        var bottom = top + stripHeight
        var out = { y: top, x: 0, width: 0, span: 0, label: false, fits: false }
        if (!row || attachmentRect.width <= 0 || attachmentRect.height <= 0)
            return out
        if (top < margin || bottom + margin > row.height)
            return out
        // Beside the card: the notice's band must meet the card's.
        if (bottom <= attachmentRect.y
            || top >= attachmentRect.y + attachmentRect.height)
            return out
        // Things that can share the band beside the card: the toolbar (and
        // 4px under it), and the receipt faces, which lift 4px and scale 5%
        // on hover, so with a gap.
        var obstacles = []
        if (toolbar && toolbarBand.width > 0
            && top < toolbarBand.y + toolbarBand.height + AppTheme.spacing4
            && bottom > toolbarBand.y)
            obstacles.push(toolbarBand)
        if (receipts && receipts.visible && receiptRect.width > 0
            && receiptRect.height > 0) {
            var g = AppTheme.spacing8
            var rr = Qt.rect(receiptRect.x - g, receiptRect.y - g,
                             receiptRect.width + 2 * g,
                             receiptRect.height + 2 * g)
            if (top < rr.y + rr.height && bottom > rr.y)
                obstacles.push(rr)
        }
        var start = ownBubble ? margin : contentRect.x + contentRect.width + gap
        var end = ownBubble ? contentRect.x - gap : row.width - margin
        for (var i = 0; i < obstacles.length; ++i) {
            var o = obstacles[i]
            // One over the start of the span leaves no span at all.
            if (!ownBubble && o.x <= start && o.x + o.width > start)
                return out
            if (o.x + o.width > start && o.x < end)
                end = Math.min(end, o.x - AppTheme.spacing4)
        }
        out.span = Math.max(0, end - start)
        if (out.span < minWidth)
            return out
        // The status text when it has room for a few words or all of it;
        // otherwise the icon's tooltip and accessible name carry it and the
        // notice is just its controls.
        var w = Math.min(naturalWidth, out.span)
        out.label = w - fixedWidth - strip.spacing
                    >= Math.min(40, statusLabel.implicitWidth)
        out.width = out.label ? w : fixedWidth
        out.x = ownBubble ? end - out.width : start
        for (var j = 0; j < collisionItems.length; ++j) {
            var c = collisionItems[j]
            if (!c || !c.visible || c.width <= 0 || c.height <= 0)
                continue
            var r = rectInRow(c)
            if (out.x < r.x + r.width && out.x + out.width > r.x
                && top < r.y + r.height && bottom > r.y)
                return out
        }
        out.fits = true
        return out
    }
    readonly property real centredY: Math.round(
        attachmentRect.y + (attachmentRect.height - stripHeight) / 2)
    readonly property var placement: {
        var a = placeAt(row ? Math.max(margin, Math.min(row.height - margin
                                                        - stripHeight,
                                                        centredY)) : 0)
        if (!toolbar || toolbarBand.width <= 0)
            return a
        var b = placeAt(Math.max(centredY, toolbarBand.y + toolbarBand.height
                                           + AppTheme.spacing4))
        if (a.fits && (!b.fits || a.width >= b.width))
            return a
        return b.fits ? b : a
    }
    readonly property bool showLabel: placement.label
    readonly property real span: placement.span
    x: placement.x
    y: placement.y
    width: placement.fits ? placement.width : minWidth
    readonly property bool fits: placement.fits

    readonly property string token: "" + notice
    readonly property bool wants:
        eligible && fits && !risky && items.length > 0 && mediaKey !== ""
    readonly property bool owned:
        wants && DownloadNotices.owner(mediaKey) === token
    // The claim this notice holds, as it was made. A row's media key or
    // surface can change in place (the event is replaced, the view is reused),
    // and a release must name what was CLAIMED, not what the bindings say now:
    // releasing the new key left the old one claimed by nobody visible, and
    // its download was shown nowhere.
    property string claimedKey: ""
    property string claimedSurface: ""
    function syncClaim() {
        if (claimedKey !== ""
            && (!wants || claimedKey !== mediaKey || claimedSurface !== surface)) {
            DownloadNotices.setClaim(claimedKey, token, claimedSurface, false)
            claimedKey = ""
            claimedSurface = ""
        }
        if (wants && claimedKey === "") {
            claimedKey = mediaKey
            claimedSurface = surface
            DownloadNotices.setClaim(claimedKey, token, claimedSurface, true)
        }
    }
    onWantsChanged: syncClaim()
    onMediaKeyChanged: syncClaim()
    onSurfaceChanged: syncClaim()
    Component.onCompleted: syncClaim()
    Component.onDestruction: {
        if (claimedKey !== "")
            DownloadNotices.setClaim(claimedKey, token, claimedSurface, false)
    }
    // Shown beside its card at least once: the pane card keeps it until the
    // user dismisses or opens it, wherever it goes next.
    onOwnedChanged: if (owned && item) DownloadNotices.markShownBeside(item.id)
    onItemChanged: if (owned && item) DownloadNotices.markShownBeside(item.id)
    visible: owned
    z: 2

    radius: AppTheme.radiusMd
    color: AppTheme.cardElevated
    border.width: 1
    border.color: AppTheme.borderSubtle

    // A soft shadow so it reads as floating beside the card.
    Rectangle {
        z: -1
        anchors.fill: parent
        anchors.topMargin: 2
        anchors.bottomMargin: -3
        radius: notice.radius
        color: AppTheme.shadowSoft
    }

    RowLayout {
        id: strip
        anchors.fill: parent
        anchors.leftMargin: AppTheme.spacing8
        anchors.rightMargin: AppTheme.spacing4
        spacing: AppTheme.spacing4

        Item {
            id: statusBox
            objectName: "downloadNoticeStatusIcon"
            implicitWidth: 20
            implicitHeight: 20
            Layout.preferredWidth: implicitWidth
            Layout.preferredHeight: implicitHeight
            AppBusyIndicator {
                anchors.centerIn: parent
                visible: notice.saving
                running: notice.saving && notice.visible
                size: 16
            }
            Icon {
                anchors.centerIn: parent
                visible: !notice.saving
                name: notice.done ? "check_circle" : "error"
                size: 18
                color: notice.failed ? AppTheme.danger : AppTheme.success
            }
            // The status, also when its label is hidden for width.
            Accessible.role: Accessible.StaticText
            Accessible.name: notice.statusText
            HoverHandler { id: statusHover }
            ToolTip.visible: statusHover.hovered
            ToolTip.delay: 400
            ToolTip.text: notice.statusText
        }

        Label {
            id: statusLabel
            objectName: "downloadNoticeStatus"
            visible: notice.showLabel
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: notice.failed ? AppTheme.danger : AppTheme.textMuted
            font.pixelSize: AppTheme.textMeta
            text: notice.statusText
            HoverHandler { id: labelHover }
            ToolTip.visible: labelHover.hovered && statusLabel.truncated
            ToolTip.delay: 400
            ToolTip.text: notice.statusText
        }
        AppButton {
            id: openButton
            objectName: "downloadOpenButton"
            visible: notice.showsOpen
            size: "sm"
            // No 72px floor here: every pixel beside the card counts.
            minWidth: 0
            kind: "primary"
            text: qsTranslate("DownloadsCard", "Open")
            // AppButton's own content row always builds a leading Icon, and
            // with no iconName that Icon is a Text holding "": a permanent
            // viewport observer in a timeline row. The same label without
            // the icon; contentItem is deferred, so AppButton's default row
            // is never created, and its implicitWidth (which reads that row)
            // is replaced here.
            implicitWidth: leftPadding + openLabel.implicitWidth + rightPadding
            contentItem: Label {
                id: openLabel
                objectName: "buttonLabel"
                text: openButton.text
                color: openButton._ink
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textMeta
                font.weight: AppTheme.weightStrong
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                elide: Label.ElideRight
            }
            Accessible.name: notice.item
                ? qsTranslate("DownloadsCard", "Open %1").arg(notice.item.fileName)
                : ""
            onClicked: {
                if (app.downloads.open(notice.item.id))
                    app.downloads.dismiss(notice.item.id)
            }
        }
        IconButton {
            id: folderButton
            objectName: "downloadShowInFolderButton"
            visible: notice.showsFolder
            iconName: "folder"
            iconSize: 18
            implicitWidth: 28; implicitHeight: 28
            Accessible.name: qsTranslate("DownloadsCard", "Show in folder")
            ToolTip.text: qsTranslate("DownloadsCard", "Show in folder")
            ToolTip.visible: hovered
            ToolTip.delay: 500
            onClicked: app.downloads.showInFolder(notice.item.id)
        }
        IconButton {
            id: retryButton
            objectName: "downloadRetryButton"
            visible: notice.showsRetry
            iconName: "refresh"
            iconSize: 18
            implicitWidth: 28; implicitHeight: 28
            Accessible.name: qsTranslate("DownloadsCard", "Try again")
            ToolTip.text: qsTranslate("DownloadsCard", "Try again")
            ToolTip.visible: hovered
            ToolTip.delay: 500
            onClicked: app.downloads.retry(notice.item.id)
        }
        IconButton {
            id: closeButton
            objectName: notice.saving ? "downloadCancelButton"
                                      : "downloadDismissButton"
            iconName: "close"
            iconSize: 16
            implicitWidth: 24; implicitHeight: 24
            Accessible.name: notice.saving && notice.item
                ? qsTranslate("DownloadsCard", "Cancel download of %1")
                      .arg(notice.item.fileName)
                : qsTranslate("DownloadsCard", "Dismiss")
            ToolTip.text: notice.saving
                          ? qsTranslate("DownloadsCard", "Cancel download")
                          : qsTranslate("DownloadsCard", "Dismiss")
            ToolTip.visible: hovered
            ToolTip.delay: 500
            onClicked: notice.saving ? app.downloads.cancel(notice.item.id)
                                     : app.downloads.dismiss(notice.item.id)
        }
    }
}
