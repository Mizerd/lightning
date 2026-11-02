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

    struct TextElement {
        int line = 0;
        QStringList properties;

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
            depth = 0;
            QString current;
            for (int i = open; i <= end && i < source.size(); ++i) {
                const QChar c = source.at(i);
                if (c == QLatin1Char('{')) {
                    ++depth;
                    if (depth > 1) {
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

    static QString stateActivityBlock(const QString &delegate)
    {
        const int start = delegate.indexOf(QStringLiteral("id: stateActivity"));
        if (start < 0) return {};
        const int end = delegate.indexOf(
            QStringLiteral("\n    ColumnLayout {\n        id: layout"), start);
        if (end < start) return {};
        return delegate.mid(start, end - start);
    }

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
    void savedMessageLandingAndActionsRemainDistinct()
    {
        const QString main = read(QStringLiteral("Main.qml"));
        const QString saved = read(QStringLiteral("SavedMessagesPanel.qml"));
        QVERIFY(!main.isEmpty() && !saved.isEmpty());
        const int reset = main.indexOf(QStringLiteral("function onRoomTimelineReady(roomId)"));
        QVERIFY(reset >= 0);
        QVERIFY(main.mid(reset, 700).contains(QStringLiteral(
            "app.pagination.jumpToEvent(eventId)")));
        QVERIFY(main.contains(QStringLiteral("function onSaveSucceeded()")));
        QVERIFY(saved.contains(QStringLiteral("iconName: \"forum\"")));
        QVERIFY(saved.contains(QStringLiteral("iconName: \"home\"")));
        QVERIFY(saved.contains(QStringLiteral(
            "ToolTip.text: qsTr(\"Open thread root in room\")")));
        QVERIFY(saved.contains(QStringLiteral(
            "ToolTip.text: qsTr(\"Open room\")")));
    }

    void presetDescriptionMatchesCozySmallAvatarRhythm()
    {
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(settings.contains(QStringLiteral("Nheko's close row spacing")));
        QVERIFY(!settings.contains(QStringLiteral("tight spacing, and threads")));
    }

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
        QVERIFY2(handlersSeen >= 10,
                 qPrintable(QStringLiteral("only %1 onMediaCached handlers found")
                                .arg(handlersSeen)));
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "onMediaCached assigns a bound `source` (this strands the "
                     "Image on the first image it ever loaded) in: %1")
                                .arg(offenders.join(QStringLiteral(", ")))));
    }

    void bannerPreviewsReAskWhenTheirPageIsComplete()
    {
        const struct { const char *file; const char *id; } previews[] = {
            { "SettingsScreen.qml", "ownBannerImage" },
            { "SpaceSettingsDialog.qml", "bannerPreview" },
        };
        for (const auto &p : previews) {
            const QString source = read(QString::fromLatin1(p.file));
            QVERIFY2(!source.isEmpty(), p.file);
            const int image = source.indexOf(
                QStringLiteral("id: %1\n").arg(QString::fromLatin1(p.id)));
            QVERIFY2(image >= 0, p.file);
            const int connections = source.indexOf(
                QStringLiteral("Connections {"), image);
            QVERIFY2(connections > image, p.file);
            const QString between = source.mid(image, connections - image);
            QVERIFY2(between.contains(QStringLiteral("Component.onCompleted"))
                         && between.contains(QStringLiteral("resolveTick++")),
                     p.file);
        }
    }

    void ctrlQQuitsEvenWithCloseToTrayOn()
    {
        const QString main = read(QStringLiteral("Main.qml"));
        QVERIFY(!main.isEmpty());
        const int quitAction =
            main.indexOf(QStringLiteral("sequenceFor(\"app.quit\")"));
        QVERIFY2(quitAction > 0,
                 "the quit Shortcut must take its sequence from the registry");
        const QString shortcut = bracedBody(
            main, main.lastIndexOf(QStringLiteral("Shortcut {"), quitAction));
        QVERIFY2(!shortcut.isEmpty(), "the quit Shortcut must still exist");
        QVERIFY(shortcut.contains(QStringLiteral("quitRequested = true")));
        QVERIFY(shortcut.contains(QStringLiteral("Qt.quit()")));
        const QString closing = bracedBody(
            main, main.indexOf(QStringLiteral("onClosing:")));
        QVERIFY(!closing.isEmpty());
        QVERIFY(closing.contains(
            QStringLiteral("app.closeHidesWindow(window.quitRequested)")));

        QVERIFY2(main.contains(QStringLiteral("onApplicationQuitIntended")),
                 "the window must honour a deliberate quit from C++, or "
                 "close-to-tray silently vetoes an update");
        const QString intended = bracedBody(
            main, main.indexOf(QStringLiteral("onApplicationQuitIntended")));
        QVERIFY2(intended.contains(QStringLiteral("quitRequested = true")),
                 "the announced quit must set the same flag Ctrl+Q sets");
    }

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
        QVERIFY(body.contains(QStringLiteral("Window.Hidden")));
        QVERIFY(body.contains(QStringLiteral("window.visible = true")));
        QVERIFY(body.contains(QStringLiteral("Window.Minimized")));
        QVERIFY(body.contains(QStringLiteral("lastOnScreenVisibility")));
        QVERIFY(body.contains(QStringLiteral("window.raise()")));
        QVERIFY(body.contains(QStringLiteral("window.requestActivate()")));

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

        const int vis = main.indexOf(QStringLiteral("onVisibilityChanged:"));
        QVERIFY(vis > 0);
        const QString visBody = bracedBody(main, vis);
        QVERIFY(visBody.contains(
            QStringLiteral("window.lastOnScreenVisibility = window.visibility")));
    }

    void windowGeometryIsRestoredInBindingsAndFlushedOnClose()
    {
        const QString main = read(QStringLiteral("Main.qml"));
        QVERIFY(!main.isEmpty());
        QVERIFY(main.contains(QStringLiteral(
            "readonly property rect startupGeometry: app.restorableWindowGeometry")));
        for (const QString &prop : { QStringLiteral("width:"),
                                    QStringLiteral("height:") }) {
            QVERIFY2(main.contains(prop + QStringLiteral(" hasStartupGeometry")),
                     qPrintable(prop));
        }
        QVERIFY2(!main.contains(QStringLiteral("x: hasStartupGeometry")),
                 "window x is a binding again: it will fight the user's drag "
                 "and re-centre on any screen-metric change");
        QVERIFY2(!main.contains(QStringLiteral("y: hasStartupGeometry")),
                 "window y is a binding again");
        QVERIFY2(main.contains(QStringLiteral("visible: false")),
                 "the window is shown before startup placement is applied, so "
                 "the user watches it jump into position");
        const QString completed = bracedBody(
            main, main.indexOf(QStringLiteral("Component.onCompleted:")));
        QVERIFY(!completed.isEmpty());
        QVERIFY2(completed.contains(QStringLiteral("applyStartupPlacement()")),
                 "startup placement is never applied");
        QVERIFY(completed.indexOf(QStringLiteral("applyStartupPlacement()"))
                < completed.indexOf(QStringLiteral("visible = true")));
        QVERIFY(!completed.contains(QStringLiteral("window.width")));
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

        QVERIFY2(body.contains(QStringLiteral("viewportFillLastHeight")),
                 "the budget is spent on attempts rather than on attempts "
                 "that failed to make the content taller");
        QVERIFY2(body.contains(QStringLiteral("viewportFillLastRows")),
                 "nothing distinguishes a page that advanced the pagination "
                 "cursor from one that did nothing, so a collapsed run stops "
                 "the reader dead and expanding it by hand is the only way on");
        QVERIFY2(body.contains(QStringLiteral("maxInvisibleFillRetries")),
                 "invisible progress has no bound of its own");

        QVERIFY2(body.contains(QStringLiteral("viewportFillLastEmptyPages")),
                 "nothing compares the controller's completed-empty-page "
                 "counter across attempts, so a page the filter emptied is "
                 "indistinguishable from a dispatch that went nowhere and "
                 "spends the small no-progress bound meant for the latter");
        QVERIFY2(body.contains(QStringLiteral("maxEmptyFillPages")),
                 "a run of filtered history has no bound of its own");

        const int busyGuard = body.indexOf(QStringLiteral("app.pagination.busy"));
        QVERIFY2(busyGuard >= 0,
                 "calls made while a page is in flight still spend the fill "
                 "budget, so redundant geometry signals exhaust it before any "
                 "page has had a chance to help");
        QVERIFY2(busyGuard < budget,
                 "the in-flight guard must come before the budget is spent");
    }

    void roomListWidthIsSavedWhenTheDragEnds()
    {
        const QString shell = read(QStringLiteral("MainScreen.qml"));
        QVERIFY(!shell.isEmpty());
        const int target = shell.indexOf(
            QStringLiteral("target: roomsPanel.SplitView.view"));
        QVERIFY2(target > 0, "the drag-release trigger must exist");
        const int conn = shell.lastIndexOf(QStringLiteral("Connections {"),
                                           target);
        QVERIFY(conn > 0);
        const QString body = bracedBody(shell, conn);
        QVERIFY(body.contains(QStringLiteral("onResizingChanged")));
        QVERIFY(body.contains(QStringLiteral("widthSaver.restart()")));
        const int guarded = shell.indexOf(QStringLiteral(
            "onWidthChanged: if (!SplitView.view.resizing && widthSaver.dragged)"));
        QVERIFY(guarded > 0);
        QVERIFY(shell.mid(guarded, 140).contains(QStringLiteral("widthSaver.restart()")));
    }

    void imageViewerClosesInstantlyOutsideThePictureAndZoomsOnIt()
    {
        const QString viewer = read(QStringLiteral("ImageViewerOverlay.qml"));
        QVERIFY(!viewer.isEmpty());

        QCOMPARE(viewer.count(QStringLiteral("onTapped: viewer.close()")), 1);

        QVERIFY2(viewer.contains(QStringLiteral("viewer.toggleZoomAt")),
                 "the picture no longer zooms on click");
        QVERIFY2(viewer.contains(QStringLiteral("x < left || x > left + iw")),
                 "the image tap has no band check, so the margin around a "
                 "fitted picture is still part of the picture's hit target "
                 "and a click there zooms instead of closing");

        QVERIFY(!viewer.contains(QStringLiteral("onDoubleTapped")));
        QVERIFY(!viewer.contains(QStringLiteral("exclusiveSignals")));

        QVERIFY(viewer.contains(QStringLiteral("WheelHandler")));
        QVERIFY(viewer.contains(QStringLiteral("zoomStep(1.2)")));
    }

    void chatWideDropQueuesAttachmentsWithRichPreviews()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!pane.isEmpty());
        QVERIFY(pane.contains(QStringLiteral("id: chatDropArea")));
        QVERIFY(pane.contains(QStringLiteral(
            "app.composer.addDroppedAttachments(drop.hasUrls ? drop.urls : [], key)")));
        QVERIFY(pane.contains(QStringLiteral("messageComposer.focusStagedAttachmentSend()")));
        QVERIFY(pane.contains(QStringLiteral(
            "keys: [\"text/uri-list\", \"application/vnd.portal.filetransfer\",")));
        QVERIFY(pane.contains(QStringLiteral(
            "width: root.threadSurfaceOpen\n"
            "               ? (sideSurface.threadBeside ? root.width - sideSurface.width - 1\n"
            "                                           : 0)")));
        QVERIFY(pane.contains(QStringLiteral(
            "? (sideSurface.beside ? root.width - sideSurface.width - 1 : 0)")));
        QVERIFY(!pane.contains(QStringLiteral(
            "chatDropArea\n        anchors.fill: parent")));

        const QString composer = read(QStringLiteral("MessageComposerBar.qml"));
        QVERIFY(!composer.isEmpty());
        QVERIFY(composer.contains(QStringLiteral("isGifChip")));
        QVERIFY(composer.contains(QStringLiteral("isVideoChip")));
        QVERIFY(composer.contains(QStringLiteral(
            "onClicked: app.composer.attachments.removeAt(index)")));
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

    void unsanitizedServerTextIsAlwaysPlainText()
    {
        static const QRegularExpression remotelyChosen(
            QStringLiteral(
                "(topic|roomname|spacename|displayname|sendername|membername"
                "|authorname|channelname|dmname|targetname|inviter|invitee"
                "|reactionkey|statustext|statusmessage|alias|packname"
                "|stickername|devicename|sessionname|filename|attribution"
                "|\\bsender\\b|\\buserid\\b|\\bbody\\b|\\bpreview\\b"
                "|\\bname\\b|\\blabel\\b|\\bsubtitle\\b)"),
            QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression ourOwnText(QStringLiteral(
            "^text\\s*:\\s*(qsTr\\(.*\\)|qsTrId\\(.*\\)"
            "|\"[^\"]*\"|'[^']*')\\s*$"));
        static const QRegularExpression bareAlias(QStringLiteral(
            "^text\\s*:\\s*[A-Za-z_][A-Za-z0-9_]*\\.([A-Za-z_][A-Za-z0-9_]*)\\s*$"));

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
                const QString declared =
                    element.property(QStringLiteral("textFormat"));
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
        QVERIFY(!pane.contains(QStringLiteral("header: Item {")));
        QVERIFY(pane.contains(QStringLiteral("objectName: \"paginationHeader\"")));
        QVERIFY(pane.contains(QStringLiteral("anchors.top: parent.top")));
        QVERIFY(pane.contains(QStringLiteral("app.pagination.restoreConversation(")));
        QVERIFY(pane.contains(QStringLiteral("saveScrollAnchor(")));
        QVERIFY(pane.contains(QStringLiteral("eventIdAtViewRow(row)")));

        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.contains(
            QStringLiteral("app.pagination.jumpToEvent(model.replyToEventId")));
        QVERIFY(!delegate.contains(QStringLiteral("app.pagination.highlightedEventId")));
        QVERIFY(delegate.contains(QStringLiteral("navigateToEvent(")));
        QVERIFY(delegate.contains(QStringLiteral("navigationHighlightEventId")));
        QVERIFY(pane.contains(QStringLiteral("navigateToEvent")));
        QVERIFY(pane.contains(QStringLiteral("navigationHighlightEventId")));
        const QString threadPanel = read(QStringLiteral("ThreadPanel.qml"));
        QVERIFY(threadPanel.contains(QStringLiteral("navigateToEvent")));
        QVERIFY(threadPanel.contains(QStringLiteral("navigationHighlightEventId")));
        QVERIFY(pane.contains(QStringLiteral("viewportFillCheckScheduled")));
        QVERIFY(pane.contains(QStringLiteral("Qt.callLater(function()")));
        QVERIFY(pane.contains(QStringLiteral("app.pagination.requestViewportFill()")));
        QVERIFY(pane.contains(QStringLiteral("function checkNearTopEdge(")));
        QVERIFY(pane.contains(QStringLiteral("nearTopArmed")));

        QVERIFY(pane.contains(QStringLiteral("function distanceFromTop()")));
        QVERIFY(pane.contains(QStringLiteral(
            "return wheelMaxY() - contentY")));
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
        QVERIFY(pane.contains(QStringLiteral("fromTop <= nearTopEnterDistance")));
        QVERIFY(pane.contains(QStringLiteral("fromTop >= nearTopExitDistance")));
        QVERIFY(pane.contains(QStringLiteral(
            "<= timeline.nearTopEnterDistance")));
        QVERIFY(pane.contains(QStringLiteral("nearTopRequestDistance")));
        QVERIFY(pane.contains(QStringLiteral("fromTop <= 1")));
        QVERIFY(pane.contains(QStringLiteral(
            "fromTop < nearTopRequestDistance - 1")));
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

    void animatedGifSkeletonUsesActiveRendererState()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("objectName: \"imageSkeleton\"")));
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
        QVERIFY(thread.contains(QStringLiteral("threadMediaButton")));
        QVERIFY2(!thread.contains(QStringLiteral("threadGifButton")),
                 "the separate thread GIF button is back; the merged picker "
                 "has one entry point");
        QVERIFY(room.contains(QStringLiteral("composerMediaButton")));
        QVERIFY2(!room.contains(QStringLiteral("composerGifButton")),
                 "the separate room GIF button is back");
        for (const QString &host : { room, thread }) {
            QVERIFY(host.contains(QStringLiteral("offerKindTabs")));
            QVERIFY(host.contains(QStringLiteral("onKindRequested")));
        }

        const QString picker = read(QStringLiteral("GifPicker.qml"));
        QVERIFY(!picker.isEmpty());
        QVERIFY(room.contains(QStringLiteral(
            "app.gifSend.sendToRoom(app.currentRoomId, result,")));
        QVERIFY(thread.contains(QStringLiteral("app.gifSend.sendToThread(")));
        QVERIFY(thread.contains(QStringLiteral("app.thread.rootEventId")));
        QVERIFY(!picker.contains(QStringLiteral("sendToRoom")));
        QVERIFY(!picker.contains(QStringLiteral("sendTextMessage")));
    }

    void gifAutoplayAndSettingsWired()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString picker = read(QStringLiteral("GifPicker.qml"));
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(delegate.contains(QStringLiteral("gifMode: MotionPolicy.mode")));
        QVERIFY(delegate.contains(QStringLiteral("MotionPolicy.plays(gifHovered)")));
        QVERIFY(!delegate.contains(QStringLiteral("app.settings.gifAutoplay")));
        QVERIFY(delegate.contains(QStringLiteral("app.settings.videoPrefetchEnabled")));
        QVERIFY(delegate.contains(QStringLiteral("app.settings.videoPrefetchMaxMb")));
        QVERIFY(settings.contains(QStringLiteral("app.settings.videoPrefetchEnabled = checked")));
        QVERIFY(settings.contains(QStringLiteral("app.settings.videoPrefetchMaxMb = Math.round(value)")));
        QVERIFY(picker.contains(QStringLiteral("MotionPolicy.plays(tileHover.hovered)")));
        QVERIFY(settings.contains(QStringLiteral("app.settings.gifAutoplay = currentValue")));
        QVERIFY(settings.contains(QStringLiteral("app.settings.gifSafeSearch = currentValue")));
        QVERIFY(settings.contains(QStringLiteral("app.settings.gifPreferredProvider = currentValue")));
        QVERIFY(settings.contains(QStringLiteral("app.settings.storeRecentGifs = checked")));
        QVERIFY(settings.contains(QStringLiteral("providerConfigured(\"giphy\")")));
        QVERIFY(settings.contains(QStringLiteral(
            "GIF searches are sent directly to the ")));
        QVERIFY(settings.contains(QStringLiteral("gifClearConfirm.open(\"favorites\")")));
        QVERIFY(settings.contains(QStringLiteral("app.gif.favorites.clearAll()")));
        QVERIFY(settings.contains(QStringLiteral("objectName: \"starredGifsSummaryLabel\"")));
        QVERIFY(settings.contains(QStringLiteral("app.gif.starredStore.count")));
        QVERIFY(settings.contains(QStringLiteral("app.gif.starredStore.totalBytes")));
        QVERIFY(settings.contains(QStringLiteral("kept on this ")));
        QVERIFY(settings.contains(QStringLiteral("device only and removed ")));
        QVERIFY(settings.contains(QStringLiteral(
            "when you sign out of this ")));
        QVERIFY(settings.contains(QStringLiteral("starredGifsClearConfirm.open()")));
        QVERIFY(settings.contains(QStringLiteral("app.gif.starredStore.clearAll()")));
    }

    void threadRootUsesSummaryCard()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        QVERIFY(delegate.contains(QStringLiteral("ThreadSummaryCard {")));
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
            "onActivated: root.showThread(")));
        QVERIFY(delegate.contains(QStringLiteral(
            "app.showThread(app.currentRoomId, rootId)")));
        QVERIFY(!delegate.contains(QStringLiteral("reply(s) in thread")));
        QVERIFY(!delegate.contains(QStringLiteral("· in thread")));

        const QString cardQml = read(QStringLiteral("ThreadSummaryCard.qml"));
        QVERIFY(!cardQml.isEmpty());
        QVERIFY(cardQml.contains(QStringLiteral("signal activated()")));
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
        QVERIFY(cardQml.contains(QStringLiteral(
            "replyCount > 0 ? qsTr(\"%n reply(s)\", \"\", replyCount)")));
    }

    void stateActivityUsesNeutralGroupedPresentation()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("stateGroupEntries")));
        QVERIFY(delegate.contains(QStringLiteral(
            "visible: !root.isVirtualRow && !root.isStateActivity")));
        const QString activity = read(QStringLiteral("RoomActivityDelegate.qml"));
        QVERIFY(!activity.contains(QStringLiteral("AppTheme.cardElevated")));
        QVERIFY(activity.contains(QStringLiteral("summaryRow")));
        QVERIFY(activity.contains(QStringLiteral("modelData.description")));
        QVERIFY(activity.contains(QStringLiteral("model: expandedColumn.visible ? root.entries")));
        QVERIFY(!activity.contains(QStringLiteral("linkPreviews")));
        QVERIFY(!activity.contains(QStringLiteral("messageActions")));
    }

    void stateActivityQualifiesListViewViewOnNestedControls()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = stateActivityBlock(delegate);
        QVERIFY(!block.isEmpty());
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
        QVERIFY(delegate.contains(
            QStringLiteral("app.settings.showMembershipEvents")));
        QVERIFY(delegate.contains(
            QStringLiteral("app.settings.showProfileChangeEvents")));
        QVERIFY(delegate.contains(QStringLiteral("naturalImplicitHeight")));
        QVERIFY(delegate.contains(QStringLiteral("dividerSuppressed")));
        QVERIFY(delegate.contains(QStringLiteral("deletedFollower")));
        QVERIFY(delegate.contains(QStringLiteral(
            "(!roomActivityVisible || suppressedAsThreadRoot"
            " || dividerSuppressed"
            "\n         || deletedFollower || !threadSearchVisible) ? 0")));
        QVERIFY(settings.contains(QStringLiteral("Show room activity")));
        QVERIFY(settings.contains(QStringLiteral(
            "onToggled: app.settings.showRoomActivity = checked")));
    }

    void undecryptableRowsExposeRetryAndSecurityActions()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("Retry decryption")));
        QVERIFY(delegate.contains(QStringLiteral(
            "root.timelineModel.retryDecryption()")));
        QVERIFY(delegate.contains(QStringLiteral(
            "app.showSettingsSection(\"security\")")));
        QVERIFY(!delegate.contains(QStringLiteral("model.sessionId")));
        QVERIFY(!delegate.contains(QStringLiteral("model.ciphertext")));
    }

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
        QVERIFY(settings.contains(QStringLiteral("var all = app.sessionDevices")));
        QVERIFY(settings.contains(QStringLiteral("var f = sessionFilter.current")));
        for (const char *branch : { "f === \"current\" && d.isCurrent === true",
                                    "f === \"verified\" && d.trusted === true",
                                    "d.trusted !== true",
                                    "f === \"unverified\"" })
            QVERIFY2(settings.contains(QLatin1String(branch)), branch);
        QVERIFY(settings.contains(QStringLiteral("This session")));
        QVERIFY(settings.contains(QStringLiteral("Not verified")));

        const QString code = withoutComments(settings);
        for (const char *raw : { "modelData.verified", "d.verified",
                                 "modelData.crossSigned", "d.crossSigned" })
            QVERIFY2(!code.contains(QLatin1String(raw)), raw);
        QVERIFY(code.contains(QStringLiteral("label: modelData.trusted === true")));
        QVERIFY(code.contains(QStringLiteral("d => d.trusted === true")));
        QVERIFY(!settings.contains(QStringLiteral("is not supported yet")));
        QVERIFY(settings.contains(
            QStringLiteral("signOutOtherSessionsButton")));
        QVERIFY(settings.contains(QStringLiteral("sessionSignOutButton_")));
        QVERIFY(settings.contains(QStringLiteral(
            "visible: modelData.isCurrent !== true")));
        QVERIFY(!settings.contains(QStringLiteral("accessToken")));
        QVERIFY(!settings.contains(QStringLiteral("access_token")));
    }

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
        QVERIFY2(handler.contains(QStringLiteral("m_sessionTrustState")),
                 "the scanned extent does not contain the label it guards");
        QVERIFY2(handler.contains(QStringLiteral("Cross-signing unavailable")),
                 "the scanned extent is missing the chain's FIRST arm");
        QVERIFY2(handler.contains(QStringLiteral("Not verified")),
                 "the scanned extent is missing the chain's LAST arm");

        const QString body = withoutComments(handler);
        QVERIFY2(body.contains(QStringLiteral("deviceCrossSigned")),
                 "the session trust label must read is_cross_signed_by_owner()");
        QVERIFY2(!body.contains(QStringLiteral("deviceVerified")),
                 "the session trust label is reached from Device::is_verified(), "
                 "which matrix-sdk makes a constant true for our own device");
    }

    void recoveryInputIsMaskedClearedAndHonest()
    {
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        const QString entry = read(QStringLiteral("RecoveryKeyEntry.qml"));
        QVERIFY(entry.contains(QStringLiteral("echoMode: TextInput.Password")));
        QVERIFY(settings.contains(QStringLiteral("Recovery key or passphrase"))
                || entry.contains(QStringLiteral("Recovery key or passphrase")));
        QVERIFY(entry.contains(QStringLiteral("field.text = \"\"")));
        QVERIFY(settings.contains(QStringLiteral("app.refreshCryptoHealth()"))
                || entry.contains(QStringLiteral("app.refreshCryptoHealth()")));
        QVERIFY(!settings.contains(QStringLiteral("Set up backup")));

        const QString card = read(QStringLiteral("CrossSigningSetupCard.qml"));
        QVERIFY(card.contains(QStringLiteral("objectName: \"setUpCrossSigningButton\"")));
        QVERIFY(card.contains(
            QStringLiteral("app.backup.runAction(\"setup_cross_signing\")")));
        QVERIFY(card.contains(
            QStringLiteral("visible: root.setupState === \"not_set_up\"")));
        QVERIFY(card.contains(
            QStringLiteral("app.backup.runAction(\"reset_cross_signing\")")));
        QVERIFY(card.contains(QStringLiteral("pendingConfirm !== \"reset\"")));
        QVERIFY(settings.contains(QStringLiteral("CrossSigningSetupCard {")));
        QVERIFY(!card.contains(QStringLiteral("secretStorageAvailable")));
        QVERIFY(card.contains(QStringLiteral("\"recovery_key_required\"")));
        const QString replaceCall =
            QStringLiteral("app.backup.runCrossSigning(app.backup.lastAction, \"\", true)");
        QCOMPARE(card.count(replaceCall), 1);
        const qsizetype confirmAt =
            card.indexOf(QStringLiteral("if (root.pendingConfirm !== \"replace\")"));
        QVERIFY(confirmAt >= 0);
        QVERIFY(confirmAt < card.indexOf(replaceCall));
        QCOMPARE(card.count(QStringLiteral("Qt.openUrlExternally(")), 1);
        QVERIFY(card.contains(QStringLiteral("Qt.openUrlExternally(app.backup.approvalUrl)")));

        QFile controller(QStringLiteral(QML_DIR "/../src/crypto/BackupController.cpp"));
        QVERIFY(controller.open(QIODevice::ReadOnly));
        const QString controllerSrc = QString::fromUtf8(controller.readAll());
        QVERIFY(card.contains(QStringLiteral("app.backup.runCrossSigning(")));
        QVERIFY(controllerSrc.contains(QStringLiteral("void BackupController::runCrossSigning(")));
        QVERIFY(controllerSrc.contains(QStringLiteral("runCrossSigning(action, QString(), false)")));
        QVERIFY(controllerSrc.contains(QStringLiteral("m_client->crossSigningAction(")));
        QFile clientHeader(QStringLiteral(QML_DIR "/../src/matrix/MatrixClient.h"));
        QVERIFY(clientHeader.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(clientHeader.readAll())
                    .contains(QStringLiteral("virtual quint64 crossSigningAction(")));
        QFile rustClient(QStringLiteral(QML_DIR "/../src/matrix/RustSdkMatrixClient.cpp"));
        QVERIFY(rustClient.open(QIODevice::ReadOnly));
        const QString rustClientSrc = QString::fromUtf8(rustClient.readAll());
        QVERIFY(rustClientSrc.contains(QStringLiteral("RustSdkMatrixClient::crossSigningAction(")));
        QVERIFY(rustClientSrc.contains(QStringLiteral("mx_rust_cross_signing_action(")));
        QFile ffi(QStringLiteral(QML_DIR "/../rust/src/lib.rs"));
        QVERIFY(ffi.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(ffi.readAll()).contains(QStringLiteral(
            "uia::start_cross_signing(bridge, &action, recovery_key, replace_recovery_key != 0, op_id)")));
        QFile bridge(QStringLiteral(QML_DIR "/../rust/src/uia.rs"));
        QVERIFY(bridge.open(QIODevice::ReadOnly));
        const QString bridgeSrc = QString::fromUtf8(bridge.readAll());
        QVERIFY(bridgeSrc.contains(QStringLiteral("bootstrap_cross_signing(")));
        QVERIFY(bridgeSrc.contains(QStringLiteral("StoragePlan::EnableRecovery) => recovery")));
        QVERIFY(bridgeSrc.contains(QStringLiteral(".enable()")));
    }

    void unreadNavigationUsesSdkMarkerAndBottomThreshold()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(delegate.contains(QStringLiteral("objectName: \"unreadDivider\"")));
        QVERIFY(delegate.contains(QStringLiteral("qsTr(\"New messages\")")));
        QVERIFY(pane.contains(QStringLiteral("objectName: \"jumpToLatestButton\"")));
        QVERIFY(pane.contains(QStringLiteral("!timeline.stickToBottom")));
        QVERIFY(pane.contains(QStringLiteral("function atBottomEdge()")));
        QVERIFY(pane.contains(QStringLiteral("wheelMinY() + bottomFollowSlack")));
        QVERIFY(!pane.contains(QStringLiteral("contentHeight - 40")));
        QVERIFY(!pane.contains(QStringLiteral("contentY + height === contentHeight")));
    }

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
        QVERIFY(!touchpad.contains(QStringLiteral("captureViewAnchor()")));
        QVERIFY(touchpad.contains(QStringLiteral("scrollSettleTimer.restart()")));

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
        QString maintainGuard =
            pane.mid(guardStart, maintainEnd - guardStart);
        const int hold = maintainGuard.indexOf(QStringLiteral("if (heldDelta > 0) {"));
        QVERIFY2(hold > 0, "the newest-end hold moved or was renamed");
        QVERIFY(maintainGuard.lastIndexOf(QStringLiteral("insertedBelowAnchorHeight("), hold) > 0);
        const int holdEnd = maintainGuard.indexOf(QStringLiteral("return"), hold);
        QVERIFY(holdEnd > hold);
        const QString holdText = maintainGuard.mid(hold, holdEnd - hold);
        QCOMPARE(holdText.count(QStringLiteral("contentY ="))
                     + holdText.count(QStringLiteral("setPositionY(")), 1);
        maintainGuard.remove(hold, holdEnd - hold);
        QVERIFY(!maintainGuard.contains(QStringLiteral("contentY +=")));
        QVERIFY(!maintainGuard.contains(QStringLiteral("contentY =")));
        QVERIFY(!maintainGuard.contains(QStringLiteral("setPositionY(")));

        QVERIFY(pane.contains(QRegularExpression(QStringLiteral(
            "userScrollActive:\\s*moving \\|\\| scrollBarHeld \\|\\| wheelAnimating"
            "\\s*\\|\\| scrollSettleTimer\\.running"))));

        const int settled =
            pane.indexOf(QStringLiteral("function onWheelMotionSettled()"));
        QVERIFY(settled >= 0);
        const int settledEnd =
            pane.indexOf(QStringLiteral("scrollSettleTimer.restart()"), settled);
        QVERIFY(settledEnd > settled);
        const QString settledBlock = pane.mid(settled, settledEnd - settled);
        QVERIFY(settledBlock.contains(QStringLiteral("timeline.captureViewAnchor()")));
    }

    void theTimelinePaneReopensTimelinesWhenTheEventCacheIsCleared()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        const int at = pane.indexOf(QStringLiteral("function onEventCacheWasCleared()"));
        QVERIFY(at > 0);
        const QString block = pane.mid(at, 500);
        QVERIFY(block.contains(QStringLiteral("app.reloadCurrentRoomTimeline()")));
        QVERIFY(block.contains(QStringLiteral("app.thread.reopen()")));
        QVERIFY(pane.mid(pane.lastIndexOf(QStringLiteral("target:"), at), 40)
                    .contains(QStringLiteral("app.about")));
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
            "readonly property real avatarGutterWidth: (compactMode || smallAvatarMode)\n"
            "                                              ? 8 : (bubbleMode ? 44 : 40)")));
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

        QVERIFY(!delegate.contains(QStringLiteral("Qt.AlignRight")));
        QVERIFY(delegate.contains(QStringLiteral(
            "readonly property bool bubbleMode: timelineLayout === 1 && isDirectRoom")));
        QVERIFY(delegate.contains(QStringLiteral(
            "? (model.isOwn === true ? AppTheme.ownBubble")));
        QVERIFY2(!delegate.contains(QStringLiteral("AppTheme.mentionHighlight")),
                 "the mention ROW WASH is back — mentionHighlight is the "
                 "badge's token and resolves to a danger-adjacent rose under "
                 "Storm, which is what drew a red box around mentions");
        QVERIFY(delegate.contains(QStringLiteral(
            "                       : \"transparent\"")));
        QVERIFY(delegate.contains(QStringLiteral(
            "readonly property bool mentionBarVisible:\n"
            "                    !root.bubbleMode\n"
            "                    && (model.roomMentionHighlighted === true\n"
            "                        || (model.mentionsMe === true\n"
            "                            && (model.pushHighlighted === true\n"
            "                                || model.pushVerdictKnown !== true)))")));
        QVERIFY(delegate.contains(QStringLiteral(
            "                    visible: bubble.mentionBarVisible\n")));
        QVERIFY(delegate.contains(QStringLiteral(
            "                    radius: 1\n"
            "                    color: AppTheme.bolt\n")));
    }

    void mentionStyleIsPushedWithTheLinkInk()
    {
        const QString shell = read(QStringLiteral("MainScreen.qml"));
        QVERIFY(!shell.isEmpty());
        const QRegularExpression call(QStringLiteral(
            "setMentionStyle\\(\\s*accent\\s*,\\s*soft\\s*,\\s*code\\s*,"
            "\\s*linkInk\\s*,\\s*AppTheme\\.underlineLinks\\s*\\)"));
        QCOMPARE(shell.count(call), 2);
        QVERIFY2(shell.contains(QStringLiteral("AppTheme.link")),
                 "the link ink must come from the theme, not a literal");
        QVERIFY2(shell.contains(QStringLiteral("function onLinkChanged()")),
                 "a theme change that moves only the link ink must re-push");
        QVERIFY2(shell.contains(QStringLiteral(
                     "function onUnderlineLinksChanged()")),
                 "toggling underline-links must re-push the mention style");
    }

    void continuationRowsStayCompactAndActionsFloat()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!delegate.isEmpty());
        QVERIFY(!pane.isEmpty());

        QVERIFY(delegate.contains(QStringLiteral(
            "implicitHeight: root.showsIdentity && !root.smallAvatarMode\n"
            "                                ? 34 : bodyLabel.implicitHeight")));
        QVERIFY(delegate.contains(QStringLiteral(
            "objectName: \"continuationTimestamp\"")));
        QVERIFY(delegate.contains(QStringLiteral(
                    "active: !root.showsIdentity && rowHover.hovered"))
                || delegate.contains(QStringLiteral(
                    "visible: !root.showsIdentity && rowHover.hovered")));
        QVERIFY(delegate.contains(QStringLiteral("return \"\"")));
        QVERIFY(!delegate.contains(QStringLiteral(
            "return model.showSenderIdentity === true ? \"\" : ts")));

        QVERIFY(delegate.contains(QStringLiteral("anchors.right: parent.right")));
        QVERIFY(delegate.contains(QStringLiteral("anchors.top: parent.top")));
        QVERIFY(pane.contains(QStringLiteral("spacing: 0")));
    }

    void wrappedBodiesHaveStableIncubationWidths()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
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
        QVERIFY(uploadStart > previewStart);
        QVERIFY(delegate.mid(uploadStart, metaStart - uploadStart)
                    .contains(QStringLiteral("Layout.fillWidth: true")));

        QVERIFY(mediaBlock.contains(QStringLiteral(
            "Layout.alignment: Qt.AlignLeft")));
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

    void messageBodySelectionFollowsTheTheme()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QCOMPARE(delegate.count(QStringLiteral(
                     "selectionColor: AppTheme.textSelection")), 2);
    }

    void composerTabLeavesInsteadOfTyping()
    {
        const QString bar = read(QStringLiteral("MessageComposerBar.qml"));
        int from = 0, handlers = 0;
        while ((from = bar.indexOf(QStringLiteral("Keys.onTabPressed"), from)) >= 0) {
            const int end = bar.indexOf(QStringLiteral("Keys.onEscapePressed"), from);
            QVERIFY(end > from);
            const QString body = bar.mid(from, end - from);
            QVERIFY2(!body.contains(QStringLiteral("event.accepted = false")),
                     "Tab must not fall through to tab insertion");
            QVERIFY(body.contains(QStringLiteral("nextItemInFocusChain")));
            ++handlers;
            from = end;
        }
        QCOMPARE(handlers, 2);
    }

    void messageContentAndActionsRemainInteractive()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("TextEdit {\n                        id: bodyLabel")));
        QVERIFY(delegate.contains(QStringLiteral("readOnly: true")));
        QVERIFY(delegate.contains(QStringLiteral("selectByMouse: !/spoiler:toggle")));
        QVERIFY(delegate.contains(QStringLiteral("app.media.openWebUrl(link)")));
        QVERIFY(delegate.contains(QStringLiteral("mention:")));
        QVERIFY(delegate.contains(QStringLiteral("id: replyBox")));
        QVERIFY(delegate.contains(QStringLiteral("id: messageActionBar")));
        QVERIFY(delegate.contains(QStringLiteral("id: previewLoader")));
        QVERIFY(delegate.contains(QStringLiteral("id: imageComponent")));
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

    void stuckSendsCanBeCancelledAndMediaUploadsShowRealProgress()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());

        QVERIFY(delegate.contains(QStringLiteral("objectName: \"cancelSendLink\"")));
        QVERIFY(delegate.contains(QStringLiteral("root.canCancelSendAt(index)")));
        QVERIFY(delegate.contains(
            QStringLiteral("root.timelineModel.canCancelSend(")));
        QVERIFY(delegate.contains(
            QStringLiteral("root.timelineModel.cancelSend(")));

        QVERIFY(delegate.contains(
            QStringLiteral("function canCancelSendAt(viewRow) {")));

        QVERIFY(delegate.contains(
            QStringLiteral("objectName: \"uploadProgressLoader\"")));
        QVERIFY(delegate.contains(
            QStringLiteral("indeterminate: root.uploadProgress < 0")));
        QVERIFY(delegate.contains(QStringLiteral(
            "model.uploadProgress === undefined ? -1 : model.uploadProgress")));
        QVERIFY(delegate.contains(QStringLiteral("&& root.mediaRowBody")));
    }

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
            QVERIFY2(src.contains(QStringLiteral("editorOverrideFor")),
                     "the id must come from the registry, which is what "
                     "carries the EditorContext flag");
        }

        for (const QString &src : { bar, thread }) {
            QVERIFY2(!src.contains(QStringLiteral("\"composer.italic\"")),
                     "hand-listed editor ids are back; a seventh editor "
                     "shortcut would work in one composer and not the other");
        }

        QVERIFY(thread.contains(QStringLiteral("applyThreadFormat")));
    }

    void everyBridgeBackedSurfaceHearsTheRetryableSweep()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());

        const int failedHandlers =
            delegate.count(QStringLiteral("function onMediaFetchFailed("));
        QVERIFY2(failedHandlers >= 3,
                 qPrintable(QStringLiteral("expected the bridge-backed "
                                           "surfaces, found %1")
                                .arg(failedHandlers)));
        const int retryHandlers =
            delegate.count(QStringLiteral("function onMediaRetryable("));
        QCOMPARE(retryHandlers, failedHandlers);

        for (const QString &file : { QStringLiteral("Avatar.qml"),
                                     QStringLiteral("MediaListThumbnail.qml") }) {
            const QString src = read(file);
            QVERIFY(!src.isEmpty());
            QVERIFY2(src.contains(QStringLiteral("onMediaRetryable")),
                     qPrintable(file + QStringLiteral(" lost its recovery "
                                                      "channel")));
        }
    }

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

    void everyPollBindingOnAnswerTextTouchesTheRevision()
    {
        const QString src = read(QStringLiteral("CreatePollDialog.qml"));
        QVERIFY(!src.isEmpty());
        QVERIFY2(src.contains(QStringLiteral("property int answerRevision")),
                 "the revision counter is gone, so this case is testing "
                 "nothing");

        const QStringList lines = src.split(QLatin1Char('\n'));
        static const QRegularExpression decl(
            QStringLiteral("^\\s*(readonly\\s+)?property\\s+\\S+\\s+(\\w+)\\s*:"));
        int checked = 0;
        for (int i = 0; i < lines.size(); ++i) {
            const auto m = decl.match(lines.at(i));
            if (!m.hasMatch())
                continue;
            const QString name = m.captured(2);
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

    void theFollowCheckboxKeepsFollowingTheStoreAfterAClick()
    {
        const QString src = read(QStringLiteral("PolicyListDialog.qml"));
        QVERIFY(!src.isEmpty());
        const int at = src.indexOf(QStringLiteral("policyFollowCheck"));
        QVERIFY2(at >= 0, "the follow checkbox is gone — re-anchor this case");
        const QString block = blockAround(src, at);
        QVERIFY2(!block.isEmpty(), "could not slice the checkbox");
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

    void theViewerThumbnailStripResolvesMediaThatArrivesLate()
    {
        const QString src = read(QStringLiteral("ImageViewerOverlay.qml"));
        QVERIFY(!src.isEmpty());
        const int at = src.indexOf(QStringLiteral("id: thumbImage"));
        QVERIFY2(at >= 0, "the strip's thumbnail image is gone — re-anchor "
                          "this case");
        const QString block = blockAround(src, at);
        QVERIFY2(!block.isEmpty(), "could not slice the thumbnail image");
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

    void theBubbleTapExcludesTheActionBarItOpens()
    {
        const QString src = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!src.isEmpty());
        const int rowTap = src.indexOf(QStringLiteral("objectName: \"rowEmptySideTap\""));
        QVERIFY2(rowTap >= 0, "the row's empty-side tap is gone — re-anchor");
        const int rowPin = src.indexOf(QStringLiteral("root.toggleActionsPin()"), rowTap);
        const int at = src.indexOf(QStringLiteral("root.toggleActionsPin()"),
                                   rowPin + 1);
        QVERIFY2(at >= 0, "the bubble tap no longer pins the action bar — "
                          "re-anchor this case");
        const QString block = blockAround(src, at);
        QVERIFY2(!block.isEmpty(), "could not slice the bubble tap handler");
        QVERIFY2(block.contains(QStringLiteral("overlayAt(")),
                 "the bubble tap does not consult the shared overlay bands");
        const int fn = src.indexOf(QStringLiteral("function overlayAt("));
        QVERIFY2(fn >= 0, "the shared overlay-band function is gone");
        const QString bands = src.mid(fn, 1800);
        QVERIFY2(bands.contains(QStringLiteral("messageActionBarLoader")),
                 "a click on the action bar's padding, or in a gap between "
                 "its buttons, still falls through to the bubble and toggles "
                 "the pin — closing the bar the user was aiming at");
    }

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

    void aFocusedAppButtonActivatesOnReturnAndTheDeleteConfirmFocusesCancel()
    {
        const QString button = read(QStringLiteral("AppButton.qml"));
        QVERIFY(!button.isEmpty());
        QVERIFY2(button.contains(QStringLiteral("Keys.onReturnPressed:"))
                     && button.contains(QStringLiteral("Keys.onEnterPressed:")),
                 "AppButton does not activate on Return/Enter");
        QVERIFY2(button.contains(QStringLiteral("root.clicked()")),
                 "the Return handler never emits clicked()");
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY2(delegate.contains(QStringLiteral(
                     "onOpened: confirmCancelButton.forceActiveFocus()"))
                     && delegate.contains(QStringLiteral(
                            "id: confirmCancelButton")),
                 "the destructive confirm does not focus Cancel on open");
    }

    void theEscapeShortcutCancelsAPendingEditBeforeClosingAPanel()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        const int shortcutAt = pane.indexOf(QStringLiteral(
            "!timeline.emojiPickerOpen && !middleClickScroller.active"));
        QVERIFY(shortcutAt >= 0);
        const int handlerAt = pane.indexOf(QStringLiteral("onActivated: {"),
                                           shortcutAt);
        const int editAt = pane.indexOf(
            QStringLiteral("else if (app.composer.isEditing)"), handlerAt);
        const int infoAt = pane.indexOf(
            QStringLiteral("else if (root.infoOpen)"), handlerAt);
        QVERIFY2(editAt > handlerAt && editAt < infoAt,
                 "the Escape Shortcut closes a panel before it cancels a "
                 "pending edit");
    }

    void theThreadComposerEditsItsLastOwnMessageOnUp()
    {
        const QString panel = read(QStringLiteral("ThreadPanel.qml"));
        QVERIFY(panel.contains(QStringLiteral(
            "function editLastOwnThreadMessage()")));
        QCOMPARE(panel.count(QStringLiteral(
                     "panel.editLastOwnThreadMessage()")), 2);
        QVERIFY(panel.contains(QStringLiteral("app.thread.model")));
        const int fn = panel.indexOf(QStringLiteral(
            "function editLastOwnThreadMessage()"));
        QVERIFY(panel.mid(fn, 600).contains(QStringLiteral(
            "app.composer.text.length > 0")));
    }

    void theComposerCaretStopsBlinkingWhenIdle()
    {
        const QString caret = read(QStringLiteral("IdleCaret.qml"));
        QVERIFY(!caret.isEmpty());
        QVERIFY2(caret.contains(QStringLiteral("cursorVisible = false")),
                 "the idle caret no longer switches the field's own blink off");
        QVERIFY2(caret.contains(QStringLiteral("cursorVisible = true")),
                 "activity no longer brings the blinking caret back");
        QVERIFY(caret.contains(QStringLiteral("onTextChanged")));
        QVERIFY(caret.contains(QStringLiteral("onCursorPositionChanged")));
        QVERIFY(caret.contains(QStringLiteral("onActiveFocusChanged")));
        const QString composer = read(QStringLiteral("MessageComposerBar.qml"));
        QCOMPARE(composer.count(QStringLiteral("cursorDelegate:")), 2);
        QCOMPARE(composer.count(QStringLiteral("IdleCaret {")), 2);
        for (const QString &id : { QStringLiteral("input"),
                                   QStringLiteral("richInput") }) {
            QVERIFY2(composer.contains(QStringLiteral("field: %1 }").arg(id)),
                     qPrintable(QStringLiteral("%1 has no IdleCaret").arg(id)));
        }
    }

    void textSizeSettingReachesCallAndThreadChrome()
    {
        const QRegularExpression literal(
            QStringLiteral("font\\.pixelSize:\\s*\\d"));
        for (const QString &name : { QStringLiteral("ThreadSummaryCard.qml"),
                                     QStringLiteral("VoiceConnectedBar.qml"),
                                     QStringLiteral("RoomCallBanner.qml") }) {
            const QString qml = read(name);
            QVERIFY2(!qml.isEmpty(), qPrintable(name));
            QVERIFY2(!literal.match(qml).hasMatch(),
                     qPrintable(name + QStringLiteral(" has a literal pixelSize")));
            QVERIFY2(qml.contains(QStringLiteral("AppTheme.scaled(")),
                     qPrintable(name));
        }
        const QString tile = read(QStringLiteral("MediaBrowserTile.qml"));
        QVERIFY(!tile.contains(QStringLiteral("\"#FFFFFF\"")));
        QVERIFY(tile.contains(QStringLiteral("Accessible.onPressAction")));
    }

    void paginationLoadingIsAThinBarNotAPillOverTheText()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        const int at = pane.indexOf(QStringLiteral("objectName: \"paginationHeader\""));
        QVERIFY(at >= 0);
        const QString block = pane.mid(at, 2600);
        QVERIFY(block.contains(QStringLiteral("objectName: \"paginationLoadingBar\"")));
        QVERIFY(block.contains(QStringLiteral("height: 3")));
        QVERIFY(block.contains(QStringLiteral(
            "!== PaginationController.Loading\n                    anchors.centerIn: parent")));
    }

    void aRoomSelectionEndsWhenTheListLayoutChanges()
    {
        const QString panel = read(QStringLiteral("RoomsPanel.qml"));
        for (const char *handler : { "function onRoomNavigationLayoutChanged() {",
                                     "function onCompactRoomListChanged() {" }) {
            const int at = panel.indexOf(QLatin1String(handler));
            QVERIFY2(at >= 0, handler);
            QVERIFY2(panel.mid(at, 120).contains(
                         QStringLiteral("app.roomBatch.exitSelection()")),
                     handler);
        }
    }
};

QTEST_MAIN(QmlBindingContractTest)
#include "QmlBindingContractTest.moc"
