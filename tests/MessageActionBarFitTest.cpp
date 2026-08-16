// v0.7.1: source-contract proof for the message hover action bar fitting a
// thin/one-line row, AND for the safe ownership model that replaced a
// crashing first attempt at the fix.
//
// User report: "if a message is thin, the box with settings and stuff
// doesnt fit" — the bar (28px buttons + 2*spacing2 padding, ~32px tall) was
// anchored INSIDE bubbleRow with a -3px top overhang, and root's clip
// (line 13, `clip: ListView.view === null`, load-bearing for
// row-content-must-not-bleed-into-a-neighbour) chopped it on a short/
// continuation row.
//
// A first fix escaped the clip by having the per-row Loader's loaded
// Rectangle reparent into Overlay.overlay. That CRASHED: bisected against
// timeline-pane-qml-test (52 passed/11 failed on unmodified HEAD; SIGSEGV,
// null deref, after growthDeltaIsDeferredWhileFlickableOwnsTheDrag on the
// reparenting version). The Loader still believed it owned the item for
// destruction purposes after its `parent:` was reassigned elsewhere, and
// destroying the delegate during pagination/room-switch churn produced a
// dangling pointer.
//
// The real fix keeps the diagnosis (a short row genuinely cannot contain
// the bar; escaping the clip is genuinely the answer) but changes HOW: ONE
// shared bar instance is owned by TimelinePane.qml (never created/
// destroyed by a per-row Loader, never reparented after creation — exactly
// like the pre-existing shared reaction picker/profile popover), and the
// active row PUBLISHES only primitive facts into it (a mapped point, an
// eventId, a couple of booleans) — nothing here is ever a stored
// QObject/Item reference, so a row's destruction cannot dangle anything on
// the shared side. The thread panel (a real ListView, clip: false) never
// had the defect and keeps its original, always-safe, in-row bar
// unchanged except for the (harmless, non-reparenting) tooltip-flip fix.
//
// Modeled on ContextMenuContractTest.cpp's read()/bounded-block scanning
// style; never weakens an assertion, only adds new ones.

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

    // MessageDelegate.qml's thread-panel-only bar (the original, never-
    // broken, in-row anchored design) plus the room-timeline fact
    // publishing that lives further down (right after ensureContextMenu),
    // up to openMessageDetails — bounded so later, unrelated content can
    // never false-positive a count-based assertion meant only for this
    // region. The reactions row and read-receipt strip fall inside this
    // span too, but neither contains a ToolTip or a sourceComponent, so
    // they cannot skew the count-based assertions below.
    static QString delegateActionBarRegion(const QString &delegate)
    {
        const int start = delegate.indexOf(
            QStringLiteral("id: threadActionBarLoader"));
        if (start < 0) return {};
        const int end = delegate.indexOf(
            QStringLiteral("function openMessageDetails"), start);
        if (end < start) return {};
        return delegate.mid(start, end - start);
    }

    // TimelinePane.qml's shared bar Rectangle, bounded to just that
    // declaration (through its own closing brace, ending right where the
    // room-switch Connections block begins).
    static QString sharedBarBlock(const QString &pane)
    {
        const int start = pane.indexOf(QStringLiteral("id: sharedMessageActionBar"));
        if (start < 0) return {};
        const int end = pane.indexOf(
            QStringLiteral("function onCurrentRoomIdChanged()"), start);
        if (end < start) return {};
        return pane.mid(start, end - start);
    }

private Q_SLOTS:
    // The bar must be ONE instance declared directly in TimelinePane.qml —
    // never created by a Loader, never reparented after creation (the
    // crashing shape). It escapes clip the same way the pre-existing
    // shared reaction picker/profile popover already do: parent:
    // Overlay.overlay set ONCE, at declaration.
    void sharedActionBarIsOneInstanceDeclaredDirectly()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!pane.isEmpty());
        const QString block = sharedBarBlock(pane);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("parent: Overlay.overlay")));
        // Not Loader-hosted — a Loader whose loaded item is reparented is
        // exactly the crashing shape this replaces.
        QVERIFY(!block.contains(QStringLiteral("Loader {")));
        QVERIFY(!block.contains(QStringLiteral("sourceComponent")));
        QCOMPARE(block.count(QStringLiteral("IconButton {")), 4);
    }

    // MessageDelegate.qml must contain NO per-row Loader whose loaded item
    // reparents into Overlay.overlay. The thread panel's own bar (the only
    // remaining sourceComponent: Rectangle in this file) must not reparent
    // either — it never needed to (clip is false there).
    void messageDelegateNeverReparentsAPerRowLoaderedItem()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        const QString block = delegateActionBarRegion(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(!block.contains(QStringLiteral("parent: Overlay.overlay")));
        // Exactly one sourceComponent: Rectangle in the whole file — the
        // thread-panel bar — and it lives inside this bounded region.
        QCOMPARE(delegate.count(QStringLiteral("sourceComponent: Rectangle")), 1);
        QVERIFY(block.contains(QStringLiteral("sourceComponent: Rectangle")));
    }

    // The thread panel's bar is the pre-existing, never-broken design,
    // restored verbatim (in-row anchors, no escape hatch needed since
    // clip is false there — a real ListView).
    void threadPanelBarKeepsOriginalInRowAnchoring()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = delegateActionBarRegion(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("anchors.top: parent.top")));
        QVERIFY(block.contains(QStringLiteral("anchors.right: parent.right")));
        QVERIFY(block.contains(QStringLiteral("anchors.topMargin: -3")));
        QVERIFY(block.contains(QStringLiteral(
            "active: root.inThreadPanel\n"
            "                        && (latched || rowHover.hovered")));
    }

    // The room-timeline row publishes ONLY primitive facts (an actionKey,
    // an eventId, a bool, two coordinates, a bool) to the shared bar — the
    // exact call signature proves nothing resembling `root` or `this`
    // (an Item/QObject reference) is ever handed over.
    void roomTimelineRowPublishesOnlyPrimitiveFacts()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = delegateActionBarRegion(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("function syncSharedActionBar()")));
        QVERIFY(block.contains(QStringLiteral(
            "root.timelineView.claimActionBar(\n"
            "                root.actionKey, root.eventIdForActions(),\n"
            "                model.redacted === true,\n"
            "                mappedRowAnchor.x, mappedRowAnchor.y,\n"
            "                root.moreMenuOpen)")));
        QVERIFY(block.contains(QStringLiteral(
            "root.timelineView.releaseActionBar(root.actionKey)")));
        // Never a stored row/Item reference on either side.
        QVERIFY(!delegate.contains(QStringLiteral("activeActionsRow")));
    }

    // A row must release its claim (if it holds one) before it is
    // destroyed — Component.onDestruction runs while the object is still
    // valid, which is exactly why this hook (not a reuse/reset path) is
    // the correct place for it on a Repeater-hosted, never-recycled row.
    //
    // It must use the FORCED release. The ordinary one refuses while the
    // pointer is on the bar, which is correct for a live row (the bar has
    // to survive the row's own hover ending) and WRONG for a dying one: a
    // room switch destroys the row, the claim is never cleared, and the
    // bar stays on screen over the next room holding the previous room's
    // event id — React/Reply would then act on a message that is no longer
    // displayed. See TimelinePane.qml's forceReleaseActionBar.
    void rowForceReleasesClaimOnDestruction()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = delegateActionBarRegion(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral(
            "if (root.timelineView && !root.inThreadPanel)\n"
            "            root.timelineView.forceReleaseActionBar(root.actionKey)")));
    }

    // The forced path exists, skips ONLY the hover guard (never the
    // key-ownership guard, or a superseded row could steal the current
    // owner's claim), and clearing always drops the stale hover flag.
    void forcedReleaseSkipsHoverGuardButNotOwnershipGuard()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!pane.isEmpty());
        // The WHOLE body is quoted, which is what proves the negative: the
        // hover guard cannot be present anywhere in it.
        QVERIFY2(pane.contains(QStringLiteral(
                     "function forceReleaseActionBar(key) {\n"
                     "                    if (activeActionsKey !== key)\n"
                     "                        return\n"
                     "                    clearActionBar()\n"
                     "                }")),
                 "forceReleaseActionBar must check ownership, and only ownership");
        // The ordinary release keeps BOTH guards.
        QVERIFY(pane.contains(QStringLiteral(
            "if (activeActionsKey !== key || sharedActionBarHovered)")));
        // Clearing drops the stale hover flag as well as the claim, so a
        // bar that left the screen cannot block the next row's claim.
        QVERIFY(pane.contains(QStringLiteral("function clearActionBar() {")));
        QVERIFY(pane.contains(QStringLiteral("sharedActionBarHovered = false")));
    }

    // TimelinePane's shared state is primitives only (string/real/bool),
    // and the "More" button is reached by a broadcast signal carrying only
    // primitives too — never a var/object parameter that could smuggle a
    // row reference through.
    void timelineSharedStateIsPrimitivesOnly()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!pane.isEmpty());
        QVERIFY(pane.contains(QStringLiteral("property string activeActionsKey: \"\"")));
        QVERIFY(pane.contains(QStringLiteral("property string activeActionsEventId: \"\"")));
        QVERIFY(pane.contains(QStringLiteral("property bool activeActionsRedacted: false")));
        QVERIFY(pane.contains(QStringLiteral("property real activeActionsAnchorX: 0")));
        QVERIFY(pane.contains(QStringLiteral("property real activeActionsAnchorY: 0")));
        QVERIFY(pane.contains(QStringLiteral("property bool activeActionsMoreMenuOpen: false")));
        QVERIFY(pane.contains(QStringLiteral(
            "signal moreMenuRequested(string key, real x, real y)")));
        QVERIFY(pane.contains(QStringLiteral("function claimActionBar(")));
        QVERIFY(pane.contains(QStringLiteral("function releaseActionBar(key) {")));
        // A late release from a superseded row must never clear the
        // current owner's claim.
        QVERIFY(pane.contains(QStringLiteral("if (activeActionsKey !== key")));
    }

    // The "More" button is the one action that still needs THAT row's own
    // (already safe, Popup-based) context menu; it must be reached by the
    // broadcast + key-match pattern, never a stored reference, and must be
    // a no-op in the thread panel (which has no such signal).
    void moreButtonRoutesThroughBroadcastMatchedByKey()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        const QString sharedBlock = sharedBarBlock(pane);
        QVERIFY(!sharedBlock.isEmpty());
        QVERIFY(sharedBlock.contains(QStringLiteral("timeline.moreMenuRequested(")));

        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString delegateBlock = delegateActionBarRegion(delegate);
        QVERIFY(!delegateBlock.isEmpty());
        QVERIFY(delegateBlock.contains(QStringLiteral(
            "target: root.inThreadPanel ? null : root.timelineView")));
        QVERIFY(delegateBlock.contains(QStringLiteral(
            "function onMoreMenuRequested(key, x, y) {\n"
            "            if (key !== root.actionKey)")));
    }

    // The tooltip-flip fix (kept, per instruction) applies in BOTH hosts:
    // the thread panel's own bar and the shared room-timeline bar.
    void tooltipFlipRetainedInBothHosts()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString delegateBlock = delegateActionBarRegion(delegate);
        QVERIFY(!delegateBlock.isEmpty());
        QVERIFY(delegateBlock.contains(QStringLiteral("property bool tooltipsBelow")));
        QCOMPARE(delegateBlock.count(QStringLiteral("ToolTip {")), 3);
        QCOMPARE(delegateBlock.count(QStringLiteral("threadActionBar.tooltipsBelow")), 3);

        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        const QString sharedBlock = sharedBarBlock(pane);
        QVERIFY(!sharedBlock.isEmpty());
        QVERIFY(sharedBlock.contains(QStringLiteral("property bool tooltipsBelow")));
        QCOMPARE(sharedBlock.count(QStringLiteral("ToolTip {")), 4);
        QCOMPARE(sharedBlock.count(QStringLiteral("sharedMessageActionBar.tooltipsBelow")), 4);
    }

    // openContextMenu grew a third parameter so a caller with a point
    // already in Overlay.overlay space can hand it over directly; the two
    // pre-existing root-local callers (keyboard Menu key and right-click)
    // must keep calling it the old, unmapped way.
    void openContextMenuKeepsRootLocalCallersUnchanged()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        QVERIFY(delegate.contains(QStringLiteral(
            "function openContextMenu(x, y, alreadyInOverlaySpace)")));
        QVERIFY(delegate.contains(QStringLiteral(
            "root.openContextMenu(root.width / 2, root.height / 2)")));
        QVERIFY(delegate.contains(QStringLiteral(
            "root.openContextMenu(p.x, p.y)")));
    }
};

QTEST_MAIN(MessageActionBarFitTest)
#include "MessageActionBarFitTest.moc"
