// v0.7.2: the message hover action bar must fit a thin/one-line row.
//
// User report: "if a message is thin, the box with settings and stuff doesnt
// fit". The bar is anchored inside the row with a -3px top overhang, and the
// delegate root used to clip (`clip: ListView.view === null`), so a row
// shorter than the bar chopped it.
//
// TWO fixes were tried. The first replaced the per-row bar with ONE shared
// instance parented to Overlay.overlay, positioned from a mapToItem anchor.
// That shipped and was reverted: rows carry `rotation: 180`, so the mapped
// point landed at the row's visual BOTTOM ("the ui appears at the bottom and
// cant even be clicked"), the anchor binding had no dependency to
// re-evaluate on when the view scrolled, and the claim/release handshake let
// two rows show state at once ("sometimes its possible to select two
// messages").
//
// What ships now is the ORIGINAL per-row Loader — plain anchors against the
// row, no coordinate mapping, and hover is naturally exclusive so two rows
// can never both show a bar. The clipping is fixed at its source instead:
// the row no longer clips. That clip dated from the TableView era ("while a
// recycled row is being remeasured"); rows are not recycled now and size to
// their content exactly, and the thread panel has always run the same
// delegate and the same bar unclipped.

#include <QtTest/QtTest>

#include <QFile>

class MessageActionBarFitTest : public QObject
{
    Q_OBJECT

    static QString read(const QString &name)
    {
        QFile file(QStringLiteral(QML_DIR "/") + name);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                               : QString{};
    }

private Q_SLOTS:
    // The row must NOT clip, or the bar is chopped on any row shorter than
    // it — the reported defect.
    void theRowDoesNotClipItsHoverBar()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        QVERIFY2(delegate.contains(QStringLiteral("clip: false")),
                 "the row must not clip, or a thin row cuts the action bar");
        QVERIFY2(!delegate.contains(
                     QStringLiteral("clip: ListView.view === null")),
                 "the TableView-era conditional clip is what chopped the bar");
    }

    // One bar per row, anchored in place. No mapToItem: rows are rotated
    // 180 degrees, so a mapped overlay anchor lands at the visual bottom,
    // and it has nothing to re-evaluate on when the view scrolls.
    void theBarIsAnchoredInTheRowNotMappedIntoAnOverlay()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("id: messageActionBarLoader")));
        QVERIFY(delegate.contains(QStringLiteral("anchors.top: parent.top")));
        QVERIFY(delegate.contains(QStringLiteral("anchors.right: parent.right")));
        QVERIFY2(!delegate.contains(QStringLiteral("mappedRowAnchor")),
                 "an overlay-mapped anchor cannot survive the row rotation");

        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!pane.isEmpty());
        QVERIFY2(!pane.contains(QStringLiteral("sharedMessageActionBar")),
                 "the shared instance was reverted");
        QVERIFY2(!pane.contains(QStringLiteral("claimActionBar")),
                 "the claim/release handshake was reverted");
    }

    // Serves BOTH panels. The gate is hover/pin/menu only — restricting it
    // to the thread panel is what left the room timeline on the shared
    // instance.
    void bothPanelsUseTheSameBar()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY2(!delegate.contains(QStringLiteral("active: root.inThreadPanel")),
                 "the bar must not be gated to the thread panel");
        QVERIFY(delegate.contains(
            QStringLiteral("active: latched || root.actionsVisible")));
        QVERIFY(delegate.contains(QStringLiteral("visible: root.actionsVisible")));
    }

    // Exactly ONE row may show a bar. Reported: "sometimes its possible to
    // select two messages and only one gets ui".
    //
    // Per-row hover is not self-exclusive in practice. A pinned row (its
    // context menu was opened, or it is the reply/edit target) keeps its bar
    // while the pointer moves onto a different row, and both then satisfy
    // their own local condition. The rows must therefore agree through ONE
    // shared value rather than each deciding alone: the view publishes the
    // hovered row's key, a row shows its bar when that key is its own, and a
    // pinned row yields as soon as any row is hovered.
    void onlyOneRowCanShowTheBar()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY2(pane.contains(QStringLiteral("property string hoveredActionsKey")),
                 "the view must own the single hovered-row key");

        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        // The row shows its bar only when the shared key names IT.
        QVERIFY(delegate.contains(QStringLiteral(
            "root.timelineView.hoveredActionsKey === actionKey")));
        // A pinned row defers the moment any other row is hovered.
        QVERIFY2(delegate.contains(QStringLiteral(
                     "&& root.timelineView.hoveredActionsKey === \"\"")),
                 "a pinned row must yield to a hovered one, or two bars show");
        // Publishing: hover sets the key, un-hover clears only its own.
        QVERIFY(delegate.contains(QStringLiteral(
            "root.timelineView.hoveredActionsKey = root.actionKey")));
    }

    // Lazily created, then latched for the delegate's lifetime — zero
    // creation cost for rows the pointer never visits.
    void theBarIsCreatedOnFirstNeed()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("property bool latched: false")));
        QVERIFY(delegate.contains(QStringLiteral(
            "onLoaded: Qt.callLater(function() { latched = true })")));
    }
};

QTEST_MAIN(MessageActionBarFitTest)
#include "MessageActionBarFitTest.moc"
