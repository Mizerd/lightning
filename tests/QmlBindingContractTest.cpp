#include <QRegularExpression>
#include <QtTest/QtTest>

#include <QDir>
#include <QFile>

class QmlBindingContractTest : public QObject
{
    Q_OBJECT

    static QString read(const QString &name)
    {
        QFile file(QStringLiteral(QML_DIR "/") + name);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                               : QString{};
    }

    // The `stateActivity` Item through the sibling `layout` ColumnLayout
    // that follows it in MessageDelegate.qml.
    static QString stateActivityBlock(const QString &delegate)
    {
        const int start = delegate.indexOf(QStringLiteral("id: stateActivity"));
        if (start < 0) return {};
        const int end = delegate.indexOf(
            QStringLiteral("\n    ColumnLayout {\n        id: layout"), start);
        if (end < start) return {};
        return delegate.mid(start, end - start);
    }

    // The balanced-brace body of the FIRST `{` at or after `from`.
    static QString bracedBody(const QString &text, int from)
    {
        const int open = text.indexOf(QLatin1Char('{'), from);
        if (open < 0)
            return {};
        int depth = 0;
        for (int i = open; i < text.size(); ++i) {
            if (text.at(i) == QLatin1Char('{'))
                ++depth;
            else if (text.at(i) == QLatin1Char('}') && --depth == 0)
                return text.mid(open, i - open + 1);
        }
        return {};
    }

    static QStringList qmlFiles()
    {
        QDir dir(QStringLiteral(QML_DIR));
        return dir.entryList({ QStringLiteral("*.qml") }, QDir::Files,
                             QDir::Name);
    }

private Q_SLOTS:
    // 2026-08-23 tester report: "when i click on my own profile it loads up
    // the banner, but then when i click on anyone elses it replaces whatever
    // they might have had with mine" — and the same for Space banners.
    //
    // The cause was one line, repeated in five places: a media-cache
    // completion handler that ASSIGNED `source` on the Image whose `source`
    // was a binding. In QML an imperative write to a bound property destroys
    // the binding, so the first banner that ever finished loading detached
    // that Image from its mxc for the rest of the session — every later
    // profile card, Space, reply quote or preview kept the first image.
    //
    // A cache completion must therefore re-EVALUATE the binding (bump a
    // counter it reads), never replace it. This scans every QML file rather
    // than the five that were wrong, because the pattern is the kind that
    // gets copied into the sixth.
    void mediaCacheHandlersNeverAssignABoundSource()
    {
        static const QRegularExpression assignsSource(
            QStringLiteral("(^|[^=!<>])\\bsource\\s*=[^=]"));
        QStringList offenders;
        int handlersSeen = 0;
        for (const QString &name : qmlFiles()) {
            const QString text = read(name);
            QVERIFY2(!text.isEmpty(), qPrintable(name));
            int at = 0;
            while ((at = text.indexOf(QStringLiteral("onMediaCached"), at)) >= 0) {
                ++handlersSeen;
                const QString body = bracedBody(text, at);
                if (assignsSource.match(body).hasMatch())
                    offenders << name;
                at += body.isEmpty() ? 1 : body.size();
            }
        }
        // The scan is only meaningful if it found the handlers at all.
        QVERIFY2(handlersSeen >= 10,
                 qPrintable(QStringLiteral("only %1 onMediaCached handlers found")
                                .arg(handlersSeen)));
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "onMediaCached assigns a bound `source` (this strands the "
                     "Image on the first image it ever loaded) in: %1")
                                .arg(offenders.join(QStringLiteral(", ")))));
    }

    // 2026-08-23 tester report: "Ctrl+Q does not work when keep running is
    // selected." Qt asks every top-level window to close as part of quitting
    // and a window that REFUSES aborts the quit, so the close-to-tray branch
    // answered the quit request too and Ctrl+Q merely hid the window — in
    // exactly the mode where the tray icon has no menu and Ctrl+Q is the only
    // documented way out.
    void ctrlQQuitsEvenWithCloseToTrayOn()
    {
        const QString main = read(QStringLiteral("Main.qml"));
        QVERIFY(!main.isEmpty());
        // The shortcut announces the intent before asking Qt to quit...
        //
        // 2026-08-26: the sequence itself now comes from ShortcutRegistry
        // (Settings → Keyboard shortcuts), so the anchor is the ACTION ID
        // rather than the literal key — rebinding quit must not be able to
        // retire this contract by moving the string it was pinned to.
        const int quitAction =
            main.indexOf(QStringLiteral("sequenceFor(\"app.quit\")"));
        QVERIFY2(quitAction > 0,
                 "the quit Shortcut must take its sequence from the registry");
        const QString shortcut = bracedBody(
            main, main.lastIndexOf(QStringLiteral("Shortcut {"), quitAction));
        QVERIFY2(!shortcut.isEmpty(), "the quit Shortcut must still exist");
        QVERIFY(shortcut.contains(QStringLiteral("quitRequested = true")));
        QVERIFY(shortcut.contains(QStringLiteral("Qt.quit()")));
        // ...and the close handler stands aside when it sees it. Without the
        // guard in this condition the quit is swallowed.
        const QString closing = bracedBody(
            main, main.indexOf(QStringLiteral("onClosing:")));
        QVERIFY(!closing.isEmpty());
        QVERIFY(closing.contains(QStringLiteral("!window.quitRequested")));
        QVERIFY(closing.contains(QStringLiteral("closeToTray")));
    }

    // 2026-08-23 tester report: "Window geometry and position is not saved."
    // 2026-08-31 report: "opens half off screen, and then fights being
    // dragged; closing and reopening fixes it."
    //
    // THE RULE CHANGED, and the reason the old one existed has not. Qt shows
    // the window during its own componentComplete(), so geometry applied from
    // a completion handler used to land after the window was already on
    // screen and the user watched it jump — which is why the SIZE is still
    // restored declaratively below.
    //
    // The POSITION cannot be. As a binding, the fresh-launch branch read
    // Screen.desktopAvailableWidth/Height, which notify, and `width`, which
    // changes on every resize — so the window re-centred itself under its own
    // user mid-drag, and centred against metrics that were not settled yet on
    // the first launch. It is applied once, imperatively, which breaks the
    // binding for good.
    //
    // What keeps the ORIGINAL defect closed is that the window now starts
    // HIDDEN and is shown only after placement. Nobody watches a window jump
    // that was never on screen. Both halves are asserted, because either one
    // alone brings a defect back.
    void windowGeometryIsRestoredInBindingsAndFlushedOnClose()
    {
        const QString main = read(QStringLiteral("Main.qml"));
        QVERIFY(!main.isEmpty());
        // Declarative restore, from the pre-filtered CONSTANT value.
        QVERIFY(main.contains(QStringLiteral(
            "readonly property rect startupGeometry: app.restorableWindowGeometry")));
        // SIZE stays declarative: neither dimension reads a notifying source.
        for (const QString &prop : { QStringLiteral("width:"),
                                    QStringLiteral("height:") }) {
            QVERIFY2(main.contains(prop + QStringLiteral(" hasStartupGeometry")),
                     qPrintable(prop));
        }
        // POSITION must NOT be a binding — that is the defect.
        QVERIFY2(!main.contains(QStringLiteral("x: hasStartupGeometry")),
                 "window x is a binding again: it will fight the user's drag "
                 "and re-centre on any screen-metric change");
        QVERIFY2(!main.contains(QStringLiteral("y: hasStartupGeometry")),
                 "window y is a binding again");
        // ...and the window must not be visible before it is placed.
        QVERIFY2(main.contains(QStringLiteral("visible: false")),
                 "the window is shown before startup placement is applied, so "
                 "the user watches it jump into position");
        const QString completed = bracedBody(
            main, main.indexOf(QStringLiteral("Component.onCompleted:")));
        QVERIFY(!completed.isEmpty());
        QVERIFY2(completed.contains(QStringLiteral("applyStartupPlacement()")),
                 "startup placement is never applied");
        // Placement, then show, in that order.
        QVERIFY(completed.indexOf(QStringLiteral("applyStartupPlacement()"))
                < completed.indexOf(QStringLiteral("visible = true")));
        // The SIZE still may not be assigned from the handler.
        QVERIFY(!completed.contains(QStringLiteral("window.width")));
        // Saved only from the windowed state, and flushed when closing.
        QVERIFY(main.contains(QStringLiteral("saveWindowGeometry")));
        QVERIFY(main.contains(QStringLiteral(
            "window.visibility !== Window.Windowed")));
        const QString closing = bracedBody(
            main, main.indexOf(QStringLiteral("onClosing:")));
        QVERIFY(closing.contains(QStringLiteral("window.flushGeometry()")));
    }

    // ── The archived-room freeze (2026-08-31) ────────────────────────────
    //
    // CAPTURED, not theorised. LIGHTNING_SCROLL_TRACE from an archived room
    // whose entire history is routine state:
    //
    //   rows=52  srcRows=52  stateRows=46  stateGroups=1 contentH=60
    //   rows=74  srcRows=74  stateRows=65  stateGroups=1 contentH=60
    //   rows=144 srcRows=144 stateRows=129 stateGroups=1 contentH=60
    //
    // A hundred and twenty-nine state rows fold into ONE collapsed group, so
    // contentHeight is 60px and can never reach the viewport height. The fill
    // guard `contentHeight >= height` is therefore unsatisfiable, every page
    // adds ~22 more rows of no height, and the client paginates towards the
    // room's start without pause. stick=1, topDist=0 and nearTop=1 all read
    // true at once, because with 60px of content top and bottom are the same
    // place.
    //
    // A SOURCE CONTRACT ON PURPOSE, and the reason is worth knowing. A
    // behavioural version was written first and DELETED: it passed against
    // the unfixed tree. Measured — unfixed, the mock harness settles by
    // itself at 102 rows and 3 pages, because what drives the runaway in
    // production is the paced row-reveal (the capture shows backlog climbing
    // past 340, each reveal a geometry signal, each geometry signal another
    // fill request) and the mock delivers its pages without that pacing. The
    // harness cannot exhibit the defect, so no assertion over it can catch
    // the defect.
    //
    // What IS checkable is the ordering that made the budget useless.
    void theViewportFillBudgetGatesTheRequestAndNotJustTheTimer()
    {
        const QString source = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!source.isEmpty());

        const int fn = source.indexOf(
            QStringLiteral("function maybeFillViewport()"));
        QVERIFY2(fn >= 0, "maybeFillViewport is gone; re-point this contract");
        const QString body = bracedBody(source, fn);
        QVERIFY(!body.isEmpty());

        const int budget = body.indexOf(
            QStringLiteral("viewportFillRetries >= maxViewportFillRetries"));
        const int request = body.indexOf(
            QStringLiteral("app.pagination.requestViewportFill()"));
        QVERIFY2(budget >= 0, "the fill budget check is gone");
        QVERIFY2(request >= 0, "nothing requests a viewport fill");
        QVERIFY2(budget < request,
                 "requestViewportFill() is issued BEFORE the budget is "
                 "checked, so exhausting the budget stops only the retry "
                 "timer while every geometry signal issues another request — "
                 "the archived-room freeze");

        // And there must be TWO bounds, because there are two kinds of
        // progress. Bounding on visible height alone traded the freeze for
        // the original complaint: the room stopped after eight pages and the
        // reader had to expand the activity group by hand before anything
        // more would load. A page that added ROWS advanced the pagination
        // cursor towards the real messages beyond the collapsed run, so it
        // gets the generous bound; a page that added nothing gets the small
        // one. Expanding an activity group must never be required to reach
        // older history.
        QVERIFY2(body.contains(QStringLiteral("viewportFillLastHeight")),
                 "the budget is spent on attempts rather than on attempts "
                 "that failed to make the content taller");
        QVERIFY2(body.contains(QStringLiteral("viewportFillLastRows")),
                 "nothing distinguishes a page that advanced the pagination "
                 "cursor from one that did nothing, so a collapsed run stops "
                 "the reader dead and expanding it by hand is the only way on");
        QVERIFY2(body.contains(QStringLiteral("maxInvisibleFillRetries")),
                 "invisible progress has no bound of its own");

        // And a call made while a page is still in flight must not spend the
        // budget. This function is called by every geometry signal, so
        // several land between one request and its completion, and each sees
        // no growth for the trivial reason that the page has not arrived.
        // Measured: an archived room reported "fill budget exhausted
        // requests= 8" while its rows had gone 79 -> 123 — pages WERE
        // productive and the generous bound was never reached, because the
        // small one had been eaten by redundant calls.
        const int busyGuard = body.indexOf(QStringLiteral("app.pagination.busy"));
        QVERIFY2(busyGuard >= 0,
                 "calls made while a page is in flight still spend the fill "
                 "budget, so redundant geometry signals exhaust it before any "
                 "page has had a chance to help");
        QVERIFY2(busyGuard < budget,
                 "the in-flight guard must come before the budget is spent");
    }

    // 2026-08-23 tester report: "Panel size is not saved." The room list's
    // width was written back from onWidthChanged while `resizing` was false —
    // which is never true during a drag, and the RELEASE moves nothing, so it
    // produces no widthChanged either. Every intermediate pixel was correctly
    // skipped and the final width was never offered. The falling edge of
    // `resizing` is the one moment that matters.
    void roomListWidthIsSavedWhenTheDragEnds()
    {
        const QString shell = read(QStringLiteral("MainScreen.qml"));
        QVERIFY(!shell.isEmpty());
        const int target = shell.indexOf(
            QStringLiteral("target: roomsPanel.SplitView.view"));
        QVERIFY2(target > 0, "the drag-release trigger must exist");
        // From the enclosing Connections, so the handler and the target it
        // watches are proven to be the same block.
        const int conn = shell.lastIndexOf(QStringLiteral("Connections {"),
                                           target);
        QVERIFY(conn > 0);
        const QString body = bracedBody(shell, conn);
        QVERIFY(body.contains(QStringLiteral("onResizingChanged")));
        QVERIFY(body.contains(QStringLiteral("widthSaver.restart()")));
        // Still debounced: one QSettings write per mouse move is not a thing
        // to do, so the intermediate-pixel guard has to stay.
        QVERIFY(shell.contains(QStringLiteral(
            "onWidthChanged: if (!SplitView.view.resizing) widthSaver.restart()")));
    }

    // Live feedback (2026-08-11, twice): a full-screen image closes on a
    // click ANYWHERE — the image included — and INSTANTLY. The first fix
    // used an exclusive single/double-tap split to keep double-click zoom,
    // which made every close wait out the ~400ms double-click interval;
    // the tap now closes directly and zoom stays on wheel/buttons/keys.
    void imageViewerClosesInstantlyOnClickAnywhere()
    {
        const QString viewer = read(QStringLiteral("ImageViewerOverlay.qml"));
        QVERIFY(!viewer.isEmpty());
        // Both the image tap and the scrim tap close, undelayed.
        QCOMPARE(viewer.count(QStringLiteral("onTapped: viewer.close()")), 2);
        QVERIFY(!viewer.contains(QStringLiteral("exclusiveSignals")));
        QVERIFY(!viewer.contains(QStringLiteral("onDoubleTapped")));
        // Zoom survives through the non-conflicting inputs.
        QVERIFY(viewer.contains(QStringLiteral("WheelHandler")));
        QVERIFY(viewer.contains(QStringLiteral("zoomStep(1.2)")));
    }

    // Live feedback (2026-08-11): dropping files anywhere over the CHAT
    // queues them as composer attachments (the composer's own DropArea only
    // covered the composer bar), and the tray previews are real: static
    // thumbnail for images, animated for GIFs, first-frame player poster
    // for videos, with the remove button preserved.
    void chatWideDropQueuesAttachmentsWithRichPreviews()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!pane.isEmpty());
        QVERIFY(pane.contains(QStringLiteral("id: chatDropArea")));
        QVERIFY(pane.contains(QStringLiteral(
            "app.composer.addAttachment(drop.urls[i])")));
        QVERIFY(pane.contains(QStringLiteral("keys: [\"text/uri-list\"]")));
        // review H1: the chat-wide area must never cover the thread
        // surface — ThreadPanel owns its drops (thread send path). The
        // geometry stops short of the side-by-side panel and collapses to
        // zero under the full-width thread layout.
        QVERIFY(pane.contains(QStringLiteral(
            "width: root.threadSurfaceOpen")));
        QVERIFY(pane.contains(QStringLiteral(
            "? (root.width >= 660 ? root.width - 340 : 0)")));
        QVERIFY(!pane.contains(QStringLiteral(
            "chatDropArea\n        anchors.fill: parent")));

        const QString composer = read(QStringLiteral("MessageComposerBar.qml"));
        QVERIFY(!composer.isEmpty());
        QVERIFY(composer.contains(QStringLiteral("isGifChip")));
        QVERIFY(composer.contains(QStringLiteral("isVideoChip")));
        QVERIFY(composer.contains(QStringLiteral(
            "onClicked: app.composer.attachments.removeAt(index)")));
        // The video poster player renders exactly the first frame.
        QVERIFY(composer.contains(QStringLiteral(
            "MediaPlayer.LoadedMedia")));
    }

    void dialogsHaveIndependentBoundedWidths()
    {
        const QString roomInfo = read(QStringLiteral("RoomInfoPanel.qml"));
        const QString account = read(QStringLiteral("AccountMenu.qml"));
        QVERIFY(!roomInfo.isEmpty());
        QVERIFY(!account.isEmpty());
        QVERIFY(roomInfo.contains(QStringLiteral(
            "width: Math.max(240, Math.min(400, parent ? parent.width - 32 : 400))")));
        QVERIFY(account.contains(QStringLiteral(
            "width: Math.max(240, Math.min(420, parent ? parent.width - 32 : 420))")));
        QVERIFY(!roomInfo.contains(QStringLiteral("Layout.maximumWidth: 360")));
        QVERIFY(!account.contains(QStringLiteral("Layout.maximumWidth: 380")));
    }

    void roomPreviewIsHardClampedToOneLine()
    {
        // The summary layer normalizes previews, but persisted
        // pre-normalization strings (and future producer bugs) must still
        // never expand a room row: explicit '\n's break lines even with
        // elide set, so the label needs the hard clamp. Plain text keeps a
        // message body from rich-formatting the room list.
        const QString delegate = read(QStringLiteral("RoomDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        const int label =
            delegate.indexOf(QStringLiteral("objectName: \"roomPreviewLabel\""));
        QVERIFY(label >= 0);
        const QString block = delegate.mid(label, 1400);
        QVERIFY(block.contains(QStringLiteral("maximumLineCount: 1")));
        QVERIFY(block.contains(QStringLiteral("wrapMode: Text.NoWrap")));
        QVERIFY(block.contains(QStringLiteral("textFormat: Text.PlainText")));
    }

    void paginationVisibilityDoesNotDependOnGeometry()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!pane.isEmpty());
        QVERIFY(pane.contains(QStringLiteral("app.pagination.presentationState")));
        QVERIFY(pane.contains(QStringLiteral("PaginationController.Hidden ? 0 : 32")));
        // v0.6.4: the loading / failure indicator is a TOP OVERLAY, not
        // ListView content. As a ListView header its 0<->32 height toggle
        // changed contentHeight and shoved the reader's viewport (and flipped
        // atYBeginning into extra near-top requests) every time pagination
        // started or stopped. The overlay keeps the semantic-state height but
        // never perturbs timeline geometry.
        QVERIFY(!pane.contains(QStringLiteral("header: Item {")));
        QVERIFY(pane.contains(QStringLiteral("objectName: \"paginationHeader\"")));
        QVERIFY(pane.contains(QStringLiteral("anchors.top: parent.top")));
        QVERIFY(pane.contains(QStringLiteral("restoreScrollAnchor(app.currentRoomId)")));
        QVERIFY(pane.contains(QStringLiteral("saveScrollAnchor(")));
        QVERIFY(pane.contains(QStringLiteral("eventIdAtViewRow(row)")));

        // 2026-08-20 (C5): the shared delegate must NOT name a controller.
        // It used to call app.pagination.jumpToEvent() and read
        // app.pagination.highlightedEventId directly — but ThreadPanel.qml
        // renders this same delegate, and app.pagination is wired only to the
        // ROOM timeline. A thread reply's target is a thread event, which the
        // live room timeline hides (hide_threaded_events), so the click cost
        // eight real room paginations and then reported failure. The delegate
        // now goes through the view contract its host supplies, exactly like
        // openSenderProfile / openReactionPicker already did. These two
        // assertions replace the two that pinned the old hardcoding.
        // The REPLY PREVIEW specifically must not name the room controller.
        // One deliberate app.pagination.jumpToEvent survives in this file: the
        // thread panel's "Open in room" menu item, whose entire purpose is to
        // leave the thread for the room. Routing that through the thread's own
        // navigation would defeat it, so it is correct and stays — which is
        // why this assertion is scoped to the reply target rather than
        // banning the symbol outright.
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.contains(
            QStringLiteral("app.pagination.jumpToEvent(model.replyToEventId")));
        QVERIFY(!delegate.contains(QStringLiteral("app.pagination.highlightedEventId")));
        QVERIFY(delegate.contains(QStringLiteral("navigateToEvent(")));
        QVERIFY(delegate.contains(QStringLiteral("navigationHighlightEventId")));
        // Both hosts must actually supply that contract.
        QVERIFY(pane.contains(QStringLiteral("navigateToEvent")));
        QVERIFY(pane.contains(QStringLiteral("navigationHighlightEventId")));
        const QString threadPanel = read(QStringLiteral("ThreadPanel.qml"));
        QVERIFY(threadPanel.contains(QStringLiteral("navigateToEvent")));
        QVERIFY(threadPanel.contains(QStringLiteral("navigationHighlightEventId")));
        QVERIFY(pane.contains(QStringLiteral("viewportFillCheckScheduled")));
        QVERIFY(pane.contains(QStringLiteral("Qt.callLater(function()")));
        QVERIFY(pane.contains(QStringLiteral("app.pagination.requestViewportFill()")));
        // v0.6.4: near-top pagination is EDGE-triggered with hysteresis, so a
        // reader sitting near the top cannot re-send userInitiated requests
        // every frame and spin the zero-progress loop. The user-scroll trigger
        // sites go through checkNearTopEdge (latched between an enter and a
        // wider exit band); the passive atYBeginning fill trigger stays.
        QVERIFY(pane.contains(QStringLiteral("function checkNearTopEdge(")));
        QVERIFY(pane.contains(QStringLiteral("nearTopArmed")));

        // v0.7.3: every near-top proximity comparison is measured against
        // distanceFromTop(), never against raw contentY. contentY is an offset
        // from originY, and originY is arbitrary and MOVES as history loads.
        // MEASURED: it sat at ~+2484 in the offscreen fixture with the reader at
        // the very top, so `contentY <= height/2` was permanently FALSE there.
        // INFERRED from the live trace: it sat far enough the other way that raw
        // contentY stayed inside the band through all of loaded history, making
        // the enter band permanently true and the exit band unreachable, so the
        // gesture-settle re-arm fired after EVERY gesture in either direction and
        // each one bought four more pagination batches — the reported "it keeps
        // loading old messages each time I scroll up ... and down".
        //
        // A geometry test cannot police this on its own: whether the two
        // measures disagree depends on where originY happens to sit in the
        // fixture, so a fixture with a small originY would pass either way. This
        // scan is the mechanism-level guard — the comparison sites themselves —
        // and no choice of fixture geometry can make it vacuous.
        QVERIFY(pane.contains(QStringLiteral("function distanceFromTop()")));
        // The rotation moved the top of history to the HIGH bound, so the
        // distance is measured from wheelMaxY(). The property under test is
        // unchanged: a distance against a bound, never raw contentY.
        QVERIFY(pane.contains(QStringLiteral(
            "return wheelMaxY() - contentY")));
        // The bands are DISTANCES now, and are named so. The old ...Y names
        // invited exactly the frame confusion above; forbid their return.
        QVERIFY(pane.contains(QStringLiteral("nearTopEnterDistance")));
        QVERIFY(pane.contains(QStringLiteral("nearTopExitDistance")));
        QVERIFY(!pane.contains(QStringLiteral("nearTopEnterY")));
        QVERIFY(!pane.contains(QStringLiteral("nearTopExitY")));
        for (const QString &raw :
                 { QStringLiteral("contentY <= nearTopEnterDistance"),
                   QStringLiteral("contentY >= nearTopExitDistance"),
                   QStringLiteral("contentY < nearTopEnterDistance"),
                   QStringLiteral("contentY > nearTopExitDistance") }) {
            QVERIFY2(!pane.contains(raw),
                     qPrintable(QStringLiteral(
                         "near-top proximity compared against raw contentY "
                         "(\"%1\"); contentY is an offset from a moving originY, "
                         "so this is not a proximity test — use "
                         "distanceFromTop()").arg(raw)));
        }
        // Both bands, and the gesture-settle re-arm, read the corrected measure.
        QVERIFY(pane.contains(QStringLiteral("fromTop <= nearTopEnterDistance")));
        QVERIFY(pane.contains(QStringLiteral("fromTop >= nearTopExitDistance")));
        QVERIFY(pane.contains(QStringLiteral(
            "<= timeline.nearTopEnterDistance")));
        // The progress gate lives at the DISPATCH site, not on the settle
        // re-arm. Guarding only the re-arm was wrong twice over: an upward
        // gesture re-armed the latch and the next DOWNWARD gesture consumed it
        // and fetched, and a reader parked at the exact top could never re-arm
        // because contentY is at its minimum there and cannot decrease. Both
        // conditions live in the distanceFromTop() frame.
        QVERIFY(pane.contains(QStringLiteral("nearTopRequestDistance")));
        QVERIFY(pane.contains(QStringLiteral("fromTop <= 1")));
        QVERIFY(pane.contains(QStringLiteral(
            "fromTop < nearTopRequestDistance - 1")));
        // The baseline must RATCHET to the closest approach, on every in-band
        // sample — not merely record the distance at the last dispatch. Without
        // this, everything between the top and the dispatch point stays unpaid
        // and a later downward sample fetches, which is the reported defect
        // surviving its own fix.
        QVERIFY(pane.contains(QStringLiteral(
            "if (fromTop < nearTopRequestDistance)")));
        QVERIFY2(!pane.contains(QStringLiteral("nearTopRequestY")),
                 "the progress baseline must not live in the raw contentY frame");
        QVERIFY(!pane.contains(QStringLiteral(
            "readonly property int paginationState")));
        QVERIFY(!pane.contains(QStringLiteral("showPaginationStatus")));
        QVERIFY(!pane.contains(QStringLiteral(
            "height: paginationHeader.visible ? paginationHeader.implicitHeight")));
    }

    // v0.7: the loading presentation is the shared Skeleton, and it keys off
    // BOTH renderers' actual status (static Image and AnimatedImage), so a
    // ready GIF frame can never sit behind a lingering placeholder. Delegate
    // reuse still resets media identity.
    void animatedGifSkeletonUsesActiveRendererState()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("objectName: \"imageSkeleton\"")));
        // The static renderer hides while the animated one is playing AND
        // while the image is locally hidden (a hidden bitmap must not be
        // painted at all — see MediaVisibilityStore); the invariant this case
        // is really about is that BOTH renderers' status drives the skeleton,
        // so a ready GIF frame can never sit behind a lingering placeholder.
        QVERIFY(delegate.contains(QStringLiteral(
            "visible: !imageBox.animateGif && !root.mediaHidden")));
        QVERIFY(delegate.contains(QStringLiteral(
            "img.status !== Image.Ready")));
        QVERIFY(delegate.contains(QStringLiteral(
            "animatedImg.status !== AnimatedImage.Ready")));
        QVERIFY(delegate.contains(QStringLiteral(
            "playing: imageBox.animateGif && root.rowOnScreen")));
        QVERIFY(delegate.contains(QStringLiteral("onMediaIdentityChanged")));
    }

    // v0.6.1: the GIF picker is wired to app.gif in both composers, presents
    // provider tabs / search / categories / a result grid / state overlays /
    // attribution, animates previews only while visible, and never renders the
    // sendable original GIF as a grid tile (previews use the small variant).
    void gifPickerWiredIntoBothComposers()
    {
        const QString room = read(QStringLiteral("MessageComposerBar.qml"));
        const QString thread = read(QStringLiteral("ThreadPanel.qml"));
        QVERIFY(room.contains(QStringLiteral("GifPicker {")));
        QVERIFY(room.contains(QStringLiteral("target: \"room\"")));
        QVERIFY(room.contains(QStringLiteral("root.openGifPicker()")));
        QVERIFY(room.contains(QStringLiteral("app.gif.available")));
        QVERIFY(thread.contains(QStringLiteral("GifPicker {")));
        QVERIFY(thread.contains(QStringLiteral("target: \"thread\"")));
        QVERIFY(thread.contains(QStringLiteral("threadGifButton")));

        const QString picker = read(QStringLiteral("GifPicker.qml"));
        QVERIFY(!picker.isEmpty());
        // The picker's OWN internals are not this case's business, and it had grown
        // 170 lines of them: provider tabs, the choose()/snapshot() send path, tile
        // sources, state overlays, keyboard handling. Twenty-six of those needles
        // were asserted a second time in GifPickerRedesignContractTest — which the
        // block's own comments already pointed at — so they live there now, with the
        // rest of the picker's contract, and the real-engine half stays in
        // GifPickerSelectionQmlTest. What remains here is what the name promises:
        // both composers host the picker, and a chosen GIF reaches the right
        // destination.
        //
        // Selecting a GIF routes to the destination-captured send pipeline:
        // the room composer to the room, the thread composer into the thread.
        QVERIFY(room.contains(QStringLiteral(
            "app.gifSend.sendToRoom(app.currentRoomId, result)")));
        QVERIFY(thread.contains(QStringLiteral("app.gifSend.sendToThread(")));
        QVERIFY(thread.contains(QStringLiteral("app.thread.rootEventId")));
        // The picker itself never sends to a room/thread directly.
        QVERIFY(!picker.contains(QStringLiteral("sendToRoom")));
        QVERIFY(!picker.contains(QStringLiteral("sendTextMessage")));
    }

    // v0.6.1: GIF autoplay is a tri-state (Always/OnHover/Never) honored by the
    // timeline and the picker, configured in Settings alongside safe-search,
    // provider, recents and clear actions with a privacy disclosure.
    void gifAutoplayAndSettingsWired()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString picker = read(QStringLiteral("GifPicker.qml"));
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        // Timeline honors the tri-state (Never = static, OnHover = hover-gated).
        QVERIFY(delegate.contains(QStringLiteral("gifMode: app.settings.gifAutoplay")));
        QVERIFY(delegate.contains(QStringLiteral("gifMode === 0 || gifHovered")));
        QVERIFY(delegate.contains(QStringLiteral("app.settings.gifAutoplay !== 2")));
        // Picker previews honor it too.
        QVERIFY(picker.contains(QStringLiteral("app.settings.gifAutoplay")));
        // Settings expose the controls, bound to the settings model.
        QVERIFY(settings.contains(QStringLiteral("app.settings.gifAutoplay = currentValue")));
        QVERIFY(settings.contains(QStringLiteral("app.settings.gifSafeSearch = currentValue")));
        QVERIFY(settings.contains(QStringLiteral("app.settings.gifPreferredProvider = currentValue")));
        QVERIFY(settings.contains(QStringLiteral("app.settings.storeRecentGifs = checked")));
        // Honest provider availability + privacy disclosure + confirmed clears.
        QVERIFY(settings.contains(QStringLiteral("providerConfigured(\"giphy\")")));
        QVERIFY(settings.contains(QStringLiteral(
            "GIF searches are sent directly to the ")));
        QVERIFY(settings.contains(QStringLiteral("gifClearConfirm.open(\"favorites\")")));
        QVERIFY(settings.contains(QStringLiteral("app.gif.favorites.clearAll()")));
        // v0.6.6 (review HIGH-2): the client-local starred-GIF store gets
        // its own visible count/size row and confirmed Clear All — never
        // folded into the Favorites/Recents clear actions above, since it
        // holds actual decrypted file bytes on this device rather than
        // small provider-CDN metadata rows.
        QVERIFY(settings.contains(QStringLiteral("objectName: \"starredGifsSummaryLabel\"")));
        QVERIFY(settings.contains(QStringLiteral("app.gif.starredStore.count")));
        QVERIFY(settings.contains(QStringLiteral("app.gif.starredStore.totalBytes")));
        QVERIFY(settings.contains(QStringLiteral("kept on this ")));
        // The copy must disclose the sign-out consequence (review finding:
        // sign-out deletes the store; "kept on this device only" alone
        // would read as a durability promise).
        QVERIFY(settings.contains(QStringLiteral("device only and removed ")));
        QVERIFY(settings.contains(QStringLiteral(
            "when you sign out of this ")));
        QVERIFY(settings.contains(QStringLiteral("starredGifsClearConfirm.open()")));
        QVERIFY(settings.contains(QStringLiteral("app.gif.starredStore.clearAll()")));
    }

    // v0.6.1: the thread root uses the Element-style summary card wired to the
    // SDK thread-summary roles, and activating it opens the real thread. The
    // old plain-text "reply(s) in thread" link and the redundant "· in thread"
    // reply label are gone.
    void threadRootUsesSummaryCard()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        QVERIFY(delegate.contains(QStringLiteral("ThreadSummaryCard {")));
        // Gated on the thread-root role. 2026-08-19: the card is now
        // behind a Loader (a never-laid-out Text inside it kept the
        // ItemObservesViewport flag on EVERY row — see the scroll round),
        // so the gate is `active:`; either spelling satisfies the
        // contract, which is "the card appears only on a thread root".
        QVERIFY(delegate.contains(QStringLiteral(
                    "active: model.isThreadRoot === true"))
                || delegate.contains(QStringLiteral(
                    "visible: model.isThreadRoot === true")));
        QVERIFY(delegate.contains(QStringLiteral(
            "replyCount: model.threadReplyCount")));
        QVERIFY(delegate.contains(QStringLiteral(
            "latestKind: model.threadLatestKind")));
        QVERIFY(delegate.contains(QStringLiteral(
            "latestSender: model.threadLatestSenderDisplayName")));
        QVERIFY(delegate.contains(QStringLiteral(
            "onActivated: app.thread.openThread(")));
        // The noisy legacy presentation must not come back.
        QVERIFY(!delegate.contains(QStringLiteral("reply(s) in thread")));
        QVERIFY(!delegate.contains(QStringLiteral("· in thread")));

        // The card renders a thread icon, elides its preview, never plays a
        // full GIF (still label only), and is keyboard-activable + accessible.
        const QString cardQml = read(QStringLiteral("ThreadSummaryCard.qml"));
        QVERIFY(!cardQml.isEmpty());
        QVERIFY(cardQml.contains(QStringLiteral("signal activated()")));
        // Design shell: interface chrome uses a Material Symbols glyph, never
        // an inline Canvas/SVG vector path.
        QVERIFY(cardQml.contains(QStringLiteral("name: \"forum\"")));
        QVERIFY(!cardQml.contains(QStringLiteral("Canvas {")));
        QVERIFY(cardQml.contains(QStringLiteral("elide: Text.ElideRight")));
        QVERIFY(cardQml.contains(QStringLiteral("maximumLineCount: 1")));
        QVERIFY(cardQml.contains(QStringLiteral("Keys.onReturnPressed")));
        QVERIFY(cardQml.contains(QStringLiteral("Keys.onSpacePressed")));
        QVERIFY(cardQml.contains(QStringLiteral("Accessible.role: Accessible.Button")));
        QVERIFY(cardQml.contains(QStringLiteral("qsTr(\"GIF\")")));
        QVERIFY(cardQml.contains(QStringLiteral("qsTr(\"Encrypted reply\")")));
        QVERIFY(cardQml.contains(QStringLiteral("qsTr(\"Message removed\")")));
        // Count is never invented: number only when the SDK count is > 0.
        QVERIFY(cardQml.contains(QStringLiteral(
            "replyCount > 0 ? qsTr(\"%n reply(s)\", \"\", replyCount)")));
    }

    void stateActivityUsesNeutralGroupedPresentation()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("stateGroupEntries")));
        QVERIFY(delegate.contains(QStringLiteral(
            "visible: !root.isVirtualRow && !root.isStateActivity")));
        // No message-bubble-like card: the old Rectangle+cardElevated+border
        // treatment for the collapsed row must be gone, replaced with a
        // compact, clickable summary row.
        const QString activity = read(QStringLiteral("RoomActivityDelegate.qml"));
        QVERIFY(!activity.contains(QStringLiteral("AppTheme.cardElevated")));
        QVERIFY(activity.contains(QStringLiteral("summaryRow")));
        QVERIFY(activity.contains(QStringLiteral("modelData.description")));
        QVERIFY(activity.contains(QStringLiteral("model: expandedColumn.visible ? root.entries")));
        QVERIFY(!activity.contains(QStringLiteral("linkPreviews")));
        QVERIFY(!activity.contains(QStringLiteral("messageActions")));
    }

    // 0.5.14 checkpoint 2: clicking Expand did nothing because the summary
    // row referenced the bare `ListView.view` attached property, which is
    // only populated on the delegate's own root item, not on nested
    // children — every other action in this same file correctly qualifies
    // with `root.ListView.view`. Pin that convention for the state-activity
    // controls specifically, since that's exactly where it regressed.
    void stateActivityQualifiesListViewViewOnNestedControls()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = stateActivityBlock(delegate);
        QVERIFY(!block.isEmpty());
        // The delegate now reaches its host through root.timelineView,
        // which resolves the attached view ONCE in the delegate root's own
        // scope. The hazard this test exists for is unchanged and was
        // confirmed live: an attached property referenced from a nested
        // object attaches to THAT object, where the view is never populated,
        // and fails silently. One such reference (inside a Timer) disabled
        // the whole exact-height cache. So no nested block may name an
        // attached view at all.
        QVERIFY(block.count(QStringLiteral("root.timelineView")) > 0);
        QCOMPARE(block.count(QStringLiteral("ListView.view")), 0);
        QCOMPARE(block.count(QStringLiteral("TableView.view")), 0);
    }

    void roomActivitySettingIsPresentationOnly()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(delegate.contains(QStringLiteral(
            "if (!isRoutineActivity) return true")));
        QVERIFY(delegate.contains(QStringLiteral(
            "if (!app.settings.showRoomActivity) return false")));
        // 2026-08-26: the master switch gained two halves (membership vs
        // profile changes) and both are read in the SAME presentation-only
        // expression — the split must not acquire a second mechanism.
        QVERIFY(delegate.contains(
            QStringLiteral("app.settings.showMembershipEvents")));
        QVERIFY(delegate.contains(
            QStringLiteral("app.settings.showProfileChangeEvents")));
        // v0.6.0: the zero-height presentation filter also covers the
        // thread panel's pinned-root suppression — same mechanism, still
        // presentation-only.
        QVERIFY(delegate.contains(QStringLiteral("naturalImplicitHeight")));
        // 2026-08-20 (C4): a third presentation-only suppression joined the
        // same expression — a date divider whose entire run is hidden. It is
        // the same mechanism (zero height, row stays in the authoritative
        // model), so it belongs in this assertion rather than beside it.
        //
        // 2026-08-22: a FOURTH — every redacted row after the first of its
        // run, which the leader replaces with one "N messages deleted" line.
        // Same mechanism again: the rows stay in the model and the model
        // still counts them, so a deletion never changes what the timeline
        // knows, only what it draws.
        QVERIFY(delegate.contains(QStringLiteral("dividerSuppressed")));
        QVERIFY(delegate.contains(QStringLiteral("deletedFollower")));
        QVERIFY(delegate.contains(QStringLiteral(
            "(!roomActivityVisible || suppressedAsThreadRoot"
            " || dividerSuppressed"
            "\n         || deletedFollower) ? 0")));
        QVERIFY(settings.contains(QStringLiteral("Show room activity")));
        QVERIFY(settings.contains(QStringLiteral(
            "onToggled: app.settings.showRoomActivity = checked")));
    }

    // v0.6.0 checkpoint 8: the unable-to-decrypt placeholder exposes a
    // manual Retry (through the view-provided timeline model, so it works
    // in the thread panel too) and a Security settings jump — and never
    // renders raw session/ciphertext fields.
    void undecryptableRowsExposeRetryAndSecurityActions()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("Retry decryption")));
        QVERIFY(delegate.contains(QStringLiteral(
            "root.timelineModel.retryDecryption()")));
        QVERIFY(delegate.contains(QStringLiteral(
            "app.showSettingsSection(\"security\")")));
        // No ciphertext/session-id MODEL fields are ever bound (the word in
        // a comment is fine; a binding would be model.<field>).
        QVERIFY(!delegate.contains(QStringLiteral("model.sessionId")));
        QVERIFY(!delegate.contains(QStringLiteral("model.ciphertext")));
    }

    // v0.6.0 checkpoint 9 / reshaped for v0.7.x: the Sessions card lists
    // devices with honest trust labels and — since the UIA round — offers
    // remote sign-out through the reusable UIA flow (per-device and
    // all-others), never optimistically (tiles follow the authoritative
    // refetch), and never binds token-like fields. The old "not supported
    // yet" disclaimer must be GONE now that the capability is real.
    void sessionsCardIsHonest()
    {
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(settings.contains(QStringLiteral(
            "onClicked: app.refreshSessionDevices()")));
        QVERIFY(settings.contains(QStringLiteral("model: app.sessionDevices")));
        QVERIFY(settings.contains(QStringLiteral("This session")));
        QVERIFY(settings.contains(QStringLiteral("Not verified")));
        QVERIFY(!settings.contains(QStringLiteral("is not supported yet")));
        QVERIFY(settings.contains(
            QStringLiteral("signOutOtherSessionsButton")));
        QVERIFY(settings.contains(QStringLiteral("sessionSignOutButton_")));
        // The current session is never offered for remote deletion.
        QVERIFY(settings.contains(QStringLiteral(
            "visible: modelData.isCurrent !== true")));
        QVERIFY(!settings.contains(QStringLiteral("accessToken")));
        QVERIFY(!settings.contains(QStringLiteral("access_token")));
    }

    // v0.6.0 checkpoint 10: the recovery input is masked, accepts key or
    // passphrase, is wiped immediately after dispatch, and a successful
    // recovery re-reads SDK trust/backup state. No new-backup or
    // cross-signing SETUP button is faked (UIA limitation documented).
    void recoveryInputIsMaskedClearedAndHonest()
    {
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(settings.contains(QStringLiteral("echoMode: TextInput.Password")));
        QVERIFY(settings.contains(QStringLiteral("Recovery key or passphrase")));
        QVERIFY(settings.contains(QStringLiteral("recoveryField.text = \"\"")));
        QVERIFY(settings.contains(QStringLiteral("app.refreshCryptoHealth()")));
        QVERIFY(!settings.contains(QStringLiteral("Set up backup")));
        QVERIFY(!settings.contains(QStringLiteral("Set up cross-signing")));
    }

    void unreadNavigationUsesSdkMarkerAndBottomThreshold()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(delegate.contains(QStringLiteral("objectName: \"unreadDivider\"")));
        QVERIFY(delegate.contains(QStringLiteral("qsTr(\"New messages\")")));
        QVERIFY(pane.contains(QStringLiteral("objectName: \"jumpToLatestButton\"")));
        QVERIFY(pane.contains(QStringLiteral("!timeline.stickToBottom")));
        // Bottom-follow is latched to user intent via a small slack (a reader
        // who scrolls up is never re-pinned by proximity), replacing the wide
        // 40px window that snapped the view back to the newest message.
        QVERIFY(pane.contains(QStringLiteral("function atBottomEdge()")));
        QVERIFY(pane.contains(QStringLiteral("wheelMinY() + bottomFollowSlack")));
        QVERIFY(!pane.contains(QStringLiteral("contentHeight - 40")));
        QVERIFY(!pane.contains(QStringLiteral("contentY + height === contentHeight")));
    }

    // Native-touchpad architecture: the deferred anchor correction must NEVER
    // run while the user's gesture owns the position, and the touchpad hot path
    // must NOT do per-delta geometry work.
    //   * The pixelDelta branch must NOT call captureViewAnchor() — the old
    //     per-delta indexAt/itemAtIndex/stableIdAt scan is gone; it must keep
    //     the scroll session alive with scrollSettleTimer.restart().
    //   * maintainViewAnchor() must BRANCH on userScrollActive: mid-gesture
    //     it applies a RELATIVE growth delta (round 3 — content resizing
    //     above the reader must not throw the view, the "an image pops in
    //     and I jump" defect), while the ABSOLUTE restore to the captured
    //     offset stays idle-only, since only an absolute write can disagree
    //     with where the gesture has since moved the view and fight it.
    //   * The anchor is (re)captured once the gesture settles — on the mouse
    //     path via onWheelMotionSettled and universally via scrollSettleTimer.
    // The offscreen QPA does not incubate ListView delegates, so the pixel
    // outcome is only provable on a physical touchpad; this scan guards the
    // wiring so a future edit cannot silently reintroduce the mid-gesture
    // absolute-write fight, nor drop the relative growth compensation.
    void touchpadScrollUsesTwoModeAnchorMaintenance()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));

        const int pixelBranch =
            pane.indexOf(QStringLiteral("if (event.pixelDelta.y !== 0)"));
        QVERIFY(pixelBranch >= 0);
        const int angleBranch = pane.indexOf(
            QStringLiteral("else if (event.angleDelta.y !== 0)"), pixelBranch);
        QVERIFY(angleBranch > pixelBranch);
        QString touchpad = pane.mid(pixelBranch, angleBranch - pixelBranch);
        // Strip comment lines before scanning: the branch's comment
        // legitimately names captureViewAnchor() when explaining where the
        // full re-derivation does happen (at settle), but a BARE
        // `captureViewAnchor()` call would resolve through the ListView's
        // scope chain and silently reintroduce the per-delta scan — so the
        // scan must stay broad enough to catch that, and precise enough not
        // to trip on prose.
        {
            QStringList codeOnly;
            const QList<QStringView> touchpadLines =
                QStringView(touchpad).split(QLatin1Char('\n'));
            for (const QStringView &line : touchpadLines) {
                if (!line.trimmed().startsWith(QLatin1String("//")))
                    codeOnly << line.toString();
            }
            touchpad = codeOnly.join(QLatin1Char('\n'));
        }
        // No per-delta anchor scan on the touchpad hot path.
        QVERIFY(!touchpad.contains(QStringLiteral("captureViewAnchor()")));
        // The session stays alive for the whole gesture.
        QVERIFY(touchpad.contains(QStringLiteral("scrollSettleTimer.restart()")));

        // The deferred correction is gated on the scroll session. The
        // scanned region starts at the userScrollActive branch, NOT at the
        // function declaration: the displaced-anchor resolve above it
        // legitimately calls positionViewAtIndex (an absolute view move used
        // purely to materialise a destroyed delegate, with the position
        // restored immediately after), and including it would make the
        // "no absolute write" assertion below false for the wrong reason.
        const int maintain =
            pane.indexOf(QStringLiteral("function maintainViewAnchor()"));
        QVERIFY(maintain >= 0);
        const int guardStart =
            pane.indexOf(QStringLiteral("if (userScrollActive) {"), maintain);
        QVERIFY(guardStart > maintain);
        const int maintainEnd =
            pane.indexOf(QStringLiteral("desired = anchorY + viewAnchorOffset"),
                         guardStart);
        QVERIFY(maintainEnd > guardStart);
        const QString maintainGuard =
            pane.mid(guardStart, maintainEnd - guardStart);
        // The mid-gesture path writes NOTHING. Applying the anchor delta
        // while input owns the viewport was tried twice and rejected twice by
        // physical testing — the second time with real measured heights and
        // with translateActiveMotion() carrying the wheel target along, so
        // neither "the quantity was noise" nor "the engine drove it back out"
        // explains it. anchorY moves both when rows resize under the reader
        // and when the view re-anchors its own loaded rows, and the raw delta
        // cannot tell those apart; feeding it into contentY pulled the reader
        // up and down, including with nothing loading at all.
        // The two contentY scans carry the whole contract; a scan for
        // translateActiveMotion would only match the comment in that branch
        // recording why the write was removed, which is worth keeping.
        QVERIFY(!maintainGuard.contains(QStringLiteral("contentY +=")));
        QVERIFY(!maintainGuard.contains(QStringLiteral("contentY =")));

        // userScrollActive covers the touchpad path via the settle timer, since
        // moving/wheelAnimating are both false there.
        QVERIFY(pane.contains(QStringLiteral(
            "moving || wheelAnimating || scrollSettleTimer.running")));

        // The anchor is captured on gesture settle (mouse path + settle timer).
        const int settled =
            pane.indexOf(QStringLiteral("function onWheelMotionSettled()"));
        QVERIFY(settled >= 0);
        const int settledEnd =
            pane.indexOf(QStringLiteral("scrollSettleTimer.restart()"), settled);
        QVERIFY(settledEnd > settled);
        const QString settledBlock = pane.mid(settled, settledEnd - settled);
        QVERIFY(settledBlock.contains(QStringLiteral("timeline.captureViewAnchor()")));
    }

    void directPreviewUsesControlledSourceAndOriginalUrlActivation()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("previewImageSource")));
        QVERIFY(delegate.contains(QStringLiteral("previewAnimatedSource")));
        QVERIFY(delegate.contains(QStringLiteral("app.media.openWebUrl(card.p.url)")));
        QVERIFY(!delegate.contains(QStringLiteral("source: card.p.imageSource")));
        QVERIFY(!delegate.contains(QStringLiteral("openWebUrl(card.previewStatic")));
        QVERIFY(!delegate.contains(QStringLiteral("openWebUrl(card.previewAnimation")));
    }

    void messagesUseOneLeftAlignedSenderPresentation()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        QVERIFY(delegate.contains(QStringLiteral(
            "objectName: \"messagePresentationRow\"")));
        QVERIFY(delegate.contains(QStringLiteral(
            "readonly property real avatarGutterWidth: compactMode ? 8")));
        QVERIFY(delegate.contains(QStringLiteral(
            "readonly property bool showsIdentity: model.showSenderIdentity === true")));
        QVERIFY(delegate.contains(QStringLiteral(
            "mxc: model.senderAvatarMxc || \"\"")));
        QVERIFY(delegate.contains(QStringLiteral(
            "name: model.senderDisplayName || model.senderInitials")));
        QVERIFY(delegate.contains(QStringLiteral(
            "objectName: \"senderName\"")));
        QVERIFY(delegate.contains(QStringLiteral(
            "objectName: \"senderTimestamp\"")));

        // Current-user status may affect metadata and permissions, never
        // horizontal flow in the Modern rows. The design's Bubbles mode is
        // the ONLY colored-bubble path and it must stay gated to
        // direct-message timelines behind the message-layout setting.
        QVERIFY(!delegate.contains(QStringLiteral("Qt.AlignRight")));
        QVERIFY(delegate.contains(QStringLiteral(
            "readonly property bool bubbleMode: timelineLayout === 1 && isDirectRoom")));
        QVERIFY(delegate.contains(QStringLiteral(
            "? (model.isOwn === true ? AppTheme.ownBubble")));
        // The Modern/Compact bubble is transparent, FULL STOP — there is no
        // mention wash any more.
        //
        // 2026-08-21: the row wash was routed to mentionHighlight, which
        // Storm points at a danger-adjacent rose, so every message that
        // mentioned you was painted as a rounded red-ish box. That is the
        // user's report "tagging a person creates a red box arround it".
        // The mention edge bar below already existed as the deliberate
        // signal (bolt for "you", neutral for @room) and is enough on its
        // own; removing the fill also stops a reaction chip's translucent
        // pill compositing onto a tinted row.
        //
        // This assertion is inverted rather than deleted: a reintroduced
        // wash on this binding is the defect, and it must fail here.
        QVERIFY2(!delegate.contains(QStringLiteral("AppTheme.mentionHighlight")),
                 "the mention ROW WASH is back — mentionHighlight is the "
                 "badge's token and resolves to a danger-adjacent rose under "
                 "Storm, which is what drew a red box around mentions");
        QVERIFY(delegate.contains(QStringLiteral(
            "                       : \"transparent\"")));
        // The edge bar IS the mention signal now, so it has to be there.
        QVERIFY(delegate.contains(QStringLiteral("mentionBarVisible")));
        QVERIFY(delegate.contains(QStringLiteral(
            "? AppTheme.bolt : AppTheme.borderStrong")));
    }

    // MessageHtml's mention/link ink split is only real if QML actually
    // pushes the link colour. It gained the parameter in the 2026-08-21
    // round and NOTHING passed it, so for the whole round every external URL
    // and every mention of someone else rendered in the accent — under Storm,
    // in bolt yellow. A defaulted C++ parameter fails silently by design, so
    // the arity is pinned here.
    void mentionStyleIsPushedWithTheLinkInk()
    {
        const QString shell = read(QStringLiteral("MainScreen.qml"));
        QVERIFY(!shell.isEmpty());
        // Four arguments at BOTH push sites (timeline and thread model).
        const QRegularExpression call(QStringLiteral(
            "setMentionStyle\\(\\s*accent\\s*,\\s*soft\\s*,\\s*code\\s*,"
            "\\s*linkInk\\s*\\)"));
        QCOMPARE(shell.count(call), 2);
        QVERIFY2(shell.contains(QStringLiteral("AppTheme.link")),
                 "the link ink must come from the theme, not a literal");
        // ...and re-pushed when only the link ink moves. Several themes give
        // link and accent unrelated values, so an accent-only handler leaves
        // the models on the previous theme's link colour.
        QVERIFY2(shell.contains(QStringLiteral("function onLinkChanged()")),
                 "a theme change that moves only the link ink must re-push");
    }

    void continuationRowsStayCompactAndActionsFloat()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!delegate.isEmpty());
        QVERIFY(!pane.isEmpty());

        // Continuations must not retain the former unconditional 36px avatar
        // height or a permanent timestamp line below the body.
        QVERIFY(delegate.contains(QStringLiteral(
            "implicitHeight: root.showsIdentity ? 34 : bodyLabel.implicitHeight")));
        QVERIFY(delegate.contains(QStringLiteral(
            "objectName: \"continuationTimestamp\"")));
        // Shown only on a continuation row, only on hover. 2026-08-19:
        // now a Loader gate (`active:`) rather than `visible:` — this
        // Label's text is "" on a virtual row, and a text binding that
        // keeps producing the same empty string the item already holds
        // never reaches the line in QQuickText::setText that clears the
        // born-with ItemObservesViewport flag, which defeated Qt's
        // whole-tree pruning on every scroll. (Visibility is NOT the
        // mechanism — see the long note in MessageDelegate.qml.)
        QVERIFY(delegate.contains(QStringLiteral(
                    "active: !root.showsIdentity && rowHover.hovered"))
                || delegate.contains(QStringLiteral(
                    "visible: !root.showsIdentity && rowHover.hovered")));
        QVERIFY(delegate.contains(QStringLiteral("return \"\"")));
        QVERIFY(!delegate.contains(QStringLiteral(
            "return model.showSenderIdentity === true ? \"\" : ts")));

        // The toolbar overlays the unused right edge instead of taking a
        // RowLayout cell and narrowing the message column on hover.
        QVERIFY(delegate.contains(QStringLiteral("anchors.right: parent.right")));
        QVERIFY(delegate.contains(QStringLiteral("anchors.top: parent.top")));
        QVERIFY(pane.contains(QStringLiteral("spacing: 0")));
    }

    void wrappedBodiesHaveStableIncubationWidths()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(delegate.contains(QStringLiteral("bubble.width > 8")));
        QVERIFY(delegate.contains(QStringLiteral(": 560")));
        QVERIFY(pane.contains(QStringLiteral("available > 0 ? available : 640")));
        QVERIFY(delegate.contains(QStringLiteral("objectName: \"messageBody\"")));
    }

    void previewsAndMediaUseBoundedLeftAlignedColumns()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());

        const int mediaStart = delegate.indexOf(QStringLiteral("id: mediaBox"));
        const int bodyStart = delegate.indexOf(QStringLiteral("id: bodyLabel"),
                                               mediaStart);
        const int previewStart = delegate.indexOf(QStringLiteral("id: previewLoader"));
        const int metaStart = delegate.indexOf(QStringLiteral("id: metaRow"),
                                               previewStart);
        // The preview block ends at its NEXT SIBLING, which is no longer
        // metaRow: the upload progress bar was added between them, and it
        // fills width on purpose. Widening the window to swallow it would
        // make this guard fail on an element it was never about.
        const int uploadStart =
            delegate.indexOf(QStringLiteral("id: uploadProgressLoader"),
                             previewStart);
        const int previewEnd =
            (uploadStart > previewStart && uploadStart < metaStart)
                ? uploadStart : metaStart;
        QVERIFY(mediaStart >= 0 && bodyStart > mediaStart);
        QVERIFY(previewStart >= 0 && metaStart > previewStart);
        const QString mediaBlock = delegate.mid(mediaStart,
                                                bodyStart - mediaStart);
        const QString previewBlock = delegate.mid(previewStart,
                                                  previewEnd - previewStart);
        // …and the bar it makes room for really is the full-width one, so
        // the two are told apart deliberately rather than by luck.
        QVERIFY(uploadStart > previewStart);
        QVERIFY(delegate.mid(uploadStart, metaStart - uploadStart)
                    .contains(QStringLiteral("Layout.fillWidth: true")));

        QVERIFY(mediaBlock.contains(QStringLiteral(
            "Layout.alignment: Qt.AlignLeft")));
        QVERIFY(mediaBlock.contains(QStringLiteral(
            "Layout.maximumWidth: bubble.width")));
        QVERIFY(!mediaBlock.contains(QStringLiteral("Layout.fillWidth: true")));
        QVERIFY(!mediaBlock.contains(QStringLiteral("anchors.right: parent.right")));
        QVERIFY(previewBlock.contains(QStringLiteral(
            "Layout.alignment: Qt.AlignLeft")));
        QVERIFY(previewBlock.contains(QStringLiteral(
            "item ? item.implicitWidth : 400")));
        QVERIFY(!previewBlock.contains(QStringLiteral("Layout.fillWidth: true")));

        // Video cards: responsive 72% column cap with hard bounds and the
        // control-surface floor (the old flat 360/320 caps clipped portrait
        // controls); file cards keep a bounded width.
        QVERIFY(delegate.contains(QStringLiteral(
            "var cap = Math.min(560, Math.max(280, bubble.width * 0.72))")));
        QVERIFY(delegate.contains(QStringLiteral(
            "readonly property real minControlW: Math.min(260, bubble.width)")));
        QVERIFY(delegate.contains(QStringLiteral(
            "implicitWidth: Math.min(340, bubble.width)")));
    }

    void directGifUsesInlineMediaRenderer()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());

        QVERIFY(delegate.contains(QStringLiteral(
            "root.preview.isDirectMedia === true")));
        QVERIFY(delegate.contains(QStringLiteral(
            "? directMediaPreviewComponent")));
        const int start = delegate.indexOf(QStringLiteral(
            "id: directMediaPreviewComponent"));
        const int genericStart = delegate.indexOf(QStringLiteral(
            "id: linkPreviewComponent"), start);
        QVERIFY(start >= 0 && genericStart > start);
        const QString directBlock = delegate.mid(start, genericStart - start);

        QVERIFY(directBlock.contains(QStringLiteral(
            "objectName: \"directMediaPreview\"")));
        QVERIFY(directBlock.contains(QStringLiteral(
            "readonly property real maxWidth: Math.min(360, bubble.width - 8)")));
        QVERIFY(directBlock.contains(QStringLiteral("AnimatedImage {")));
        QVERIFY(directBlock.contains(QStringLiteral(
            "onClicked: app.media.openWebUrl(directMedia.p.url)")));
        QVERIFY(!directBlock.contains(QStringLiteral("card.p.siteName")));
        QVERIFY(!directBlock.contains(QStringLiteral("card.p.description")));
        QVERIFY(!directBlock.contains(QStringLiteral("card.p.host")));
        QVERIFY(!directBlock.contains(QStringLiteral(
            "color: AppTheme.accent\n")));
    }

    void messageContentAndActionsRemainInteractive()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("TextEdit {\n                        id: bodyLabel")));
        QVERIFY(delegate.contains(QStringLiteral("readOnly: true")));
        QVERIFY(delegate.contains(QStringLiteral("selectByMouse: true")));
        // Links stay interactive through the controlled web-open path;
        // mention links route to the member profile instead.
        QVERIFY(delegate.contains(QStringLiteral("app.media.openWebUrl(link)")));
        QVERIFY(delegate.contains(QStringLiteral("mention:")));
        QVERIFY(delegate.contains(QStringLiteral("id: replyBox")));
        // v0.7.1: the crash-fix round renamed this delegate's own (thread-
        // panel-only; clip is false there, a real ListView) bar to
        // threadActionBar — the room timeline's equivalent is now the ONE
        // shared instance in TimelinePane.qml (id: sharedMessageActionBar,
        // see MessageActionBarFitTest.cpp).
        QVERIFY(delegate.contains(QStringLiteral("id: messageActionBar")));
        QVERIFY(delegate.contains(QStringLiteral("id: previewLoader")));
        QVERIFY(delegate.contains(QStringLiteral("id: imageComponent")));
        // v0.7: reactions open the view-shared picker via the snapshotted
        // event id; the delegate owns no picker popup of its own.
        QVERIFY(delegate.contains(
            QStringLiteral("openReactionPickerFor(root.eventIdForActions()")));
        QVERIFY(!delegate.contains(QStringLiteral("id: reactionPicker")));
        QVERIFY(delegate.contains(QStringLiteral("app.composer.beginReply")));
        QVERIFY(delegate.contains(QStringLiteral("app.composer.beginEdit")));
        QVERIFY(delegate.contains(QStringLiteral("acceptedButtons: Qt.RightButton")));
        QVERIFY(delegate.contains(QStringLiteral("Qt.Key_Menu")));
        QVERIFY(delegate.contains(QStringLiteral("id: moreMenu")));
        QVERIFY(delegate.contains(QStringLiteral("Copy message link")));
        QVERIFY(delegate.contains(QStringLiteral("View details")));
        QVERIFY(delegate.contains(QStringLiteral("messageDetailsDialog")));
        QVERIFY(delegate.contains(QStringLiteral("root.menuEventId")));
    }

    // A send stuck in "sending…" is now cancellable, and a media send draws
    // real upload progress. Three properties keep both honest.
    void stuckSendsCanBeCancelledAndMediaUploadsShowRealProgress()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());

        // 1. The cancel offer is the MODEL's answer, never the status alone
        //    — a backend with no send queue has nothing to abort — and it
        //    covers Failed as well as Sending, because a failed send is
        //    still a queued item the user may not want any more.
        QVERIFY(delegate.contains(QStringLiteral("objectName: \"cancelSendLink\"")));
        QVERIFY(delegate.contains(QStringLiteral("root.canCancelSendAt(index)")));
        QVERIFY(delegate.contains(
            QStringLiteral("root.timelineModel.canCancelSend(")));
        QVERIFY(delegate.contains(
            QStringLiteral("root.timelineModel.cancelSend(")));

        // 2. `index` is read inside the built tree, never in a root-level
        //    creation-time binding (the poisoned-context family, 30ee39b).
        //    canCancelSendAt is therefore a function, not a property.
        QVERIFY(delegate.contains(
            QStringLiteral("function canCancelSendAt(viewRow) {")));

        // 3. -1 is "uploading, extent unknown" and MUST render as the
        //    indeterminate sweep. A 0% bar there claims a measurement that
        //    does not exist and would sit at zero for a whole small upload.
        QVERIFY(delegate.contains(
            QStringLiteral("objectName: \"uploadProgressLoader\"")));
        QVERIFY(delegate.contains(
            QStringLiteral("indeterminate: root.uploadProgress < 0")));
        // The normalisation exists exactly once, so a fixture model without
        // the role reads as unknown instead of assigning undefined.
        QVERIFY(delegate.contains(QStringLiteral(
            "model.uploadProgress === undefined ? -1 : model.uploadProgress")));
        // The bar belongs to media rows: a text send has no upload, and a
        // sweep under every outgoing line would be noise.
        QVERIFY(delegate.contains(QStringLiteral("&& root.mediaRowBody")));
    }

    // BOTH composers must claim the editor ShortcutOverride, and both must
    // get the id from the registry rather than from a list of their own.
    //
    // THE DEFECT THIS PINS: the thread composer had no override at all, so
    // Ctrl+B inside a thread reply was not Bold -- it reached the window and
    // toggled the conversation list mid-sentence. The room composer had
    // claimed its overrides since the design shell landed, which is why the
    // same key did two different things depending on which box had focus.
    void bothComposersClaimTheEditorOverrideThroughTheRegistry()
    {
        const QString bar = read(QStringLiteral("MessageComposerBar.qml"));
        const QString thread = read(QStringLiteral("ThreadPanel.qml"));
        QVERIFY(!bar.isEmpty());
        QVERIFY(!thread.isEmpty());

        for (const QString &src : { bar, thread }) {
            QVERIFY2(src.contains(QStringLiteral("Keys.onShortcutOverride")),
                     "a composer that never accepts the override can only "
                     "watch its format keys reach the window");
            QVERIFY2(src.contains(QStringLiteral("editorActionForKey")),
                     "the id must come from the registry, which is what "
                     "carries the EditorContext flag");
        }

        // The registry is the ONE place that knows which actions are
        // editor-context. A composer re-listing them is the duplicate that
        // let the two boxes drift apart in the first place.
        for (const QString &src : { bar, thread }) {
            QVERIFY2(!src.contains(QStringLiteral("\"composer.italic\"")),
                     "hand-listed editor ids are back; a seventh editor "
                     "shortcut would work in one composer and not the other");
        }

        // The thread box must APPLY it, not merely swallow it: accepting the
        // override without handling the press would turn Ctrl+B into a key
        // that does nothing at all, which is worse than the original bug.
        QVERIFY(thread.contains(QStringLiteral("applyThreadFormat")));
    }

    // Every surface that renders BRIDGE bytes must hear mediaRetryable.
    //
    // THE DEFECT THIS PINS: a transient failure (a timeout, a dropped fetch)
    // marks the key, and 60 s later MediaBridge sweeps the mark and emits
    // mediaRetryable so the surface can ask again. Avatar.qml and
    // MediaListThumbnail.qml listened. The THREE bridge-backed surfaces in
    // MessageDelegate — the image, the sticker and the video box — did not,
    // so a failed image sat on its fallback until something rebuilt the
    // binding, which in a quiet room means restarting the app. Reported as
    // "small images in relatively inactive rooms get stuck loading forever;
    // it gets fixed when you restart".
    void everyBridgeBackedSurfaceHearsTheRetryableSweep()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());

        // A surface is bridge-backed exactly when it marks itself failed on
        // mediaFetchFailed. Derived from the source rather than hard-coded,
        // so a FOURTH such surface is covered without editing this test.
        const int failedHandlers =
            delegate.count(QStringLiteral("function onMediaFetchFailed("));
        QVERIFY2(failedHandlers >= 3,
                 qPrintable(QStringLiteral("expected the bridge-backed "
                                           "surfaces, found %1")
                                .arg(failedHandlers)));
        const int retryHandlers =
            delegate.count(QStringLiteral("function onMediaRetryable("));
        QCOMPARE(retryHandlers, failedHandlers);

        // And the two surfaces that always had it keep it.
        for (const QString &file : { QStringLiteral("Avatar.qml"),
                                     QStringLiteral("MediaListThumbnail.qml") }) {
            const QString src = read(file);
            QVERIFY(!src.isEmpty());
            QVERIFY2(src.contains(QStringLiteral("onMediaRetryable")),
                     qPrintable(file + QStringLiteral(" lost its recovery "
                                                      "channel")));
        }
    }

};

QTEST_MAIN(QmlBindingContractTest)
#include "QmlBindingContractTest.moc"
