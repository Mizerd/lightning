// Contract pins for the Space Home lobby (badges and selection UI), the
// rail's inline space expansion, and the reader-list popover (per-reader
// timestamps). Whitespace-normalized scans, as in
// VerificationCardContractTest.
#include <QFile>
#include <QRegularExpression>
#include <QtTest>

class ElementParityContractTest : public QObject
{
    Q_OBJECT
private:
    static QString read(const QString &path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(f.readAll());
    }
    static QString normalized(const QString &s)
    {
        QString out = s;
        out.replace(QRegularExpression(QStringLiteral("\\s+")),
                    QStringLiteral(" "));
        return out.trimmed();
    }

private Q_SLOTS:
    void unifiedListShowsMembershipAndSelection()
    {
        // The list lives in SpaceLobby.qml as a sectioned lobby
        // (SpaceManager::lobbySections); it keeps everything the flat list did.
        const QString lobby = normalized(
            read(QStringLiteral(QML_DIR "/SpaceLobby.qml")));
        const QString pane = normalized(
            read(QStringLiteral(QML_DIR "/TimelinePane.qml")));
        QVERIFY(!lobby.isEmpty());
        QVERIFY(!pane.isEmpty());
        QVERIFY(lobby.contains(QStringLiteral("ROOMS AND SPACES")));
        QVERIFY(lobby.contains(
            QStringLiteral("objectName: \"spaceUnifiedChildRow\"")));
        const int row = lobby.indexOf(
            QStringLiteral("objectName: \"spaceUnifiedChildRow\""));
        QVERIFY(row >= 0);
        // Suggested is a badge on the row. "Joined" is not: it read as the
        // user's role, and the row's action (Join or the open arrow) already
        // says it. The accessible name carries it instead.
        QVERIFY(lobby.indexOf(QStringLiteral("SuggestedChip {"), row) > row);
        QVERIFY(!lobby.contains(QStringLiteral("\"Joined\")")));
        QVERIFY(lobby.contains(QStringLiteral("qsTr(\"%1, not joined\")")));
        // Selection UI is gated on the real m.space.child capability.
        QVERIFY(lobby.contains(
            QStringLiteral("objectName: \"spaceChildSelectBox\"")));
        QVERIFY(pane.contains(
            QStringLiteral("app.roomInfo.canManageSpaceChildren")));
        QVERIFY(pane.contains(
            QStringLiteral("canManage: spaceHome.canManageChildren")));
        QVERIFY(lobby.contains(
            QStringLiteral("objectName: \"spaceChildRemoveSelectedButton\"")));
        QVERIFY(lobby.contains(
            QStringLiteral("objectName: \"spaceChildSuggestToggleButton\"")));
        QVERIFY(pane.contains(
            QStringLiteral("app.spaces.setSpaceChildSuggested(")));
        // Search, grouping, ordering and dedup live in C++
        // (SpaceChildSuggestTest).
        QVERIFY(lobby.contains(
            QStringLiteral("objectName: \"spaceChildFilterField\"")));
        QVERIFY(pane.contains(QStringLiteral("app.spaces.lobbySections(")));
        // A topic is server text: always plain.
        const int topic = lobby.indexOf(
            QStringLiteral("objectName: \"spaceLobbyRowTopic\""));
        QVERIFY(topic > 0);
        QVERIFY(lobby.mid(topic, 600).contains(
            QStringLiteral("textFormat: Text.PlainText")));
    }

    // The banner's view controls must drive settings the banner actually
    // reads; a binding to a missing property is only a runtime warning in a
    // view no headless suite opens.
    void theBannerViewControlsDriveSomethingThatExists()
    {
        const QString pane = normalized(
            read(QStringLiteral(QML_DIR "/TimelinePane.qml")));
        QVERIFY(!pane.isEmpty());

        // The buttons exist...
        QVERIFY(pane.contains(
            QStringLiteral("objectName: \"spaceBannerExpandButton\"")));
        QVERIFY(pane.contains(
            QStringLiteral("objectName: \"spaceBannerHideButton\"")));
        // ...hiding has a way back...
        QVERIFY(pane.contains(
            QStringLiteral("objectName: \"spaceBannerShowButton\"")));

        // ...and each setting is read by the banner, not only written.
        QVERIFY2(pane.contains(
                     QStringLiteral("readonly property bool expanded: "
                                    "app.settings.spaceBannerExpanded")),
                 "the banner does not read spaceBannerExpanded");
        QVERIFY2(pane.contains(QStringLiteral(
                     "visible: app.settings.spaceBannersVisible")),
                 "the banner does not read spaceBannersVisible");

        // The control cluster's visibility comes from the conditions, never
        // from summing children's `visible`: that is effective visibility, so
        // a parent hidden at startup would keep the sum at zero forever.
        QVERIFY2(!pane.contains(QStringLiteral("visibleCount")),
                 "the banner controls sum their children's visible again");
        QVERIFY2(pane.contains(QStringLiteral(
                     "visible: spaceBannerCard.bannerMxc.length > 0 "
                     "|| spaceBannerCard.canEdit")),
                 "the banner control cluster does not gate on its conditions");

        // Cropped is the default: the fixed strip is the else-branch, and
        // expanding takes the picture's own shape.
        QVERIFY2(pane.contains(QStringLiteral(
                     "height: Math.round(expanded && bannerAspect > 0")),
                 "the banner height does not branch on expanded");
        QVERIFY2(pane.contains(QStringLiteral(
                     "fillMode: spaceBannerCard.expanded "
                     "? Image.PreserveAspectFit : Image.PreserveAspectCrop")),
                 "the banner fill mode does not branch on expanded");
    }

    void unifiedRowGuardsItsCheckboxBand()
    {
        // TapHandlers are non-exclusive across subtrees: a select tap must not
        // also open the row.
        const QString lobby = normalized(
            read(QStringLiteral(QML_DIR "/SpaceLobby.qml")));
        const int row = lobby.indexOf(
            QStringLiteral("objectName: \"spaceUnifiedChildRow\""));
        QVERIFY(row >= 0);
        const QString scope = lobby.mid(row, 2500);
        QVERIFY(scope.contains(QStringLiteral("row.mapToItem(selectBox,")));
        // The box itself takes the exclusive grab.
        QVERIFY(lobby.contains(
            QStringLiteral("gesturePolicy: TapHandler.WithinBounds")));
        // Same on the section header: its menu and box are excluded.
        QVERIFY(lobby.contains(
            QStringLiteral("var bands = [sectionMenuButton, sectionSelectBox]")));
        // ChannelRowGeometryQmlTest::lobbyTapsReachTheRightTarget clicks it.
    }

    void railExpandsSpacesInline()
    {
        const QString rail = normalized(
            read(QStringLiteral(QML_DIR "/SpacesRail.qml")));
        QVERIFY(!rail.isEmpty());
        QVERIFY(rail.contains(
            QStringLiteral("objectName: \"railSpaceExpandChevron\"")));
        QVERIFY(rail.contains(
            QStringLiteral("objectName: \"railSpaceMoreButton\"")));
        // The expander reveals the Space's joined subspaces (real model rows)
        // and top rooms, and its state persists in RailLayoutStore, as Element
        // persists Space-panel expansion.
        QVERIFY(rail.contains(QStringLiteral(
            "app.railLayout.toggleSpaceExpanded( spaceItem.spaceId)")));
        QVERIFY(rail.contains(QStringLiteral("showMoreRooms(")));
        // Opening a room from the expansion activates its space first: the
        // room list filters by activeSpaceId and openRoom never sets it.
        const int open = rail.indexOf(
            QStringLiteral("app.spaces.activeSpaceId = spaceItem.spaceId"));
        QVERIFY(open >= 0);
        QVERIFY(rail.indexOf(QStringLiteral("app.openRoom("), open) > open);
        // The reveal count ("+5 more") is session state on the rail root, so
        // ListView recycling and resets keep it; an account switch clears it.
        QVERIFY(rail.contains(QStringLiteral("property var railReveal")));
        QVERIFY(rail.contains(
            QStringLiteral("root.railReveal = ({})")));
    }

    void railTileHandlersAreScopedToTheTileBand()
    {
        // The tile's tap must not fire for taps in the expansion rows or on
        // the chevron badge (TapHandlers are non-exclusive). No double-tap: the
        // arrow is the one expansion trigger, and a tap on a real space opens
        // its overview.
        const QString rail = normalized(
            read(QStringLiteral(QML_DIR "/SpacesRail.qml")));
        QVERIFY(rail.contains(QStringLiteral(
            "if (eventPoint.position.y > spaceItem.tileBandHeight) return")));
        QVERIFY(rail.contains(QStringLiteral(
            "spaceItem.mapToItem(expandChevronArea,")));
        QVERIFY(rail.contains(QStringLiteral(
            "if (pointOnChevron(eventPoint)) return")));
        QVERIFY(!rail.contains(QStringLiteral("onDoubleTapped")));
        // A real space's tap opens the overview; pseudo tiles only filter.
        QVERIFY(rail.contains(QStringLiteral(
            "if (spaceItem.isRealSpace) app.openSpaceHome(spaceItem.spaceId)")));
        // No tile handler fires during a drag: a release is a drop only.
        QVERIFY(rail.contains(QStringLiteral("enabled: !root.dragging")));
    }

    // The history trim (Element's jumpToLiveTimeline() policy: rebuild at the
    // live edge rather than scroll a huge backlog) is an explicit action
    // reachable only from goToLatest()'s far branch; wiring it to scrolling
    // or pagination would reset the reader's timeline.
    void historyTrimFiresOnlyFromTheFarJumpToLatest()
    {
        const QString pane = normalized(
            read(QStringLiteral(QML_DIR "/TimelinePane.qml")));
        QVERIFY(!pane.isEmpty());
        // Exactly one call site in the pane.
        QCOMPARE(pane.count(QStringLiteral("app.trimHistoryAndJumpToLive()")),
                 1);
        // ...inside goToLatest(), after the near-glide branch returns: the far
        // case only.
        const int jump = pane.indexOf(QStringLiteral("function goToLatest()"));
        QVERIFY(jump >= 0);
        const int call =
            pane.indexOf(QStringLiteral("app.trimHistoryAndJumpToLive()"),
                         jump);
        QVERIFY(call > jump);
        const QString scope = pane.mid(jump, call - jump);
        QVERIFY(scope.contains(QStringLiteral("smoothJumpViewports")));
        QVERIFY(scope.contains(QStringLiteral("animateTo(")));
        // A refusal must fall through to the ordinary landing.
        QVERIFY(pane.indexOf(QStringLiteral("settleAtLatest()"), call) > call);
        // The wheel handler must never reach it.
        const int wheel =
            pane.indexOf(QStringLiteral("objectName: \"timelineWheelHandler\""));
        QVERIFY(wheel >= 0);
        QVERIFY(wheel > call);
    }

    // Speculative media (full-payload prefetch and poster extraction) waits
    // for the view to settle, not merely for a row to be on screen, or a fast
    // gesture pulls every payload it sweeps past. Thumbnails stay ungated.
    void speculativeMediaGatesOnSettleNotMerelyOnScreen()
    {
        const QString pane = normalized(
            read(QStringLiteral(QML_DIR "/TimelinePane.qml")));
        const QString delegate = normalized(
            read(QStringLiteral(QML_DIR "/MessageDelegate.qml")));
        const QString audio = normalized(
            read(QStringLiteral(QML_DIR "/AudioPlayerCard.qml")));
        QVERIFY(!pane.isEmpty() && !delegate.isEmpty() && !audio.isEmpty());

        // The pane owns one definition, derived from the scroll-session state.
        QVERIFY(pane.contains(QStringLiteral(
            "readonly property bool speculativeMediaAllowed: !userScrollActive")));
        // Both speculative call sites in the delegate consult it...
        QVERIFY(delegate.contains(QStringLiteral(
            "} else if (root.speculativeMediaAllowed) {")));
        QVERIFY(delegate.contains(QStringLiteral(
            "if (root.speculativeMediaAllowed && playbackAvailable")));
        // ...and neither gates the payload on on-screen alone.
        QVERIFY(!delegate.contains(QStringLiteral(
            "if (root.rowOnScreen && playbackAvailable")));
        // The thumbnail branch is deliberately ungated.
        QVERIFY(delegate.contains(QStringLiteral(
            "if (model.mediaThumbAvailable === true) {")));
        // The audio card's prefetch is gated the same way, and retries.
        QVERIFY(audio.contains(QStringLiteral(
            "if (rowOnScreen && prefetchAllowed")));
        QVERIFY(audio.contains(QStringLiteral(
            "onPrefetchAllowedChanged: if (prefetchAllowed) maybePrefetch()")));
        QVERIFY(delegate.contains(QStringLiteral(
            "prefetchAllowed: root.speculativeMediaAllowed")));
    }

    // With a row window active, a jump restores the live edge before
    // addressing a row by id: releaseAll() only lifts the pacing cap and
    // keeps the window's skip, so the target would not resolve.
    void jumpPathsRestoreTheLiveEdgeNotJustThePacedBacklog()
    {
        const QString pane = normalized(
            read(QStringLiteral(QML_DIR "/TimelinePane.qml")));
        QVERIFY(!pane.isEmpty());
        const int fn = pane.indexOf(
            QStringLiteral("function releasePendingRows()"));
        QVERIFY(fn >= 0);
        const QString scope = pane.mid(fn, 600);
        QVERIFY(scope.contains(
            QStringLiteral("app.timelineView.clearWindow()")));
        // The window never claims "at bottom" while hiding the newest message.
        QVERIFY(pane.contains(
            QStringLiteral("if (rowWindowSkip > 0) return false")));
        // The row mapping accounts for the skip.
        QVERIFY(pane.contains(QStringLiteral(
            "app.timeline.count - 1 - rowWindowSkip - row")));
    }

    void receiptPopoverCarriesElementLook()
    {
        const QString pane = normalized(
            read(QStringLiteral(QML_DIR "/TimelinePane.qml")));
        const int pop = pane.indexOf(
            QStringLiteral("objectName: \"receiptListPopover\""));
        QVERIFY(pop >= 0);
        const QString scope = pane.mid(pop, 7000);
        QVERIFY(scope.contains(QStringLiteral("Seen by 1 person")));
        QVERIFY(scope.contains(QStringLiteral("Seen by %1 people")));
        // Per-reader read time comes only from the receipt's tsMs; absence
        // renders nothing.
        QVERIFY(scope.contains(QStringLiteral("formatReadTime(")));
        QVERIFY(scope.contains(QStringLiteral("modelData.tsMs")));
        QVERIFY(scope.contains(QStringLiteral("if (!tsMs || tsMs <= 0)")));
        // The "+N" tail survives.
        QVERIFY(scope.contains(QStringLiteral("names not loaded")));
    }
};

QTEST_MAIN(ElementParityContractTest)
#include "ElementParityContractTest.moc"
