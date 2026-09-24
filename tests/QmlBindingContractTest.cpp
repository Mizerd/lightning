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

    // One text-rendering element and the properties declared at ITS OWN brace
    // depth. Depth matters: a `Label { ... Label { textFormat: ... } }` must
    // not let the inner declaration satisfy a check on the outer one, and a
    // fixed character window after a name is defeated by any comment added
    // inside the block — a shape that has cost this suite four cases.
    struct TextElement {
        int line = 0;
        QStringList properties;

        // The value of `name:` as written, or an empty string. Multi-line
        // expressions (the ternaries this codebase writes for display names)
        // are joined, so the whole expression is matched, not its first line.
        QString property(const QString &name) const
        {
            const QString prefix = name + QLatin1Char(':');
            for (const QString &p : properties) {
                if (p.startsWith(prefix))
                    return p;
            }
            return {};
        }
    };

    static QList<TextElement> textElements(const QString &source)
    {
        // Every element that RENDERS `text:` AND CAN CARRY A FORMAT.
        //
        // MenuItem and AppMenuItem are deliberately NOT here, and adding them
        // was a mistake that cost a broken build: an AbstractButton has a
        // `text` property but no `textFormat`, so `textFormat:` on one is a
        // LOAD-TIME error that makes the whole component unavailable and
        // cascades into every parent — the same shape as assigning
        // `font.families`, and just as invisible to qmlformat.
        //
        // A menu row still renders remote text, and that is handled where it
        // can be: AppMenuItem's own contentItem Label declares PlainText once,
        // so every menu row in the app inherits it and no call site needs to
        // (or is able to) repeat it.
        static const QRegularExpression opener(QStringLiteral(
            "\\b(Label|Text|AppLabel|ToolTip)\\s*\\{"));
        QList<TextElement> found;
        QRegularExpressionMatchIterator it = opener.globalMatch(source);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const int open = m.capturedEnd() - 1;
            int depth = 0;
            int end = open;
            while (end < source.size()) {
                const QChar c = source.at(end);
                if (c == QLatin1Char('{')) {
                    ++depth;
                } else if (c == QLatin1Char('}')) {
                    if (--depth == 0)
                        break;
                }
                ++end;
            }
            TextElement element;
            element.line = source.left(m.capturedStart()).count(QLatin1Char('\n')) + 1;
            // Collect depth-1 lines, joining continuations so a property's
            // whole expression is one entry.
            depth = 0;
            QString current;
            for (int i = open; i <= end && i < source.size(); ++i) {
                const QChar c = source.at(i);
                if (c == QLatin1Char('{')) {
                    ++depth;
                    if (depth > 1) {
                        // A BLOCK BINDING (`text: { ... }`) opens a brace on
                        // the property's own line. The walker used to swallow
                        // that line into the nested scope and record nothing,
                        // so the element was skipped ENTIRELY and counted as
                        // examined by nobody — 38 elements tree-wide were
                        // invisible, four of which render remote text and
                        // would have failed. Flush the pending line as a
                        // property first, then keep collecting.
                        if (depth == 2 && !current.trimmed().isEmpty()) {
                            const QString opened = current.trimmed();
                            static const QRegularExpression newProp(
                                QStringLiteral("^[A-Za-z_][A-Za-z0-9_.]*\\s*:"));
                            if (newProp.match(opened).hasMatch()) {
                                element.properties << opened;
                                current.clear();
                                continue;
                            }
                        }
                        current.append(c);
                    }
                    continue;
                }
                if (c == QLatin1Char('}')) {
                    --depth;
                    if (depth >= 1)
                        current.append(c);
                    continue;
                }
                if (c == QLatin1Char('\n')) {
                    if (depth == 1 && !current.trimmed().isEmpty()) {
                        const QString trimmed = current.trimmed();
                        // A line that opens a new property starts a new entry;
                        // anything else continues the previous one.
                        static const QRegularExpression newProperty(
                            QStringLiteral(
                                "^([A-Za-z_][A-Za-z0-9_.]*\\s*:"
                                "|readonly\\b|property\\b|function\\b"
                                "|signal\\b|component\\b|required\\b"
                                "|on[A-Z][A-Za-z0-9_]*\\s*:)"));
                        if (!element.properties.isEmpty()
                            && !newProperty.match(trimmed).hasMatch()
                            && !trimmed.startsWith(QLatin1String("//"))) {
                            element.properties.last().append(QLatin1Char(' '));
                            element.properties.last().append(trimmed);
                        } else {
                            element.properties << trimmed;
                        }
                    }
                    current.clear();
                    continue;
                }
                if (depth >= 1)
                    current.append(c);
            }
            found << element;
        }
        return found;
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

        // AND EVERY OTHER DELIBERATE QUIT MUST ANNOUNCE ITSELF THE SAME WAY.
        // Ctrl+Q was the only caller that satisfied this rule. Applying an
        // update quits from C++, close-to-tray refused that close, the quit
        // was aborted and the update helper sat waiting until it timed out --
        // a rule understood in one place that the other caller could not
        // satisfy. AppController announces the intent now, and the window
        // answers it here.
        QVERIFY2(main.contains(QStringLiteral("onApplicationQuitIntended")),
                 "the window must honour a deliberate quit from C++, or "
                 "close-to-tray silently vetoes an update");
        const QString intended = bracedBody(
            main, main.indexOf(QStringLiteral("onApplicationQuitIntended")));
        QVERIFY2(intended.contains(QStringLiteral("quitRequested = true")),
                 "the announced quit must set the same flag Ctrl+Q sets");
    }

    // 2026-09-03 tester report: "clicking a notification in the bell menu
    // minimizes Lightning."
    //
    // It was not a minimize. QWindow::show() forces the NORMAL state, so
    // calling it on a window that is already on screen and MAXIMIZED
    // un-maximizes it — the frame snaps back to the small remembered size,
    // which is what that looks like — and onVisibilityChanged then persists
    // saveWindowMaximized(false), so the user's window preference is rewritten
    // as a side effect of a click on an activity row.
    //
    // The tray path had already learned this and used `visible = true`; the
    // notification path had not. Both now share raiseIntoView(), and this
    // pins that neither of them may reach for show() again.
    void bringingTheWindowForwardNeverForcesTheNormalState()
    {
        const QString main = read(QStringLiteral("Main.qml"));
        QVERIFY(!main.isEmpty());

        const int helper = main.indexOf(
            QStringLiteral("function raiseIntoView()"));
        QVERIFY2(helper > 0,
                 "raiseIntoView() is gone; the two raise paths have diverged "
                 "again and one of them will reach for show()");
        const QString body = bracedBody(main, helper);
        QVERIFY(!body.isEmpty());
        QVERIFY2(!body.contains(QStringLiteral("show()")),
                 "raiseIntoView() calls show(), which forces Window.Windowed "
                 "and un-maximizes a maximized window");
        // Closed to the tray comes back through `visible`, which restores the
        // state the window actually had.
        QVERIFY(body.contains(QStringLiteral("Window.Hidden")));
        QVERIFY(body.contains(QStringLiteral("window.visible = true")));
        // A genuinely minimized window is restored to the state it was in,
        // not unconditionally to Windowed.
        QVERIFY(body.contains(QStringLiteral("Window.Minimized")));
        QVERIFY(body.contains(QStringLiteral("lastOnScreenVisibility")));
        QVERIFY(body.contains(QStringLiteral("window.raise()")));
        QVERIFY(body.contains(QStringLiteral("window.requestActivate()")));

        // Both callers go through it, and neither does its own thing.
        for (const QString &handler :
             { QStringLiteral("function onNotificationOpenRequested("),
               QStringLiteral("function onTrayShowRequested(") }) {
            const int at = main.indexOf(handler);
            QVERIFY2(at > 0, qPrintable(handler));
            const QString h = bracedBody(main, at);
            QVERIFY2(!h.isEmpty(), qPrintable(handler));
            QVERIFY2(h.contains(QStringLiteral("raiseIntoView()")),
                     qPrintable(handler + QStringLiteral(
                         " must raise through raiseIntoView()")));
            QVERIFY2(!h.contains(QStringLiteral("window.show()")),
                     qPrintable(handler + QStringLiteral(
                         " calls window.show() again")));
        }

        // And the pre-minimize state is actually recorded, or the restore
        // above has nothing to restore.
        const int vis = main.indexOf(QStringLiteral("onVisibilityChanged:"));
        QVERIFY(vis > 0);
        const QString visBody = bracedBody(main, vis);
        QVERIFY(visBody.contains(
            QStringLiteral("window.lastOnScreenVisibility = window.visibility")));
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

        // THREE kinds of progress since 2026-09-16, not two. The paragraph
        // above says "a page that added nothing gets the small one" — and a
        // page the timeline filter EMPTIED adds nothing while having walked
        // twenty real events, so the small bound (8) was spent on the one
        // case that most needed the generous one. A DM whose recent history
        // is MatrixRTC churn opened with one message over a blank viewport
        // and stayed there until the reader scrolled: "in this room only a
        // single image loads and I have to scroll up for anything else to
        // appear."
        //
        // Pinned here as well as behaviourally because the distinction is
        // invisible in the code that reads it: zero rows and zero pixels
        // either way, and only the controller's counter tells them apart.
        QVERIFY2(body.contains(QStringLiteral("viewportFillLastEmptyPages")),
                 "nothing compares the controller's completed-empty-page "
                 "counter across attempts, so a page the filter emptied is "
                 "indistinguishable from a dispatch that went nowhere and "
                 "spends the small no-progress bound meant for the latter");
        QVERIFY2(body.contains(QStringLiteral("maxEmptyFillPages")),
                 "a run of filtered history has no bound of its own");

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
    void imageViewerClosesInstantlyOutsideThePictureAndZoomsOnIt()
    {
        const QString viewer = read(QStringLiteral("ImageViewerOverlay.qml"));
        QVERIFY(!viewer.isEmpty());

        // THIS CONTRACT CHANGED ON 2026-09-17, DELIBERATELY AND ON REQUEST.
        //
        // It used to require TWO `onTapped: viewer.close()` — the scrim and
        // the picture — because a click anywhere closed. The maintainer asked
        // for the gesture model people arrive with instead: the picture zooms
        // at the point clicked, and everything around it still closes.
        //
        // The two properties the original contract existed to protect are
        // UNCHANGED and still asserted below, because they are what the live
        // feedback was actually about:
        //   * closing never waits out the platform's double-click interval —
        //     there is still no double-tap handler anywhere;
        //   * closing never requires the X button — the scrim closes, and so
        //     does the margin of holder around a fitted picture.
        // What was given up is closing by clicking the picture itself, which
        // is the trade that was asked for.

        // The scrim's tap: exactly one, undelayed.
        QCOMPARE(viewer.count(QStringLiteral("onTapped: viewer.close()")), 1);

        // The picture's tap zooms at the pointer...
        QVERIFY2(viewer.contains(QStringLiteral("viewer.toggleZoomAt")),
                 "the picture no longer zooms on click");
        // ...but still closes when the tap lands OUTSIDE the drawn image.
        // `imageHolder` is Math.max(flick.width, ...), so it fills the
        // viewport whatever the picture's size, and that margin is scrim as
        // far as the user is concerned. Without this band check a click in
        // the empty space around a small image zooms instead of closing —
        // which is exactly what was reported the first time this shipped.
        // Keyed on the COMPARISON, not on an expression the file contains
        // four times over. The first version of this assertion looked for
        // `viewer.baseWidth * viewer.zoom`, which also appears in
        // imageHolder's size and in both image widths — so deleting the whole
        // band check left it green. A test written to protect a fix must fail
        // when the fix is removed, and that one could not.
        QVERIFY2(viewer.contains(QStringLiteral("x < left || x > left + iw")),
                 "the image tap has no band check, so the margin around a "
                 "fitted picture is still part of the picture's hit target "
                 "and a click there zooms instead of closing");

        // Never a double-tap: that is the delay the instant close cannot pay.
        QVERIFY(!viewer.contains(QStringLiteral("onDoubleTapped")));
        QVERIFY(!viewer.contains(QStringLiteral("exclusiveSignals")));

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

    // EVERY text binding that is not one of our own literals must declare its
    // format. A QML Label defaults to Text.AutoText, which runs
    // Qt::mightBeRichText() and hands anything containing a known tag to the
    // rich-text engine — so a room name, topic or display name of
    // `<img src="http://attacker/x">` makes every viewer's client fetch that
    // URL on sight, past the media bridge §6 requires everything to go
    // through, and an unsolicited invite is enough to put a stranger's room
    // name in front of someone.
    //
    // THE RULE IS INVERTED ON PURPOSE, and this is its third shape. It began
    // as a list of the six sinks two audits happened to look at. A sweep the
    // same day found twenty-eight more. A reviewer then found the allowlist
    // still blind three ways: one property hop defeated it (`text: root._label`
    // aliasing a display name), it was case-sensitive so `senderDisplayName`
    // and `channelName` slipped through, and the parser silently SKIPPED every
    // element using a block binding (`text: { ... }`) — 38 of them, four
    // rendering remote text.
    //
    // Guessing which identifiers are attacker-controlled is the part that
    // keeps failing, so it is no longer guessed. Anything that is not a bare
    // `qsTr("literal")` (or a plain string literal, or a lookup on our own
    // theme/settings singletons) must say what it is, and the exemptions are
    // listed here where they can be argued with.
    void unsanitizedServerTextIsAlwaysPlainText()
    {
        // WHAT COUNTS AS TEXT WE DO NOT CONTROL.
        //
        // Inverting the rule entirely (require a format on every non-literal
        // binding) was tried and rejected: it flags 434 elements, almost all
        // of them counts, timestamps and durations, and a numeric label
        // rendering AutoText is harmless. The value of the rule is in what it
        // catches, and burying six real sinks in 434 rows destroys that.
        //
        // So it stays identifier-based, with the three ways the first version
        // was blind closed:
        //   * CASE-INSENSITIVE. `senderDisplayName`, `ownerDisplayName`,
        //     `panelRoomName` and `channelName` all escaped a \b-anchored
        //     case-sensitive list that only knew `displayName` and `.name`.
        //   * ONE PROPERTY HOP is followed. `text: root._label` says nothing
        //     on its own, so when a binding is a bare property reference the
        //     sweep looks that property up in the same file and judges what
        //     IT is bound to. That is how the call tiles' `_label` and
        //     `primaryLabel` aliases of a display name hid.
        //   * BLOCK BINDINGS are now parsed at all (see textElements).
        static const QRegularExpression remotelyChosen(
            QStringLiteral(
                "(topic|roomname|spacename|displayname|sendername|membername"
                "|authorname|channelname|dmname|targetname|inviter|invitee"
                "|reactionkey|statustext|statusmessage|alias|packname"
                "|stickername|devicename|sessionname|filename|attribution"
                "|\\bsender\\b|\\buserid\\b|\\bbody\\b|\\bpreview\\b"
                "|\\bname\\b|\\blabel\\b|\\bsubtitle\\b)"),
            QRegularExpression::CaseInsensitiveOption);
        // Our own literals, which cannot carry anything remote.
        static const QRegularExpression ourOwnText(QStringLiteral(
            "^text\\s*:\\s*(qsTr\\(.*\\)|qsTrId\\(.*\\)"
            "|\"[^\"]*\"|'[^']*')\\s*$"));
        // A binding that is nothing but a property reference, e.g.
        // `text: root._label` or `text: tile.primaryLabel`.
        static const QRegularExpression bareAlias(QStringLiteral(
            "^text\\s*:\\s*[A-Za-z_][A-Za-z0-9_]*\\.([A-Za-z_][A-Za-z0-9_]*)\\s*$"));

        // What `name` is bound to elsewhere in this file, for the one hop.
        const auto aliasTarget = [](const QString &source, const QString &name) {
            const QRegularExpression decl(
                QStringLiteral("\\b(?:readonly\\s+)?property\\s+\\w+\\s+")
                + QRegularExpression::escape(name)
                + QStringLiteral("\\s*:([^\\n]*)"));
            const QRegularExpressionMatch m = decl.match(source);
            return m.hasMatch() ? m.captured(1) : QString();
        };

        int checked = 0;
        int exempt = 0;
        QStringList offenders;
        const QDir dir(QStringLiteral(QML_DIR));
        const QStringList files =
            dir.entryList(QStringList{ QStringLiteral("*.qml") }, QDir::Files);
        QVERIFY2(files.size() > 20, "the qml directory did not enumerate");

        for (const QString &name : files) {
            const QString source = read(name);
            if (source.isEmpty())
                continue;
            for (const TextElement &element : textElements(source)) {
                const QString binding = element.property(QStringLiteral("text"));
                if (binding.isEmpty())
                    continue;
                if (ourOwnText.match(binding).hasMatch()) {
                    ++exempt;
                    continue;
                }
                bool hostile = remotelyChosen.match(binding).hasMatch();
                if (!hostile) {
                    // One hop: resolve a bare alias to what it is bound to.
                    const QRegularExpressionMatch alias = bareAlias.match(binding);
                    if (alias.hasMatch()) {
                        const QString target =
                            aliasTarget(source, alias.captured(1));
                        hostile = !target.isEmpty()
                            && remotelyChosen.match(target).hasMatch();
                    }
                }
                if (!hostile) {
                    ++exempt;
                    continue;
                }
                ++checked;
                // Its OWN declaration, at its own brace depth — a nested
                // child's textFormat must never be credited to its parent —
                // and it must say PLAIN. Requiring only that the property
                // EXISTS would let a future `textFormat: Text.RichText` on a
                // remote-text label pass the very check written to stop it.
                const QString declared =
                    element.property(QStringLiteral("textFormat"));
                // ARGUED EXEMPTION: a `highlighted*()` helper deliberately
                // RETURNS markup — it wraps the matched substring in a
                // <font> tag so a search hit is visible — and it escapes the
                // untrusted name FIRST. Such a binding must be rich text, so
                // demanding PlainText here would break the highlight it
                // exists for. The exemption is conditional on the helper
                // actually escaping: if someone writes a highlighter that
                // does not, this stops exempting it.
                static const QRegularExpression highlighter(
                    QStringLiteral("\\b(highlighted[A-Za-z0-9_]*)\\s*\\("));
                const QRegularExpressionMatch helper =
                    highlighter.match(binding);
                if (helper.hasMatch()) {
                    const QRegularExpression definition(
                        QStringLiteral("function\\s+")
                        + QRegularExpression::escape(helper.captured(1))
                        + QStringLiteral("\\s*\\([^)]*\\)\\s*\\{"));
                    const QRegularExpressionMatch defined =
                        definition.match(source);
                    if (defined.hasMatch()
                        && source.mid(defined.capturedEnd(), 400)
                               .contains(QStringLiteral("escapeHtml("))) {
                        ++exempt;
                        continue;
                    }
                }
                if (!declared.contains(QStringLiteral("PlainText"))) {
                    offenders << QStringLiteral("%1:%2  %3")
                                     .arg(name)
                                     .arg(element.line)
                                     .arg(binding.left(72));
                }
            }
        }
        // A sweep that matched nothing passes vacuously, which is worse than
        // no sweep: it reads as coverage. Both halves are asserted, so a
        // parser regression that stops FINDING elements fails too.
        QVERIFY2(checked > 40,
                 qPrintable(QStringLiteral("the sweep only examined %1 bindings; "
                                           "it has stopped finding them")
                                .arg(checked)));
        QVERIFY2(exempt > 200,
                 qPrintable(QStringLiteral("only %1 bindings were recognised as "
                                           "our own literals; the exemption "
                                           "pattern has stopped matching")
                                .arg(exempt)));
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "%1 element(s) render text we do not control without "
                     "declaring a PLAIN text format, so markup in it will be "
                     "rendered:\n  %2")
                                .arg(offenders.size())
                                .arg(offenders.join(QStringLiteral("\n  ")))));
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
        // 2026-09-03: ONE button per composer opens the pair. GIFs and
        // stickers are one window with two tabs now, so a per-kind button no
        // longer exists on either surface.
        QVERIFY(thread.contains(QStringLiteral("threadMediaButton")));
        QVERIFY2(!thread.contains(QStringLiteral("threadGifButton")),
                 "the separate thread GIF button is back; the merged picker "
                 "has one entry point");
        QVERIFY(room.contains(QStringLiteral("composerMediaButton")));
        QVERIFY2(!room.contains(QStringLiteral("composerGifButton")),
                 "the separate room GIF button is back");
        // Both pickers offer the strip, and both ask the HOST to swap: a
        // picker that closed and opened its sibling itself would have to know
        // its own anchor item and its host's other picker.
        for (const QString &host : { room, thread }) {
            QVERIFY(host.contains(QStringLiteral("offerKindTabs")));
            QVERIFY(host.contains(QStringLiteral("onKindRequested")));
        }

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
        // THE CARD SHARES ITS ROW WITH THE "edited" MARKER, so it must be
        // bounded by what is LEFT of the row and not only by its own
        // implicitWidth. Seen live on the packaged flatpak 2026-09-13: edit a
        // message that is a thread root and the card ran off the right edge
        // of the bubble, clipping the last characters of its own timestamp.
        // Bounded to the Loader's own block -- ending at its `sourceComponent`
        // rather than at a blank line -- so this cannot pass by picking up a
        // Layout line from somewhere else in a 5,000-line file.
        const int cardLoaderAt = delegate.indexOf(QStringLiteral(
            "active: model.isThreadRoot === true"));
        QVERIFY2(cardLoaderAt >= 0, "the thread-card Loader gate is gone");
        const int compAt = delegate.indexOf(
            QStringLiteral("sourceComponent: ThreadSummaryCard {"), cardLoaderAt);
        QVERIFY2(compAt > cardLoaderAt,
                 "could not bound the thread-card Loader block");
        const QString cardLoader = delegate.mid(cardLoaderAt, compAt - cardLoaderAt);
        QVERIFY2(cardLoader.contains(QStringLiteral("Layout.fillWidth: true")),
                 qPrintable(QStringLiteral(
                     "the thread summary card is not bounded by the space left "
                     "in its row, so an edited thread root pushes it off the "
                     "right edge of the bubble:\n") + cardLoader));
        QVERIFY2(cardLoader.contains(QStringLiteral("Layout.maximumWidth:")),
                 "fillWidth without a maximum would stretch the card to the "
                 "full bubble width on every thread root");
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
    // Source text with COMMENTS REMOVED — whole-line `//`, trailing `//` and
    // `/* */` spans. Both trust guards below assert the ABSENCE of an
    // identifier, and an absence assertion over raw source is defeated by the
    // very comments that explain why the identifier must not be there. The
    // `/* */` half is not decoration either: the AppController fix leaves
    // `bool /*deviceCrossSigned*/,` in the parameter list, on a line that does
    // not start with `//`, so without it the bare-identifier assertion would
    // false-fail on the fixed code and the only way to make it pass would be
    // the weaker parenthesised form — which misses
    // `else if (deviceCrossSigned || deviceVerified)`.
    static QString withoutComments(const QString &source)
    {
        QString out;
        const auto lines = source.split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            if (line.trimmed().startsWith(QLatin1String("//")))
                continue;
            const int slashes = line.indexOf(QLatin1String("//"));
            out += (slashes >= 0 ? line.left(slashes) : line) + QLatin1Char('\n');
        }
        // Block comments last, so a `/* */` spanning lines is caught whole.
        int open = out.indexOf(QLatin1String("/*"));
        while (open >= 0) {
            const int close = out.indexOf(QLatin1String("*/"), open + 2);
            if (close < 0)
                break;
            out.remove(open, close + 2 - open);
            open = out.indexOf(QLatin1String("/*"), open);
        }
        return out;
    }

    void sessionsCardIsHonest()
    {
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(settings.contains(QStringLiteral(
            "onClicked: app.refreshSessionDevices()")));
        // v0.9 (phase 9): the list is FILTERED (all / this device / verified /
        // not verified), but the Repeater's source is still the authoritative
        // app.sessionDevices — never a copy the UI could drift from.
        QVERIFY(settings.contains(QStringLiteral("var all = app.sessionDevices")));
        QVERIFY(settings.contains(QStringLiteral("var f = sessionFilter.current")));
        for (const char *branch : { "f === \"current\" && d.isCurrent === true",
                                    "f === \"verified\" && d.crossSigned === true",
                                    "f === \"unverified\"" })
            QVERIFY2(settings.contains(QLatin1String(branch)), branch);
        QVERIFY(settings.contains(QStringLiteral("This session")));
        QVERIFY(settings.contains(QStringLiteral("Not verified")));

        // NO TRUST BINDING MAY READ `verified`, and this is asserted as an
        // ABSENCE because the defect it guards was a binding that existed and
        // read the wrong field, which a presence check cannot see.
        //
        // `verified` is `Device::is_verified()`, and matrix-sdk marks our own
        // device locally trusted the moment it creates it
        // (machine/mod.rs:350), so for the row badged "This session" it is a
        // CONSTANT TRUE. A round on 2026-09-20 bound the chip, both filters
        // and the rollup to it; measured live, a fresh login with every
        // cross-signing key Missing was badged green "Verified".
        //
        // Follow-up, deliberately not done and why this assertion will need
        // relaxing when it is: for rows that are NOT the current session,
        // is_verified() IS the better flag, because it also catches a device
        // verified by SAS without cross-signing. Doing that properly means
        // `isCurrent ? crossSigned : verified`, at which point this becomes
        // "no UNCONDITIONAL read of verified".
        QVERIFY2(!withoutComments(settings).contains(QStringLiteral("modelData.verified"))
                 && !withoutComments(settings).contains(QStringLiteral("d.verified")),
                 "a trust binding reads verified, which is a constant true "
                 "for our own device; it must read crossSigned");
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

    // THE FOURTH TRUST SURFACE IS COMPOSED IN C++ AND ONLY DISPLAYED BY QML,
    // WHICH IS HOW IT SURVIVED A ROUND THAT FIXED THE OTHER THREE.
    //
    // `app.sessionTrustState` is a QString built in AppController's
    // `ownDeviceStatusUpdated` handler and rendered as a StatusChip in two
    // places (the account identity card, and the Sessions "Current session"
    // card directly above the device list). Every assertion in the case above
    // reads SettingsScreen.qml and none of them can see it, so that chip went
    // on saying "Verified" from `is_cross_signed_by_owner()` while the list
    // beside it said "Not verified" about the same device.
    //
    // Scans the handler's own extent, not the whole file, so an unrelated
    // mention of the flag elsewhere in AppController cannot make this pass or
    // fail by accident — and asserts the extent is real before concluding
    // anything from it.
    void theSessionTrustChipReadsCrossSigningNotTheAlwaysTrueFlag()
    {
        QFile file(QStringLiteral(LIGHTNING_SRC_DIR "/app/AppController.cpp"));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 "AppController.cpp not readable");
        const QString src = QString::fromUtf8(file.readAll());

        const int start = src.indexOf(QStringLiteral(
            "&RustSdkMatrixClient::ownDeviceStatusUpdated"));
        QVERIFY2(start > 0, "the ownDeviceStatusUpdated handler was not found");
        const int end = src.indexOf(QStringLiteral("\n        });"), start);
        QVERIFY2(end > start, "the handler's end was not found");
        const QString handler = src.mid(start, end - start);
        // BRACKET THE WHOLE if/else CHAIN, not just any part of it. The
        // delimiter above is positional, so a future nested lambda closing at
        // this indent would truncate the extent — and a truncated extent
        // still contains `m_sessionTrustState` (assigned early) and
        // `deviceVerified` (a parameter at the very top), so both assertions
        // below would pass while seeing none of the branch they guard.
        // Naming the first arm and the last is what makes that impossible.
        QVERIFY2(handler.contains(QStringLiteral("m_sessionTrustState")),
                 "the scanned extent does not contain the label it guards");
        QVERIFY2(handler.contains(QStringLiteral("Cross-signing unavailable")),
                 "the scanned extent is missing the chain's FIRST arm");
        QVERIFY2(handler.contains(QStringLiteral("Not verified")),
                 "the scanned extent is missing the chain's LAST arm");

        const QString body = withoutComments(handler);
        QVERIFY2(body.contains(QStringLiteral("deviceCrossSigned")),
                 "the session trust label must read is_cross_signed_by_owner()");
        // The BARE identifier, so that `deviceCrossSigned || deviceVerified`
        // or a bool assigned from it is caught too — each would restore the
        // permanent green. Commented out in the parameter list is fine, which
        // is why the stripper has to handle `/* */`.
        QVERIFY2(!body.contains(QStringLiteral("deviceVerified")),
                 "the session trust label is reached from Device::is_verified(), "
                 "which matrix-sdk makes a constant true for our own device");
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
            pane.indexOf(QStringLiteral("if (event.pixelDelta.y !== 0"));
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
        // ── THE CAP IS DERIVED FROM `bubbleRow`, NOT FROM `bubble` ──────
        //
        // This used to assert `bubble.width > 8`, and that mechanism is now
        // known-wrong rather than merely changed, so the contract asserts
        // its replacement instead of being deleted.
        //
        // `bubble.width` is the column's OUTER width. `bubbleContent` insets
        // every child by `bubblePad` — 0 in Modern/Compact, 10 in Bubbles —
        // so a cap written against it is exactly right where the padding is
        // zero and 20 px too generous where it is not. Measured on a real
        // 640 px row: a long body laid out 12 px past the bubble's inner
        // edge and 2 px past the ROW, and media cards 10 px past both.
        //
        // And it is a LOOP in Bubbles: the bubble is SIZED FROM
        // `bubbleContent`'s implicit width, so a child clamped against
        // `bubble.width` feeds its own input. `bubbleRow` is `fillWidth` and
        // reports no implicit width, so that end of the chain is inert.
        QVERIFY2(delegate.contains(QStringLiteral("readonly property real contentInnerCap")),
                 "MessageDelegate lost contentInnerCap — the one cap every "
                 "width-bounded child is supposed to share");
        QVERIFY2(delegate.contains(QStringLiteral("bubbleRow.width - root.avatarGutterWidth")),
                 "contentInnerCap no longer derives from bubbleRow, which is "
                 "the only end of the chain that cannot feed its own input");
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
        // `contentInnerCap`, not `bubble.width` — see the note in
        // wrappedBodiesHaveStableIncubationWidths. The outer width overshot
        // every media card by the bubble's own 10 px padding on each side.
        QVERIFY2(mediaBlock.contains(QStringLiteral(
                     "Layout.maximumWidth: root.contentInnerCap")),
                 "the media column is not bounded by contentInnerCap");
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
        QVERIFY(delegate.contains(// The EXPRESSION again, wrapped in the source. `bubble.width`
        // became `contentInnerCap` for the reason the note above gives.
        QStringLiteral("560, Math.max(280, root.contentInnerCap * 0.72)")));
        QVERIFY(delegate.contains(QStringLiteral("Math.min(260, root.contentInnerCap)")));
        QVERIFY(delegate.contains(QStringLiteral("Math.min(340, root.contentInnerCap)")));
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
        QVERIFY(directBlock.contains(// The EXPRESSION, not the whole declaration line: the source wraps
        // it across two lines, and a contract that pins formatting breaks on
        // a reflow rather than on a behaviour change. That is how all three
        // assertions in this suite went stale at once.
        QStringLiteral("Math.min(360, root.contentInnerCap)")));
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

    // ── 2026-09-18: the video overlay's double-tap does not reach the
    //    control bar ────────────────────────────────────────────────────
    //
    // `overlayTap` is a SIBLING of `VideoControlBar`, so it covers the bar's
    // whole rectangle. Its buttons and slider are Controls and accept the
    // press; its background, its time label and the gaps between controls
    // are not. A click there toggled playback, and a DOUBLE click — a user
    // reaching for play and missing by a few pixels — CLOSED the overlay.
    // The image viewer has carried a band check on its own tap since the
    // two gestures were split; this is the same check.
    void theVideoOverlayTapExcludesItsControlBar()
    {
        const QString src = read(QStringLiteral("VideoViewerOverlay.qml"));
        QVERIFY(!src.isEmpty());
        const int at = src.indexOf(QStringLiteral("id: overlayTap"));
        QVERIFY2(at >= 0, "the video overlay's tap handler is gone, so this "
                          "case is testing nothing");
        const int close = src.indexOf(QStringLiteral("onDoubleTapped"), at);
        QVERIFY2(close > at, "the overlay tap no longer closes on a double "
                             "tap — re-anchor this case");
        // EACH HANDLER'S OWN BODY, sliced by brace depth. A window that
        // spans both lets the single tap's check satisfy the double tap's —
        // which is exactly what a first version of this case did, and the
        // mutation that removed the close's exclusion passed it.
        const auto bodyOf = [&src](int from) {
            const int open = src.indexOf(QLatin1Char('{'), from);
            if (open < 0)
                return QString{};
            int depth = 0;
            for (int i = open; i < src.size(); ++i) {
                if (src.at(i) == QLatin1Char('{'))
                    ++depth;
                else if (src.at(i) == QLatin1Char('}') && --depth == 0)
                    return src.mid(open, i - open + 1);
            }
            return QString{};
        };
        // BOTH gestures, not just one: the close is the expensive half, but
        // a stray play/pause on the bar is the one that happens every time.
        const int tapped = src.indexOf(QStringLiteral("onTapped:"), at);
        QVERIFY2(tapped > at && tapped < close,
                 "the overlay tap has no single-tap handler before its "
                 "double-tap one — re-anchor this case");
        const QString tapBody = bodyOf(tapped);
        const QString doubleBody = bodyOf(close);
        QVERIFY2(!tapBody.isEmpty() && !doubleBody.isEmpty(),
                 "could not slice the overlay tap handlers' bodies");
        QVERIFY2(tapBody.contains(QStringLiteral("onTheBar(")),
                 "a single click on the video control bar's background still "
                 "toggles playback");
        QVERIFY2(doubleBody.contains(QStringLiteral("onTheBar(")),
                 "a DOUBLE click on the video control bar's background still "
                 "closes the overlay — a user reaching for play and missing "
                 "by a few pixels loses the video");
    }

    // ── 2026-09-18: selection mode really does suspend the row ──────────
    //
    // The selection TapHandler carries
    // `grabPermissions: PointerHandler.CanTakeOverFromAnything` and a comment
    // claiming "everything a row normally does — open an image, follow a
    // link, add a reaction — is suspended while selecting". It was not. Grab
    // permissions govern who may take an EXCLUSIVE grab; pointer events are
    // delivered innermost-first, so a handler on a CHILD acts before the
    // row's ever sees the press. Reproduced on a real build: clicking a
    // picture while picking messages to forward opened the full-screen
    // viewer over the picker.
    //
    // DERIVED, so a surface added later cannot quietly opt out: every
    // TapHandler and MouseArea in the delegate must either be gated on
    // `rowActionsEnabled`, be the selection handler itself, or be one of the
    // two exceptions named below — each of which carries its reason in the
    // source beside it.
    void everyRowSurfaceIsSuspendedWhileSelecting()
    {
        const QString src = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!src.isEmpty());
        QVERIFY2(src.contains(QStringLiteral("property bool rowActionsEnabled")),
                 "the gate is gone, so this case is testing nothing");

        const QStringList lines = src.split(QLatin1Char('\n'));
        int handlers = 0;
        int gated = 0;
        QStringList offenders;
        for (int i = 0; i < lines.size(); ++i) {
            const QString trimmed = lines.at(i).trimmed();
            if (trimmed != QLatin1String("TapHandler {")
                && trimmed != QLatin1String("MouseArea {")) {
                continue;
            }
            // The block, by brace depth — a fixed window after the name is
            // defeated by any comment inside it, which is the shape that has
            // cost this suite four cases.
            QString block = lines.at(i);
            int depth = lines.at(i).count(QLatin1Char('{'))
                        - lines.at(i).count(QLatin1Char('}'));
            int j = i + 1;
            while (j < lines.size() && depth > 0) {
                block += QLatin1Char('\n') + lines.at(j);
                depth += lines.at(j).count(QLatin1Char('{'))
                         - lines.at(j).count(QLatin1Char('}'));
                ++j;
            }
            ++handlers;
            const bool isSelection =
                block.contains(QStringLiteral("toggleSelectionForThisRow"));
            // The two documented exceptions: the right-click menu, which is
            // how selection mode is left as well as entered, and the failed
            // local echo's retry/cancel, which is not a selectable row.
            const bool isContextMenu =
                block.contains(QStringLiteral("acceptedButtons: Qt.RightButton"));
            const bool isLocalEchoRecovery =
                block.contains(QStringLiteral("retrySend("))
                || block.contains(QStringLiteral("cancelSend("));
            if (isSelection || isContextMenu || isLocalEchoRecovery)
                continue;
            if (block.contains(QStringLiteral("rowActionsEnabled"))) {
                ++gated;
                continue;
            }
            offenders << QStringLiteral("line %1").arg(i + 1);
        }
        QVERIFY2(handlers >= 20,
                 qPrintable(QStringLiteral("only %1 handler blocks found; the "
                                           "scan has stopped matching the file")
                                .arg(handlers)));
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "MessageDelegate handlers that still act while the user "
                     "is picking messages to forward: %1. Pointer events go "
                     "innermost-first, so the row's selection handler cannot "
                     "suppress them however its grabPermissions are set — "
                     "each one needs `enabled: root.rowActionsEnabled`, or a "
                     "documented reason not to")
                     .arg(offenders.join(QStringLiteral(", ")))));
        QVERIFY2(gated >= 15,
                 qPrintable(QStringLiteral("only %1 gated surfaces; the file "
                                           "has lost most of its gating")
                                .arg(gated)));
    }

    // ── 2026-09-18: a file that asks for an mxc image must also listen
    //    for the answer ────────────────────────────────────────────────
    //
    // `MediaBridge::mxcImageSource` returns an EMPTY STRING on a cache miss
    // and dispatches a fetch; the answer arrives later as `mediaCached`. A
    // binding that never touches a counter bumped from that signal asks
    // exactly once, gets nothing, and shows a blank square for the life of
    // the surface. Every call site in the tree paired the two except two:
    // the `:shortcode` completion popup and the sticker-pack editor, where
    // custom emoji and pack images rendered empty on first use and appeared
    // only if the bytes happened to be cached already.
    //
    // DERIVED from the call sites, so a new one cannot be added without
    // either the handler or a deliberate edit to this case.
    void everyFileThatAsksForAnMxcImageListensForTheAnswer()
    {
        QDir dir(QStringLiteral(QML_DIR));
        const QStringList files =
            dir.entryList({ QStringLiteral("*.qml") }, QDir::Files);
        QVERIFY2(files.size() > 50, "the QML directory scan found almost "
                                    "nothing, so this case audits nothing");
        int callers = 0;
        for (const QString &file : files) {
            const QString src = read(file);
            if (!src.contains(QStringLiteral("mxcImageSource(")))
                continue;
            ++callers;
            QVERIFY2(src.contains(QStringLiteral("onMediaCached")),
                     qPrintable(QStringLiteral(
                         "%1 calls mxcImageSource() and never listens for "
                         "mediaCached, so every image it asks for before the "
                         "bytes are cached stays blank for the life of the "
                         "surface — the binding asks once and the answer "
                         "arrives on a signal nothing here is connected to")
                         .arg(file)));
        }
        QVERIFY2(callers >= 5,
                 qPrintable(QStringLiteral("only %1 caller(s) found; the scan "
                                           "has stopped matching the tree")
                                .arg(callers)));
    }

    // ── 2026-09-18: every binding that reads the poll's answer TEXT must
    //    touch `answerRevision` ─────────────────────────────────────────
    //
    // `answerTexts()` reads `answerModel.get(i).answerText`, and a ListModel
    // `setProperty()` edit carries no QML-tracked dependency — so a binding
    // that calls it re-evaluates on a row being ADDED and never on a row
    // being TYPED INTO. `previewAnswers` and `formValid` both open with a
    // bare `answerRevision` read and both say why; `dirty` did not.
    //
    // The consequence was a silent draft loss: a poll typed only into answer
    // rows, with the question still empty, left `dirty` false, which kept
    // `Popup.CloseOnPressOutside` in the closePolicy and sent `maybeClose()`
    // down the branch that does not ask. A click outside destroyed the
    // draft, and so did Cancel and the X.
    //
    // DERIVED, not pinned: the rule is "every binding calling answerTexts()",
    // so a fourth one added tomorrow is covered without editing this case.
    void everyPollBindingOnAnswerTextTouchesTheRevision()
    {
        const QString src = read(QStringLiteral("CreatePollDialog.qml"));
        QVERIFY(!src.isEmpty());
        QVERIFY2(src.contains(QStringLiteral("property int answerRevision")),
                 "the revision counter is gone, so this case is testing "
                 "nothing");

        // Walk the file collecting each `readonly property ... :` binding
        // and the text of its expression, stopping at the next declaration
        // or a closing brace at the declaration's own depth.
        const QStringList lines = src.split(QLatin1Char('\n'));
        static const QRegularExpression decl(
            QStringLiteral("^\\s*(readonly\\s+)?property\\s+\\S+\\s+(\\w+)\\s*:"));
        int checked = 0;
        for (int i = 0; i < lines.size(); ++i) {
            const auto m = decl.match(lines.at(i));
            if (!m.hasMatch())
                continue;
            const QString name = m.captured(2);
            // The expression: this line plus every following line until the
            // next declaration or a line that closes the block.
            QString expr = lines.at(i);
            int braces = expr.count(QLatin1Char('{')) - expr.count(QLatin1Char('}'));
            int j = i + 1;
            while (j < lines.size()
                   && (braces > 0
                       || (!decl.match(lines.at(j)).hasMatch()
                           && !lines.at(j).trimmed().startsWith(
                                  QLatin1String("function "))
                           && !lines.at(j).trimmed().isEmpty()
                           && !lines.at(j).trimmed().startsWith(
                                  QLatin1String("//"))))) {
                expr += QLatin1Char('\n') + lines.at(j);
                braces += lines.at(j).count(QLatin1Char('{'))
                          - lines.at(j).count(QLatin1Char('}'));
                ++j;
                if (braces <= 0 && expr.contains(QLatin1Char('{')))
                    break;
            }
            if (!expr.contains(QStringLiteral("answerTexts()")))
                continue;
            ++checked;
            QVERIFY2(expr.contains(QStringLiteral("answerRevision")),
                     qPrintable(QStringLiteral(
                         "CreatePollDialog's `%1` reads the answer TEXT "
                         "through answerTexts() without touching "
                         "answerRevision, so it never re-evaluates when a "
                         "row is typed into — only when one is added. For "
                         "`dirty` that meant a draft typed into the answers "
                         "alone was discarded by a click outside, with no "
                         "confirmation.").arg(name)));
        }
        QVERIFY2(checked >= 3,
                 qPrintable(QStringLiteral(
                     "only %1 binding(s) on answerTexts() were found; the "
                     "scan has stopped matching the file it audits")
                     .arg(checked)));
    }

    // ── 2026-09-18: five more bindings that never re-evaluated ───────────
    //
    // All from the same audit and all the same shape: a Q_INVOKABLE reached
    // through a binding, which registers NO dependency, so the answer was
    // whatever was true when the component was built.

    /// The innermost `{ … }` block containing `pos`. A binding written as a
    /// braced expression body is the unit these cases care about: a `.count`
    /// read in a SIBLING binding proves nothing about this one.
    static QString blockAround(const QString &src, int pos)
    {
        int depth = 0;
        int open = -1;
        for (int i = pos; i >= 0; --i) {
            const QChar c = src.at(i);
            if (c == QLatin1Char('}'))
                ++depth;
            else if (c == QLatin1Char('{')) {
                if (depth == 0) { open = i; break; }
                --depth;
            }
        }
        if (open < 0)
            return {};
        depth = 0;
        for (int i = open; i < src.size(); ++i) {
            if (src.at(i) == QLatin1Char('{'))
                ++depth;
            else if (src.at(i) == QLatin1Char('}') && --depth == 0)
                return src.mid(open, i - open + 1);
        }
        return {};
    }

    // The popped-out call window resolved every row through an index
    // lookup. `CallShareModel::indexOfShare` and
    // `CallParticipantModel::indexOfIdentity` are Q_INVOKABLEs, and both
    // models expose a NOTIFYING `count` — so a binding that calls the
    // lookup without reading the count is frozen at its first answer.
    //
    // The expensive one was `fillShareShown`: "fill the window with this
    // share" is dropped by `onFillShareShownChanged` when the share ends,
    // and that signal could never fire, so the window stayed filled with a
    // share that had stopped.
    //
    // DERIVED, not a list of three names: every lookup in the file is
    // found and checked, so a fourth added later is covered without editing
    // this case.
    void thePipWindowsRowLookupsReadTheModelsCount()
    {
        const QString src = read(QStringLiteral("CallPipWindow.qml"));
        QVERIFY(!src.isEmpty());
        static const QRegularExpression lookup(
            QStringLiteral("\\.(indexOfShare|indexOfIdentity)\\s*\\("));
        int checked = 0;
        QRegularExpressionMatchIterator it = lookup.globalMatch(src);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const QString block = blockAround(src, m.capturedStart());
            QVERIFY2(!block.isEmpty(),
                     qPrintable(QStringLiteral(
                         "could not slice the block around %1 — re-anchor "
                         "this case").arg(m.captured(1))));
            // A `Qt.binding` re-assert or an imperative refresh function is
            // not a binding and needs no count read; a braced PROPERTY body
            // is. Both forms here are property bodies, so the rule is flat.
            ++checked;
            QVERIFY2(block.contains(QStringLiteral(".count")),
                     qPrintable(QStringLiteral(
                         "a binding calling %1 never reads the model's "
                         "`count`, so it registers no dependency and is "
                         "frozen at the answer it gave when the window was "
                         "built — shares and participants come and go "
                         "underneath it").arg(m.captured(1))));
        }
        QVERIFY2(checked >= 3,
                 qPrintable(QStringLiteral(
                     "only %1 row lookup(s) found in CallPipWindow.qml; the "
                     "scan has stopped matching the file it audits")
                     .arg(checked)));
    }

    // A CheckBox that both BINDS `checked` and writes in `onToggled` stops
    // following its source the moment it is used: a user toggle ASSIGNS
    // `checked`, which destroys the binding. `setSubscribed` is
    // asynchronous and can fail, so the box would keep claiming a
    // subscription the server refused. And the binding read
    // `isSubscribed()` — a Q_INVOKABLE — where the controller exposes a
    // notifying `subscriptions` list saying exactly the same thing.
    void theFollowCheckboxKeepsFollowingTheStoreAfterAClick()
    {
        const QString src = read(QStringLiteral("PolicyListDialog.qml"));
        QVERIFY(!src.isEmpty());
        const int at = src.indexOf(QStringLiteral("policyFollowCheck"));
        QVERIFY2(at >= 0, "the follow checkbox is gone — re-anchor this case");
        const QString block = blockAround(src, at);
        QVERIFY2(!block.isEmpty(), "could not slice the checkbox");
        // `.isSubscribed(` — a CALL, with the receiver's dot. The bare name
        // appears in the comment that explains why it is not used, and a
        // scan that cannot tell those apart fails on its own documentation.
        QVERIFY2(!block.contains(QStringLiteral(".isSubscribed(")),
                 "the follow checkbox reads `isSubscribed()`, a Q_INVOKABLE "
                 "that registers no binding dependency, instead of the "
                 "controller's notifying `subscriptions` list");
        QVERIFY2(block.contains(QStringLiteral("subscriptions")),
                 "the follow checkbox no longer reads the subscriptions list "
                 "at all");
        const int toggled = block.indexOf(QStringLiteral("onToggled"));
        QVERIFY2(toggled >= 0, "the follow checkbox no longer writes on a "
                               "toggle — re-anchor this case");
        QVERIFY2(block.mid(toggled).contains(QStringLiteral("Qt.binding(")),
                 "the follow checkbox writes on a toggle and never restores "
                 "its binding: a user click assigns `checked`, which DESTROYS "
                 "the binding above, so from the first click on the box stops "
                 "following the store — including when the write fails");
    }

    // The viewer's thumbnail strip asked the bridge for each picture once.
    // `mediaSource()` answers a miss with an empty string and dispatches a
    // fetch; the bytes arrive as `mediaCached`. Without a counter bumped
    // from that signal, every picture the strip had not already cached
    // stayed an empty 48px tile — on the one surface whose whole job is
    // showing what else is there.
    void theViewerThumbnailStripResolvesMediaThatArrivesLate()
    {
        const QString src = read(QStringLiteral("ImageViewerOverlay.qml"));
        QVERIFY(!src.isEmpty());
        const int at = src.indexOf(QStringLiteral("id: thumbImage"));
        QVERIFY2(at >= 0, "the strip's thumbnail image is gone — re-anchor "
                          "this case");
        const QString block = blockAround(src, at);
        QVERIFY2(!block.isEmpty(), "could not slice the thumbnail image");
        // THE `source:` EXPRESSION ITSELF, not the delegate around it. A
        // first version checked the whole block, and the mutation that
        // deleted the tick READ passed it: the `Connections` handler that
        // bumps the counter still names it, so "the file mentions
        // resolveTick" was true on code where the binding ignored it.
        const int src0 = block.indexOf(QStringLiteral("source:"));
        QVERIFY2(src0 >= 0, "the thumbnail has no source binding");
        const QString expr = blockAround(block, block.indexOf(
                                 QLatin1Char('{'), src0) + 1);
        QVERIFY2(!expr.isEmpty() && expr.contains(QStringLiteral("mediaSource(")),
                 "could not slice the thumbnail's source expression");
        QVERIFY2(expr.contains(QStringLiteral("resolveTick")),
                 "the strip's thumbnail source calls mediaSource() without "
                 "reading a tick, so it registers no dependency: a picture "
                 "whose bytes arrive after the strip is built never appears");
        QVERIFY2(block.contains(QStringLiteral("onMediaCached")),
                 "the strip's thumbnail has a tick that nothing bumps — "
                 "`mediaCached` is the signal that says the bytes arrived");
    }

    // The action bar is a plain Rectangle anchored over the bubble's
    // top-right corner. Its BUTTONS accept the press; its padding and the
    // gaps between them do not — so a click that missed a button by a pixel
    // reached the bubble's own tap handler and toggled the pin, closing the
    // bar out from under the pointer. Eighth instance of this shape.
    void theBubbleTapExcludesTheActionBarItOpens()
    {
        const QString src = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!src.isEmpty());
        const int at = src.indexOf(QStringLiteral("root.toggleActionsPin()"));
        QVERIFY2(at >= 0, "the bubble tap no longer pins the action bar — "
                          "re-anchor this case");
        const QString block = blockAround(src, at);
        QVERIFY2(!block.isEmpty(), "could not slice the bubble tap handler");
        QVERIFY2(block.contains(QStringLiteral("messageActionBarLoader")),
                 "a click on the action bar's padding, or in a gap between "
                 "its buttons, still falls through to the bubble and toggles "
                 "the pin — closing the bar the user was aiming at");
    }

    // Two records that must be RE-READ rather than bound, because the
    // invokable behind each depends on state its argument cannot see: the
    // Home pane's greeting (a rename left the old name on screen) and the
    // Send Later notice (a room encrypted mid-session was still promised
    // durable storage, when an encrypted room's scheduled message is held
    // in memory and discarded on close).
    void recordsBehindInvokablesAreRefreshedRatherThanBound()
    {
        const QString home = read(QStringLiteral("HomePane.qml"));
        QVERIFY(!home.isEmpty());
        QVERIFY2(!home.contains(QStringLiteral("readonly property var "
                                               "activeAccount")),
                 "HomePane binds `activeAccount` to accounts.account(), a "
                 "Q_INVOKABLE that depends on the id alone — a rename or a "
                 "new avatar never reaches the greeting");
        QVERIFY2(home.contains(QStringLiteral("refreshActiveAccount")),
                 "HomePane has no refresh for its account record");
        QVERIFY2(home.contains(QStringLiteral("onAccountsChanged")),
                 "HomePane's account record is refreshed by nothing — "
                 "`accountsChanged` is the signal that says it moved");

        const QString later = read(QStringLiteral("SendLaterDialog.qml"));
        QVERIFY(!later.isEmpty());
        QVERIFY2(!later.contains(QStringLiteral("readonly property bool "
                                                "encryptedRoom")),
                 "SendLaterDialog binds `encryptedRoom` to a Q_INVOKABLE "
                 "keyed on the room id, and the dialog is ONE instance "
                 "reused for the life of the room — encryption turned on "
                 "mid-session leaves the durable-storage promise on screen");
        QVERIFY2(later.contains(QStringLiteral("onOpened: refreshEncryptedRoom")),
                 "SendLaterDialog does not re-read whether the room is "
                 "encrypted when it opens");
    }
    // ── 2026-09-18: the rail's width stops stopped following the interface
    //    size the first time anyone dragged it ────────────────────────────
    //
    // `SplitView.preferredWidth` is BOUND to `snapWidth(settings.
    // spacesRailWidth)`, and `snapWidth` reads the rail's stops, which are
    // made of `AppTheme.scaled` pixels. SplitView writes `preferredWidth`
    // itself while dragging, so the saver has to write it back on release —
    // and it wrote a NUMBER, which leaves the property unbound for the rest
    // of the session.
    //
    // Measured on the running client: a rail dragged at 100% and then moved
    // to 140% stayed 100px wide. 100 is not a stop at that scale
    // (95/104/120/136/152), so the indent budget lands half a step short and
    // the rail draws one nesting level fewer than the grid intends — the
    // exact failure the stops exist to prevent — until the next launch
    // re-created the binding.
    //
    // Same shape as the follow checkbox in PolicyListDialog: an imperative
    // write to a bound property is a one-way door unless it is put back.
    void theRailWidthSaverPutsItsBindingBack()
    {
        const QString src = read(QStringLiteral("MainScreen.qml"));
        QVERIFY(!src.isEmpty());
        const int at = src.indexOf(QStringLiteral("id: railWidthSaver"));
        QVERIFY2(at >= 0, "the rail's width saver is gone — re-anchor this "
                          "case");
        const QString block = blockAround(src, at);
        QVERIFY2(!block.isEmpty(), "could not slice the width saver");
        QVERIFY2(block.contains(QStringLiteral("preferredWidth")),
                 "the width saver no longer writes preferredWidth — "
                 "re-anchor this case");
        const int write = block.indexOf(QStringLiteral(
            "SplitView.preferredWidth ="));
        QVERIFY2(write >= 0, "the width saver no longer assigns "
                             "preferredWidth — re-anchor this case");
        QVERIFY2(block.mid(write, 120).contains(QStringLiteral("Qt.binding(")),
                 "the width saver assigns a NUMBER to preferredWidth, which "
                 "leaves it unbound: after one drag the rail's width stops "
                 "stop following the interface size for the rest of the "
                 "session");

        // ...AND THE OTHER HALF, which the first fix created. Once
        // `preferredWidth` is a live binding it re-evaluates on every
        // interface-size change, and each re-evaluation reached this saver
        // and rewrote the STORED width — so a rail dragged to 112 at 100%
        // came back 100 after a round trip through 140%. The setting records
        // what the user dragged to; only a real divider drag may write it.
        QVERIFY2(block.contains(QStringLiteral("property bool dragged")),
                 "the width saver has no drag flag, so a width change the "
                 "interface size produced is indistinguishable from one the "
                 "user dragged — and gets persisted as if it were");
        const int wc = src.indexOf(QStringLiteral("onWidthChanged:"), at - 4000);
        QVERIFY2(wc >= 0 && wc < at,
                 "the rail's onWidthChanged handler is gone — re-anchor this "
                 "case");
        QVERIFY2(src.mid(wc, 160).contains(QStringLiteral("railWidthSaver.dragged")),
                 "the rail persists on ANY width change, including the ones "
                 "its own scaled-stop binding produces, so the stored width "
                 "creeps a stop narrower on every interface-size round trip");
    }
};

QTEST_MAIN(QmlBindingContractTest)
#include "QmlBindingContractTest.moc"
