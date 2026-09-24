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

    // One text-rendering element and the properties declared at its own brace
    // depth, so a nested element's declaration never satisfies a check on its
    // parent, and comments inside the block cannot defeat a fixed window.
    struct TextElement {
        int line = 0;
        QStringList properties;

        // The value of `name:` as written, or empty. Multi-line expressions are
        // joined, so the whole expression is matched.
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
        // Every element that renders `text:` and can carry a format. MenuItem
        // and AppMenuItem are excluded: an AbstractButton has no `textFormat`,
        // so declaring one is a load-time error. AppMenuItem's own contentItem
        // Label declares PlainText for every menu row.
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
                        // A block binding (`text: { ... }`) opens a brace on
                        // the property's own line. Flush the pending line as a
                        // property first, or the element is silently skipped.
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
    // A media-cache completion handler must re-evaluate the Image's `source`
    // binding (bump a counter it reads), never assign `source`: an imperative
    // write destroys the binding and the Image keeps its first image for the
    // rest of the session. Scans every QML file, since the pattern gets copied.
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
        // The scan is only meaningful if it found the handlers.
        QVERIFY2(handlersSeen >= 10,
                 qPrintable(QStringLiteral("only %1 onMediaCached handlers found")
                                .arg(handlersSeen)));
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "onMediaCached assigns a bound `source` (this strands the "
                     "Image on the first image it ever loaded) in: %1")
                                .arg(offenders.join(QStringLiteral(", ")))));
    }

    // Ctrl+Q quits even with close-to-tray on: a window that refuses its close
    // aborts the quit, so the close handler must stand aside for a declared
    // quit intent.
    void ctrlQQuitsEvenWithCloseToTrayOn()
    {
        const QString main = read(QStringLiteral("Main.qml"));
        QVERIFY(!main.isEmpty());
        // The shortcut announces the intent before asking Qt to quit. Anchored
        // on the action id, since the key is rebindable.
        const int quitAction =
            main.indexOf(QStringLiteral("sequenceFor(\"app.quit\")"));
        QVERIFY2(quitAction > 0,
                 "the quit Shortcut must take its sequence from the registry");
        const QString shortcut = bracedBody(
            main, main.lastIndexOf(QStringLiteral("Shortcut {"), quitAction));
        QVERIFY2(!shortcut.isEmpty(), "the quit Shortcut must still exist");
        QVERIFY(shortcut.contains(QStringLiteral("quitRequested = true")));
        QVERIFY(shortcut.contains(QStringLiteral("Qt.quit()")));
        // ...and the close handler stands aside when it sees it.
        const QString closing = bracedBody(
            main, main.indexOf(QStringLiteral("onClosing:")));
        QVERIFY(!closing.isEmpty());
        QVERIFY(closing.contains(QStringLiteral("!window.quitRequested")));
        QVERIFY(closing.contains(QStringLiteral("closeToTray")));

        // Every other deliberate quit (e.g. applying an update from C++) must
        // announce itself the same way, or close-to-tray aborts it.
        QVERIFY2(main.contains(QStringLiteral("onApplicationQuitIntended")),
                 "the window must honour a deliberate quit from C++, or "
                 "close-to-tray silently vetoes an update");
        const QString intended = bracedBody(
            main, main.indexOf(QStringLiteral("onApplicationQuitIntended")));
        QVERIFY2(intended.contains(QStringLiteral("quitRequested = true")),
                 "the announced quit must set the same flag Ctrl+Q sets");
    }

    // Bringing the window forward never calls show(): QWindow::show() forces
    // the Normal state, un-maximizing the window, and onVisibilityChanged then
    // persists that. The tray and notification paths share raiseIntoView().
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
        // Hidden to the tray comes back through `visible`, which restores the
        // previous state.
        QVERIFY(body.contains(QStringLiteral("Window.Hidden")));
        QVERIFY(body.contains(QStringLiteral("window.visible = true")));
        // A minimized window is restored to its previous state, not to
        // Windowed.
        QVERIFY(body.contains(QStringLiteral("Window.Minimized")));
        QVERIFY(body.contains(QStringLiteral("lastOnScreenVisibility")));
        QVERIFY(body.contains(QStringLiteral("window.raise()")));
        QVERIFY(body.contains(QStringLiteral("window.requestActivate()")));

        // Both callers go through it.
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

        // The pre-minimize state is actually recorded.
        const int vis = main.indexOf(QStringLiteral("onVisibilityChanged:"));
        QVERIFY(vis > 0);
        const QString visBody = bracedBody(main, vis);
        QVERIFY(visBody.contains(
            QStringLiteral("window.lastOnScreenVisibility = window.visibility")));
    }

    // Window geometry: size is restored declaratively from a constant value;
    // position is applied once, imperatively (as a binding it re-centred the
    // window under the user mid-drag). The window starts hidden and is shown
    // only after placement, so nobody sees it jump. Both halves are asserted.
    void windowGeometryIsRestoredInBindingsAndFlushedOnClose()
    {
        const QString main = read(QStringLiteral("Main.qml"));
        QVERIFY(!main.isEmpty());
        // Declarative restore, from the pre-filtered constant value.
        QVERIFY(main.contains(QStringLiteral(
            "readonly property rect startupGeometry: app.restorableWindowGeometry")));
        // Size stays declarative: neither dimension reads a notifying source.
        for (const QString &prop : { QStringLiteral("width:"),
                                    QStringLiteral("height:") }) {
            QVERIFY2(main.contains(prop + QStringLiteral(" hasStartupGeometry")),
                     qPrintable(prop));
        }
        // Position must not be a binding.
        QVERIFY2(!main.contains(QStringLiteral("x: hasStartupGeometry")),
                 "window x is a binding again: it will fight the user's drag "
                 "and re-centre on any screen-metric change");
        QVERIFY2(!main.contains(QStringLiteral("y: hasStartupGeometry")),
                 "window y is a binding again");
        // ...and the window is not visible before it is placed.
        QVERIFY2(main.contains(QStringLiteral("visible: false")),
                 "the window is shown before startup placement is applied, so "
                 "the user watches it jump into position");
        const QString completed = bracedBody(
            main, main.indexOf(QStringLiteral("Component.onCompleted:")));
        QVERIFY(!completed.isEmpty());
        QVERIFY2(completed.contains(QStringLiteral("applyStartupPlacement()")),
                 "startup placement is never applied");
        // Placement, then show.
        QVERIFY(completed.indexOf(QStringLiteral("applyStartupPlacement()"))
                < completed.indexOf(QStringLiteral("visible = true")));
        // The size is not assigned from the handler.
        QVERIFY(!completed.contains(QStringLiteral("window.width")));
        // Saved only from the windowed state, and flushed when closing.
        QVERIFY(main.contains(QStringLiteral("saveWindowGeometry")));
        QVERIFY(main.contains(QStringLiteral(
            "window.visibility !== Window.Windowed")));
        const QString closing = bracedBody(
            main, main.indexOf(QStringLiteral("onClosing:")));
        QVERIFY(closing.contains(QStringLiteral("window.flushGeometry()")));
    }

    // The viewport-fill budget gates the request, not just the retry timer.
    // In an archived room whose history is all routine state, the rows fold
    // into one 60 px group, `contentHeight >= height` never holds, and the
    // fill paginated towards the room's start without pause. A source
    // contract because the mock delivers pages without the paced row reveal
    // that drives the runaway, so a behavioural test passes unfixed.
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

        // Two bounds for two kinds of progress: a page that added rows moves
        // the cursor towards real messages beyond a collapsed run and gets the
        // generous bound; a page that added nothing gets the small one.
        // Reaching older history must never require expanding a group.
        QVERIFY2(body.contains(QStringLiteral("viewportFillLastHeight")),
                 "the budget is spent on attempts rather than on attempts "
                 "that failed to make the content taller");
        QVERIFY2(body.contains(QStringLiteral("viewportFillLastRows")),
                 "nothing distinguishes a page that advanced the pagination "
                 "cursor from one that did nothing, so a collapsed run stops "
                 "the reader dead and expanding it by hand is the only way on");
        QVERIFY2(body.contains(QStringLiteral("maxInvisibleFillRetries")),
                 "invisible progress has no bound of its own");

        // A third kind: a page the timeline filter emptied walked real events
        // and must not spend the small bound. Only the controller's
        // empty-page counter tells it apart from a page with nothing.
        QVERIFY2(body.contains(QStringLiteral("viewportFillLastEmptyPages")),
                 "nothing compares the controller's completed-empty-page "
                 "counter across attempts, so a page the filter emptied is "
                 "indistinguishable from a dispatch that went nowhere and "
                 "spends the small no-progress bound meant for the latter");
        QVERIFY2(body.contains(QStringLiteral("maxEmptyFillPages")),
                 "a run of filtered history has no bound of its own");

        // A call while a page is in flight must not spend the budget: every
        // geometry signal calls this, and none can see growth before the page
        // arrives.
        const int busyGuard = body.indexOf(QStringLiteral("app.pagination.busy"));
        QVERIFY2(busyGuard >= 0,
                 "calls made while a page is in flight still spend the fill "
                 "budget, so redundant geometry signals exhaust it before any "
                 "page has had a chance to help");
        QVERIFY2(busyGuard < budget,
                 "the in-flight guard must come before the budget is spent");
    }

    // The room list width is saved when the drag ends (`resizing` falls): the
    // release moves nothing, so onWidthChanged never offers the final width.
    void roomListWidthIsSavedWhenTheDragEnds()
    {
        const QString shell = read(QStringLiteral("MainScreen.qml"));
        QVERIFY(!shell.isEmpty());
        const int target = shell.indexOf(
            QStringLiteral("target: roomsPanel.SplitView.view"));
        QVERIFY2(target > 0, "the drag-release trigger must exist");
        // From the enclosing Connections, so handler and target are the same
        // block.
        const int conn = shell.lastIndexOf(QStringLiteral("Connections {"),
                                           target);
        QVERIFY(conn > 0);
        const QString body = bracedBody(shell, conn);
        QVERIFY(body.contains(QStringLiteral("onResizingChanged")));
        QVERIFY(body.contains(QStringLiteral("widthSaver.restart()")));
        // Still debounced during the drag.
        QVERIFY(shell.contains(QStringLiteral(
            "onWidthChanged: if (!SplitView.view.resizing) widthSaver.restart()")));
    }

    // The image viewer closes instantly on a tap outside the picture and zooms
    // at the pointer on a tap on it.
    void imageViewerClosesInstantlyOutsideThePictureAndZoomsOnIt()
    {
        const QString viewer = read(QStringLiteral("ImageViewerOverlay.qml"));
        QVERIFY(!viewer.isEmpty());

        // Closing never waits out a double-click interval (no double-tap
        // handler anywhere) and never requires the X button.

        // The scrim's tap: exactly one, undelayed.
        QCOMPARE(viewer.count(QStringLiteral("onTapped: viewer.close()")), 1);

        // The picture's tap zooms at the pointer...
        QVERIFY2(viewer.contains(QStringLiteral("viewer.toggleZoomAt")),
                 "the picture no longer zooms on click");
        // A tap outside the drawn image still closes: `imageHolder` fills the
        // viewport, and its margin is scrim to the user. Keyed on the band
        // comparison itself, not on an expression the file repeats.
        QVERIFY2(viewer.contains(QStringLiteral("x < left || x > left + iw")),
                 "the image tap has no band check, so the margin around a "
                 "fitted picture is still part of the picture's hit target "
                 "and a click there zooms instead of closing");

        // Never a double-tap: that delay is what the instant close avoids.
        QVERIFY(!viewer.contains(QStringLiteral("onDoubleTapped")));
        QVERIFY(!viewer.contains(QStringLiteral("exclusiveSignals")));

        // Zoom survives through the non-conflicting inputs.
        QVERIFY(viewer.contains(QStringLiteral("WheelHandler")));
        QVERIFY(viewer.contains(QStringLiteral("zoomStep(1.2)")));
    }

    // Dropping files anywhere over the chat queues composer attachments, with
    // real tray previews (image thumbnail, animated GIF, first-frame video
    // poster) and the remove button.
    void chatWideDropQueuesAttachmentsWithRichPreviews()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!pane.isEmpty());
        QVERIFY(pane.contains(QStringLiteral("id: chatDropArea")));
        QVERIFY(pane.contains(QStringLiteral(
            "app.composer.addAttachment(drop.urls[i])")));
        QVERIFY(pane.contains(QStringLiteral("keys: [\"text/uri-list\"]")));
        // The chat-wide area never covers the thread surface, which owns its
        // own drops (thread send path).
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
        // Persisted previews may still contain '\n' (which breaks lines even
        // with elide), so the label needs the hard clamp; plain text keeps a
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

    // Every text binding that could carry remote text declares PlainText. A
    // Label defaults to AutoText, so a room name like `<img src="http://x">`
    // would make every viewer fetch that URL, bypassing the media bridge.
    // Exemptions are listed here where they can be argued with.
    void unsanitizedServerTextIsAlwaysPlainText()
    {
        // What counts as text we do not control. Identifier-based (requiring a
        // format on every non-literal binding flags hundreds of harmless
        // numeric labels), case-insensitive, following one property hop
        // (`text: root._label`), and covering block bindings.
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
        // A binding that is only a property reference, e.g.
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
                // Its own declaration, at its own depth, and it must say
                // PlainText; merely declaring a textFormat is not enough.
                const QString declared =
                    element.property(QStringLiteral("textFormat"));
                // Exemption: a `highlighted*()` helper returns markup (a <font>
                // around the match) after escaping the untrusted name. Only
                // exempt while the helper actually escapes.
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
        // A sweep that matched nothing would pass vacuously; assert it found
        // elements.
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
        // The loading/failure indicator is a top overlay, not list content,
        // so toggling it never changes contentHeight or the reader's viewport.
        QVERIFY(!pane.contains(QStringLiteral("header: Item {")));
        QVERIFY(pane.contains(QStringLiteral("objectName: \"paginationHeader\"")));
        QVERIFY(pane.contains(QStringLiteral("anchors.top: parent.top")));
        QVERIFY(pane.contains(QStringLiteral("restoreScrollAnchor(app.currentRoomId)")));
        QVERIFY(pane.contains(QStringLiteral("saveScrollAnchor(")));
        QVERIFY(pane.contains(QStringLiteral("eventIdAtViewRow(row)")));

        // The shared delegate does not name a controller: ThreadPanel renders
        // it too, and app.pagination is the room timeline's. It navigates
        // through the view contract its host supplies. The thread panel's
        // "Open in room" item deliberately keeps app.pagination.jumpToEvent,
        // so this is scoped to the reply preview.
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.contains(
            QStringLiteral("app.pagination.jumpToEvent(model.replyToEventId")));
        QVERIFY(!delegate.contains(QStringLiteral("app.pagination.highlightedEventId")));
        QVERIFY(delegate.contains(QStringLiteral("navigateToEvent(")));
        QVERIFY(delegate.contains(QStringLiteral("navigationHighlightEventId")));
        // Both hosts supply that contract.
        QVERIFY(pane.contains(QStringLiteral("navigateToEvent")));
        QVERIFY(pane.contains(QStringLiteral("navigationHighlightEventId")));
        const QString threadPanel = read(QStringLiteral("ThreadPanel.qml"));
        QVERIFY(threadPanel.contains(QStringLiteral("navigateToEvent")));
        QVERIFY(threadPanel.contains(QStringLiteral("navigationHighlightEventId")));
        QVERIFY(pane.contains(QStringLiteral("viewportFillCheckScheduled")));
        QVERIFY(pane.contains(QStringLiteral("Qt.callLater(function()")));
        QVERIFY(pane.contains(QStringLiteral("app.pagination.requestViewportFill()")));
        // Near-top pagination is edge-triggered with hysteresis
        // (checkNearTopEdge between an enter and a wider exit band), so a
        // reader near the top does not re-request every frame. The passive
        // atYBeginning fill trigger stays.
        QVERIFY(pane.contains(QStringLiteral("function checkNearTopEdge(")));
        QVERIFY(pane.contains(QStringLiteral("nearTopArmed")));

        // Every near-top comparison uses distanceFromTop(), never raw
        // contentY: contentY is an offset from originY, which moves as history
        // loads. Geometry tests depend on where the fixture's originY sits;
        // this scan guards the comparison sites directly.
        QVERIFY(pane.contains(QStringLiteral("function distanceFromTop()")));
        // The top of history is the high bound on the rotated view, so the
        // distance is measured from wheelMaxY().
        QVERIFY(pane.contains(QStringLiteral(
            "return wheelMaxY() - contentY")));
        // The bands are distances and named so; the old ...Y names must not
        // return.
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
        // Both bands and the gesture-settle re-arm read the distance.
        QVERIFY(pane.contains(QStringLiteral("fromTop <= nearTopEnterDistance")));
        QVERIFY(pane.contains(QStringLiteral("fromTop >= nearTopExitDistance")));
        QVERIFY(pane.contains(QStringLiteral(
            "<= timeline.nearTopEnterDistance")));
        // The progress gate lives at the dispatch site: gating only the
        // re-arm let a downward gesture consume the latch, and stranded a
        // reader pinned at the exact top.
        QVERIFY(pane.contains(QStringLiteral("nearTopRequestDistance")));
        QVERIFY(pane.contains(QStringLiteral("fromTop <= 1")));
        QVERIFY(pane.contains(QStringLiteral(
            "fromTop < nearTopRequestDistance - 1")));
        // The baseline ratchets to the closest approach on every in-band
        // sample, not just the last dispatch distance.
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

    // The loading skeleton keys off both renderers' status (Image and
    // AnimatedImage), so a ready GIF frame never sits behind a placeholder.
    // Delegate reuse still resets media identity.
    void animatedGifSkeletonUsesActiveRendererState()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("objectName: \"imageSkeleton\"")));
        // The static renderer hides while the animated one plays and while
        // the image is locally hidden.
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

    // The GIF picker is wired to app.gif in both composers.
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
        // One button per composer opens the combined GIF/sticker picker.
        QVERIFY(thread.contains(QStringLiteral("threadMediaButton")));
        QVERIFY2(!thread.contains(QStringLiteral("threadGifButton")),
                 "the separate thread GIF button is back; the merged picker "
                 "has one entry point");
        QVERIFY(room.contains(QStringLiteral("composerMediaButton")));
        QVERIFY2(!room.contains(QStringLiteral("composerGifButton")),
                 "the separate room GIF button is back");
        // Both pickers offer the kind tabs and ask the host to swap.
        for (const QString &host : { room, thread }) {
            QVERIFY(host.contains(QStringLiteral("offerKindTabs")));
            QVERIFY(host.contains(QStringLiteral("onKindRequested")));
        }

        const QString picker = read(QStringLiteral("GifPicker.qml"));
        QVERIFY(!picker.isEmpty());
        // The picker's internals are covered by GifPickerRedesignContractTest
        // and GifPickerSelectionQmlTest. Here: a chosen GIF reaches the right
        // destination (room composer to the room, thread composer into the
        // thread).
        QVERIFY(room.contains(QStringLiteral(
            "app.gifSend.sendToRoom(app.currentRoomId, result)")));
        QVERIFY(thread.contains(QStringLiteral("app.gifSend.sendToThread(")));
        QVERIFY(thread.contains(QStringLiteral("app.thread.rootEventId")));
        // The picker itself never sends to a room/thread directly.
        QVERIFY(!picker.contains(QStringLiteral("sendToRoom")));
        QVERIFY(!picker.contains(QStringLiteral("sendTextMessage")));
    }

    // GIF autoplay is a tri-state (Always/OnHover/Never) honoured by the
    // timeline and picker, configured in Settings with safe-search, provider,
    // recents, clear actions and a privacy disclosure.
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
        // Provider availability, privacy disclosure and confirmed clears.
        QVERIFY(settings.contains(QStringLiteral("providerConfigured(\"giphy\")")));
        QVERIFY(settings.contains(QStringLiteral(
            "GIF searches are sent directly to the ")));
        QVERIFY(settings.contains(QStringLiteral("gifClearConfirm.open(\"favorites\")")));
        QVERIFY(settings.contains(QStringLiteral("app.gif.favorites.clearAll()")));
        // The starred-GIF store has its own count/size row and confirmed Clear
        // All: it holds decrypted file bytes on this device, not provider
        // metadata.
        QVERIFY(settings.contains(QStringLiteral("objectName: \"starredGifsSummaryLabel\"")));
        QVERIFY(settings.contains(QStringLiteral("app.gif.starredStore.count")));
        QVERIFY(settings.contains(QStringLiteral("app.gif.starredStore.totalBytes")));
        QVERIFY(settings.contains(QStringLiteral("kept on this ")));
        // The copy discloses that sign-out deletes the store.
        QVERIFY(settings.contains(QStringLiteral("device only and removed ")));
        QVERIFY(settings.contains(QStringLiteral(
            "when you sign out of this ")));
        QVERIFY(settings.contains(QStringLiteral("starredGifsClearConfirm.open()")));
        QVERIFY(settings.contains(QStringLiteral("app.gif.starredStore.clearAll()")));
    }

    // A thread root shows the summary card wired to the SDK thread-summary
    // roles, and activating it opens the thread.
    void threadRootUsesSummaryCard()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        QVERIFY(delegate.contains(QStringLiteral("ThreadSummaryCard {")));
        // The card shares its row with the "edited" marker, so it is bounded
        // by the remaining row width, not just its implicitWidth. Scoped to
        // the Loader's own block.
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
        // Gated on the thread-root role (via the Loader's `active:`).
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
        // The old plain-text presentation must not return.
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
        // The collapsed row is a compact clickable summary, not a card.
        const QString activity = read(QStringLiteral("RoomActivityDelegate.qml"));
        QVERIFY(!activity.contains(QStringLiteral("AppTheme.cardElevated")));
        QVERIFY(activity.contains(QStringLiteral("summaryRow")));
        QVERIFY(activity.contains(QStringLiteral("modelData.description")));
        QVERIFY(activity.contains(QStringLiteral("model: expandedColumn.visible ? root.entries")));
        QVERIFY(!activity.contains(QStringLiteral("linkPreviews")));
        QVERIFY(!activity.contains(QStringLiteral("messageActions")));
    }

    // State-activity controls qualify the attached view: a bare
    // `ListView.view` on a nested child is never populated.
    void stateActivityQualifiesListViewViewOnNestedControls()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = stateActivityBlock(delegate);
        QVERIFY(!block.isEmpty());
        // The delegate reaches its host through root.timelineView, resolved
        // once in the root's scope. An attached property named from a nested
        // object attaches to that object and fails silently, so no nested
        // block may name an attached view.
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
        // Both halves of the master switch (membership vs profile changes)
        // are read in the same presentation-only expression.
        QVERIFY(delegate.contains(
            QStringLiteral("app.settings.showMembershipEvents")));
        QVERIFY(delegate.contains(
            QStringLiteral("app.settings.showProfileChangeEvents")));
        // The zero-height presentation filter also covers the thread panel's
        // pinned-root suppression.
        QVERIFY(delegate.contains(QStringLiteral("naturalImplicitHeight")));
        // Same mechanism for a date divider whose whole run is hidden and for
        // redacted followers collapsed into one "N messages deleted" line: the
        // rows stay in the model, only drawing changes.
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

    // The unable-to-decrypt placeholder offers a manual Retry (via the view's
    // timeline model, so it works in threads) and a Security settings jump,
    // and never renders raw session or ciphertext fields.
    void undecryptableRowsExposeRetryAndSecurityActions()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("Retry decryption")));
        QVERIFY(delegate.contains(QStringLiteral(
            "root.timelineModel.retryDecryption()")));
        QVERIFY(delegate.contains(QStringLiteral(
            "app.showSettingsSection(\"security\")")));
        // No ciphertext/session-id model field is bound.
        QVERIFY(!delegate.contains(QStringLiteral("model.sessionId")));
        QVERIFY(!delegate.contains(QStringLiteral("model.ciphertext")));
    }

    // Source text with comments removed (whole-line, trailing and `/* */`).
    // The trust guards below assert the absence of an identifier, which the
    // explanatory comments would otherwise contain; `/* */` matters because a
    // parameter list keeps `bool /*deviceCrossSigned*/,`.
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
        // Block comments last, so a multi-line `/* */` is caught whole.
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
        // The list is filtered, but the Repeater's source is still the
        // authoritative app.sessionDevices.
        QVERIFY(settings.contains(QStringLiteral("var all = app.sessionDevices")));
        QVERIFY(settings.contains(QStringLiteral("var f = sessionFilter.current")));
        for (const char *branch : { "f === \"current\" && d.isCurrent === true",
                                    "f === \"verified\" && d.crossSigned === true",
                                    "f === \"unverified\"" })
            QVERIFY2(settings.contains(QLatin1String(branch)), branch);
        QVERIFY(settings.contains(QStringLiteral("This session")));
        QVERIFY(settings.contains(QStringLiteral("Not verified")));

        // No trust binding may read `verified`: Device::is_verified() is
        // always true for our own device (matrix-sdk marks it locally
        // trusted), so a label bound to it is permanently green. For other
        // devices is_verified() would be the better flag; relax this to "no
        // unconditional read" if that is ever done.
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

    // The session trust chip (`app.sessionTrustState`, composed in
    // AppController's ownDeviceStatusUpdated handler) must not derive
    // "Verified" from Device::is_verified(). Scans that handler's extent only,
    // and asserts the extent is real first.
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
        // Bracket the whole if/else chain by its first and last arm: a
        // truncated extent would still contain the early assignment and the
        // parameter, and pass while seeing no branch.
        QVERIFY2(handler.contains(QStringLiteral("m_sessionTrustState")),
                 "the scanned extent does not contain the label it guards");
        QVERIFY2(handler.contains(QStringLiteral("Cross-signing unavailable")),
                 "the scanned extent is missing the chain's FIRST arm");
        QVERIFY2(handler.contains(QStringLiteral("Not verified")),
                 "the scanned extent is missing the chain's LAST arm");

        const QString body = withoutComments(handler);
        QVERIFY2(body.contains(QStringLiteral("deviceCrossSigned")),
                 "the session trust label must read is_cross_signed_by_owner()");
        // The bare identifier, so `deviceCrossSigned || deviceVerified` is
        // caught too. A `/* */` mention in the parameter list is stripped.
        QVERIFY2(!body.contains(QStringLiteral("deviceVerified")),
                 "the session trust label is reached from Device::is_verified(), "
                 "which matrix-sdk makes a constant true for our own device");
    }

    // The recovery input is masked, accepts key or passphrase, is wiped right
    // after dispatch, and a successful recovery re-reads SDK trust/backup
    // state. No backup or cross-signing setup button is faked.
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
        // Bottom-follow is latched to user intent with a small slack; a reader
        // who scrolled up is never re-pinned by proximity.
        QVERIFY(pane.contains(QStringLiteral("function atBottomEdge()")));
        QVERIFY(pane.contains(QStringLiteral("wheelMinY() + bottomFollowSlack")));
        QVERIFY(!pane.contains(QStringLiteral("contentHeight - 40")));
        QVERIFY(!pane.contains(QStringLiteral("contentY + height === contentHeight")));
    }

    // Touchpad scrolling and anchor maintenance: the pixelDelta branch does no
    // per-delta anchor scan (it only restarts scrollSettleTimer),
    // maintainViewAnchor() writes nothing while a gesture owns the view, and
    // the anchor is recaptured when the gesture settles. The offscreen QPA
    // cannot show the pixel outcome, so this guards the wiring.
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
        // Strip comment lines first: the branch's comment names
        // captureViewAnchor(), but a bare call must still be caught.
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

        // Scan from the userScrollActive branch, not the function start: the
        // displaced-anchor resolve above it legitimately moves the view to
        // materialise a destroyed delegate.
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
        // The mid-gesture path writes nothing: the raw anchor delta cannot
        // distinguish rows resizing under the reader from the view re-anchoring,
        // and applying it pulled the reader both ways.
        QVERIFY(!maintainGuard.contains(QStringLiteral("contentY +=")));
        QVERIFY(!maintainGuard.contains(QStringLiteral("contentY =")));

        // userScrollActive covers the touchpad path via the settle timer
        // (moving and wheelAnimating are both false there).
        QVERIFY(pane.contains(QStringLiteral(
            "moving || wheelAnimating || scrollSettleTimer.running")));

        // The anchor is captured on gesture settle (mouse path and settle
        // timer).
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

        // Current-user status never changes horizontal flow in Modern rows.
        // Bubbles mode is the only coloured-bubble path, gated to DM
        // timelines behind the message-layout setting.
        QVERIFY(!delegate.contains(QStringLiteral("Qt.AlignRight")));
        QVERIFY(delegate.contains(QStringLiteral(
            "readonly property bool bubbleMode: timelineLayout === 1 && isDirectRoom")));
        QVERIFY(delegate.contains(QStringLiteral(
            "? (model.isOwn === true ? AppTheme.ownBubble")));
        // Modern/Compact bubbles are transparent: there is no mention wash
        // (some themes map mentionHighlight to a red-ish tone). The mention
        // edge bar is the signal.
        QVERIFY2(!delegate.contains(QStringLiteral("AppTheme.mentionHighlight")),
                 "the mention ROW WASH is back — mentionHighlight is the "
                 "badge's token and resolves to a danger-adjacent rose under "
                 "Storm, which is what drew a red box around mentions");
        QVERIFY(delegate.contains(QStringLiteral(
            "                       : \"transparent\"")));
        // The edge bar is the mention signal, so it must be present.
        QVERIFY(delegate.contains(QStringLiteral("mentionBarVisible")));
        QVERIFY(delegate.contains(QStringLiteral(
            "? AppTheme.bolt : AppTheme.borderStrong")));
    }

    // QML pushes the link colour to MessageHtml's mention style. The C++
    // parameter is defaulted and would fail silently, so the arity is pinned.
    void mentionStyleIsPushedWithTheLinkInk()
    {
        const QString shell = read(QStringLiteral("MainScreen.qml"));
        QVERIFY(!shell.isEmpty());
        // Four arguments at both push sites (timeline and thread model).
        const QRegularExpression call(QStringLiteral(
            "setMentionStyle\\(\\s*accent\\s*,\\s*soft\\s*,\\s*code\\s*,"
            "\\s*linkInk\\s*\\)"));
        QCOMPARE(shell.count(call), 2);
        QVERIFY2(shell.contains(QStringLiteral("AppTheme.link")),
                 "the link ink must come from the theme, not a literal");
        // ...and re-pushed when only the link ink changes (themes set link and
        // accent independently).
        QVERIFY2(shell.contains(QStringLiteral("function onLinkChanged()")),
                 "a theme change that moves only the link ink must re-push");
    }

    void continuationRowsStayCompactAndActionsFloat()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!delegate.isEmpty());
        QVERIFY(!pane.isEmpty());

        // Continuations drop the fixed 36 px avatar height and the permanent
        // timestamp line.
        QVERIFY(delegate.contains(QStringLiteral(
            "implicitHeight: root.showsIdentity ? 34 : bodyLabel.implicitHeight")));
        QVERIFY(delegate.contains(QStringLiteral(
            "objectName: \"continuationTimestamp\"")));
        // Shown only on a hovered continuation row, gated by the Loader's
        // `active:` so an empty Label never observes the viewport.
        QVERIFY(delegate.contains(QStringLiteral(
                    "active: !root.showsIdentity && rowHover.hovered"))
                || delegate.contains(QStringLiteral(
                    "visible: !root.showsIdentity && rowHover.hovered")));
        QVERIFY(delegate.contains(QStringLiteral("return \"\"")));
        QVERIFY(!delegate.contains(QStringLiteral(
            "return model.showSenderIdentity === true ? \"\" : ts")));

        // The toolbar overlays the right edge instead of taking a RowLayout
        // cell and narrowing the message column on hover.
        QVERIFY(delegate.contains(QStringLiteral("anchors.right: parent.right")));
        QVERIFY(delegate.contains(QStringLiteral("anchors.top: parent.top")));
        QVERIFY(pane.contains(QStringLiteral("spacing: 0")));
    }

    void wrappedBodiesHaveStableIncubationWidths()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        // Content is capped by `contentInnerCap`, derived from `bubbleRow`,
        // not `bubble.width`: the bubble's outer width ignores `bubblePad`
        // (10 px in Bubbles), and in Bubbles the bubble is sized from its
        // content, so capping against it is a loop.
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
        // The preview block ends at the upload progress bar, its next
        // sibling, which fills width on purpose.
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
        // ...and that bar really is the full-width one.
        QVERIFY(uploadStart > previewStart);
        QVERIFY(delegate.mid(uploadStart, metaStart - uploadStart)
                    .contains(QStringLiteral("Layout.fillWidth: true")));

        QVERIFY(mediaBlock.contains(QStringLiteral(
            "Layout.alignment: Qt.AlignLeft")));
        // `contentInnerCap`, not `bubble.width` (see
        // wrappedBodiesHaveStableIncubationWidths).
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

        // Video cards: a 72% column cap with hard bounds and a control-surface
        // floor; file cards keep a bounded width.
        QVERIFY(delegate.contains(
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
        // Match the expression, not the whole (wrapped) declaration line.
        QVERIFY(directBlock.contains(
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
        // Links open through the controlled web-open path; mention links
        // route to the member profile.
        QVERIFY(delegate.contains(QStringLiteral("app.media.openWebUrl(link)")));
        QVERIFY(delegate.contains(QStringLiteral("mention:")));
        QVERIFY(delegate.contains(QStringLiteral("id: replyBox")));
        // This delegate's own bar is the thread panel's `threadActionBar`;
        // the room timeline uses the one shared instance in TimelinePane.qml.
        QVERIFY(delegate.contains(QStringLiteral("id: messageActionBar")));
        QVERIFY(delegate.contains(QStringLiteral("id: previewLoader")));
        QVERIFY(delegate.contains(QStringLiteral("id: imageComponent")));
        // Reactions open the view-shared picker via the snapshotted event id.
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

    // A stuck send is cancellable and a media send shows real upload progress.
    void stuckSendsCanBeCancelledAndMediaUploadsShowRealProgress()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());

        // 1. The cancel offer is the model's answer (a backend with no send
        //    queue has nothing to abort) and covers Failed as well as Sending.
        QVERIFY(delegate.contains(QStringLiteral("objectName: \"cancelSendLink\"")));
        QVERIFY(delegate.contains(QStringLiteral("root.canCancelSendAt(index)")));
        QVERIFY(delegate.contains(
            QStringLiteral("root.timelineModel.canCancelSend(")));
        QVERIFY(delegate.contains(
            QStringLiteral("root.timelineModel.cancelSend(")));

        // 2. `index` is only read inside the built tree, never in a root-level
        //    creation-time binding, so canCancelSendAt is a function.
        QVERIFY(delegate.contains(
            QStringLiteral("function canCancelSendAt(viewRow) {")));

        // 3. -1 means "uploading, extent unknown" and renders as the
        //    indeterminate sweep, not a 0% bar.
        QVERIFY(delegate.contains(
            QStringLiteral("objectName: \"uploadProgressLoader\"")));
        QVERIFY(delegate.contains(
            QStringLiteral("indeterminate: root.uploadProgress < 0")));
        // Normalised once, so a model without the role reads as unknown.
        QVERIFY(delegate.contains(QStringLiteral(
            "model.uploadProgress === undefined ? -1 : model.uploadProgress")));
        // Only media rows show the bar.
        QVERIFY(delegate.contains(QStringLiteral("&& root.mediaRowBody")));
    }

    // Both composers claim the editor ShortcutOverride using ids from the
    // registry; without it Ctrl+B in the thread composer reached the window.
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

        // The registry is the one place that knows editor-context actions;
        // composers must not re-list them.
        for (const QString &src : { bar, thread }) {
            QVERIFY2(!src.contains(QStringLiteral("\"composer.italic\"")),
                     "hand-listed editor ids are back; a seventh editor "
                     "shortcut would work in one composer and not the other");
        }

        // The thread composer applies the format, not just swallows the key.
        QVERIFY(thread.contains(QStringLiteral("applyThreadFormat")));
    }

    // Every surface rendering bridge bytes listens for mediaRetryable, which
    // MediaBridge emits when it sweeps a transient failure mark; otherwise a
    // failed image stays on its fallback until the binding is rebuilt.
    void everyBridgeBackedSurfaceHearsTheRetryableSweep()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());

        // A surface is bridge-backed exactly when it handles mediaFetchFailed;
        // derived from the source so new surfaces are covered.
        const int failedHandlers =
            delegate.count(QStringLiteral("function onMediaFetchFailed("));
        QVERIFY2(failedHandlers >= 3,
                 qPrintable(QStringLiteral("expected the bridge-backed "
                                           "surfaces, found %1")
                                .arg(failedHandlers)));
        const int retryHandlers =
            delegate.count(QStringLiteral("function onMediaRetryable("));
        QCOMPARE(retryHandlers, failedHandlers);

        // The two surfaces that always had it keep it.
        for (const QString &file : { QStringLiteral("Avatar.qml"),
                                     QStringLiteral("MediaListThumbnail.qml") }) {
            const QString src = read(file);
            QVERIFY(!src.isEmpty());
            QVERIFY2(src.contains(QStringLiteral("onMediaRetryable")),
                     qPrintable(file + QStringLiteral(" lost its recovery "
                                                      "channel")));
        }
    }

    // The video overlay's taps exclude the control bar: `overlayTap` is a
    // sibling covering the bar, whose background and gaps do not accept the
    // press, so a near-miss toggled playback and a double click closed the
    // overlay.
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
        // Each handler's own body, sliced by brace depth, so one handler's
        // check cannot satisfy the other's.
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
        // Both gestures: single tap (play/pause) and double tap (close).
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

    // Selection mode suspends every row surface. Grab permissions do not help:
    // pointer events reach child handlers first. Derived: every TapHandler and
    // MouseArea in the delegate is gated on `rowActionsEnabled`, is the
    // selection handler, or is one of the two documented exceptions.
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
            // The block by brace depth; a fixed window is defeated by comments
            // inside it.
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
            // Exceptions: the right-click menu (which also leaves selection
            // mode) and the failed local echo's retry/cancel.
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

    // A file that asks for an mxc image also listens for mediaCached:
    // mxcImageSource returns "" on a miss and dispatches a fetch, so a binding
    // with no counter bumped from that signal stays blank. Derived from the
    // call sites.
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

    // Every binding calling answerTexts() also reads `answerRevision`: a
    // ListModel setProperty() edit registers no dependency. Without it `dirty`
    // stayed false for a poll typed only into answer rows, and closing
    // discarded the draft without asking. Derived, so new bindings are
    // covered.
    void everyPollBindingOnAnswerTextTouchesTheRevision()
    {
        const QString src = read(QStringLiteral("CreatePollDialog.qml"));
        QVERIFY(!src.isEmpty());
        QVERIFY2(src.contains(QStringLiteral("property int answerRevision")),
                 "the revision counter is gone, so this case is testing "
                 "nothing");

        // Collect each `readonly property ... :` binding and its expression,
        // up to the next declaration or a closing brace at its depth.
        const QStringList lines = src.split(QLatin1Char('\n'));
        static const QRegularExpression decl(
            QStringLiteral("^\\s*(readonly\\s+)?property\\s+\\S+\\s+(\\w+)\\s*:"));
        int checked = 0;
        for (int i = 0; i < lines.size(); ++i) {
            const auto m = decl.match(lines.at(i));
            if (!m.hasMatch())
                continue;
            const QString name = m.captured(2);
            // This line plus following lines until the next declaration or a
            // line that closes the block.
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

    // Bindings reaching a Q_INVOKABLE register no dependency, so their answer
    // is frozen at component creation.

    /// The innermost `{ … }` block containing `pos`: a `.count` read in a
    /// sibling binding proves nothing about this one.
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

    // The pop-out call window's index lookups (Q_INVOKABLEs) read the model's
    // notifying `count`, or they freeze at their first answer; e.g.
    // `fillShareShown` never dropped a share that had ended. Derived: every
    // lookup in the file is checked.
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
            // Both forms here are braced property bodies, so each needs a
            // count read.
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

    // The follow checkbox must not bind `checked` and also write it: a user
    // toggle destroys the binding, and setSubscribed() is asynchronous and
    // can fail. It reads the notifying `subscriptions` list, not the
    // isSubscribed() invokable.
    void theFollowCheckboxKeepsFollowingTheStoreAfterAClick()
    {
        const QString src = read(QStringLiteral("PolicyListDialog.qml"));
        QVERIFY(!src.isEmpty());
        const int at = src.indexOf(QStringLiteral("policyFollowCheck"));
        QVERIFY2(at >= 0, "the follow checkbox is gone — re-anchor this case");
        const QString block = blockAround(src, at);
        QVERIFY2(!block.isEmpty(), "could not slice the checkbox");
        // Match the call with its receiver's dot, not the bare name a comment
        // may contain.
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

    // The viewer's thumbnail strip re-resolves pictures that arrive later
    // (mediaSource() returns "" on a miss); otherwise uncached tiles stay
    // empty.
    void theViewerThumbnailStripResolvesMediaThatArrivesLate()
    {
        const QString src = read(QStringLiteral("ImageViewerOverlay.qml"));
        QVERIFY(!src.isEmpty());
        const int at = src.indexOf(QStringLiteral("id: thumbImage"));
        QVERIFY2(at >= 0, "the strip's thumbnail image is gone — re-anchor "
                          "this case");
        const QString block = blockAround(src, at);
        QVERIFY2(!block.isEmpty(), "could not slice the thumbnail image");
        // Check the `source:` expression itself: the Connections handler also
        // names the counter, so a whole-block check passes without the read.
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

    // The bubble's tap excludes the action bar it opens: the bar's padding
    // and gaps do not accept the press, so a near-miss toggled the pin and
    // closed the bar.
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

    // Records behind invokables are re-read, not bound: the Home greeting
    // (renames) and the Send Later notice (a room encrypted mid-session holds
    // scheduled messages only in memory).
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
    // The rail width saver restores the `preferredWidth` binding after writing
    // a dragged value: SplitView writes a number while dragging, which would
    // leave the rail off the scaled stops after an interface-size change.
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

        // ...and only a real divider drag writes the stored width: the live
        // binding re-evaluates on interface-size changes, which must not
        // overwrite what the user dragged to.
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
