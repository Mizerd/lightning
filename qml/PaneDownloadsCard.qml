import QtQuick
import MatrixClient

DownloadsCard {
    id: pane
    objectName: "paneDownloadsCard"

    property rect safeArea: Qt.rect(0, 0, parent ? parent.width : 0,
                                    parent ? parent.height : 0)
    property Item avoid: null
    // The source of truth; items no row claims are shown. `revision` is read
    // so a claim change re-evaluates this binding.
    property var allItems: app && app.downloads ? app.downloads.items : []
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
