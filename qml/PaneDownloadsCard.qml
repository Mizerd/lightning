import QtQuick
import MatrixClient

// The pane-level fallback for downloads that no timeline row shows: a save
// started from the image viewer, media history, a non-primary gallery item, a
// collapsed-embed row, or a row that is off screen, not a file card, or has
// insufficient empty space beside its card, an older download of a file
// whose row shows a newer one, and every file whose type can run programs
// (its warning needs a sentence a row's one-line notice has no room for). Every
// download is shown in exactly one place: its row's AttachmentDownloadNotice
// when that row is on screen and claims it (DownloadNotices), otherwise here.
// This also carries the risky-file warning and failures for those paths.
//
// The card rests in the bottom-left corner of `safeArea`, which the host sets
// to the chat column between the room header and the composer (the area the
// voice mini-player uses), so it never covers the Spaces rail, the account and
// settings buttons, the header or the composer. When the mini-player rests in
// the same corner (`avoid`), the card stands above it.
DownloadsCard {
    id: pane
    objectName: "paneDownloadsCard"

    // Where the card may sit, in the parent's coordinates.
    property rect safeArea: Qt.rect(0, 0, parent ? parent.width : 0,
                                    parent ? parent.height : 0)
    // Another floating card in the parent's coordinates, or null.
    property Item avoid: null
    // The source of truth; items no row claims are shown. `revision` is read
    // so a claim change re-evaluates this binding.
    property var allItems: app && app.downloads ? app.downloads.items : []
    // A download that was shown beside its file card keeps that card's rule
    // here: it stays until opened or dismissed (keepUntilDismissed stops the
    // ten-second expiry). Only a download that never had a card, such as a
    // save from the image viewer, expires on its own.
    items: {
        var rev = DownloadNotices.revision
        return DownloadNotices.unclaimed(pane.allItems).map(function(it) {
            return DownloadNotices.wasShownBeside(it.id)
                ? Object.assign({}, it, { keepUntilDismissed: true }) : it
        })
    }
    readonly property int margin: AppTheme.spacing12

    readonly property rect avoidRect: pane.avoid && pane.avoid.visible
                                      && pane.avoid.opacity > 0
        ? Qt.rect(pane.avoid.x, pane.avoid.y, pane.avoid.width,
                  pane.avoid.height)
        : Qt.rect(0, 0, 0, 0)
    readonly property real restY: pane.safeArea.y + pane.safeArea.height
                                  - pane.height - pane.margin
    readonly property bool overlapsAvoid:
        pane.avoidRect.width > 0
        && pane.x < pane.avoidRect.x + pane.avoidRect.width
        && pane.avoidRect.x < pane.x + pane.width
        && pane.restY < pane.avoidRect.y + pane.avoidRect.height
        && pane.avoidRect.y < pane.restY + pane.height

    visible: opacity > 0
    opacity: pane.items.length > 0 ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 140 } }

    width: Math.max(0, Math.min(340, pane.safeArea.width - pane.margin * 2))
    x: pane.safeArea.x + pane.margin
    y: Math.max(pane.safeArea.y + pane.margin,
                pane.overlapsAvoid
                ? pane.avoidRect.y - pane.height - AppTheme.spacing8
                : pane.restY)
}
