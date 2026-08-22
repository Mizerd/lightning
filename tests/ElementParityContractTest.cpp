// 2026-08-19 Element-parity round — contract pins for the three surfaces:
// the unified Space Home "Rooms and spaces" list (Joined badges +
// selection UI), the rail's inline space expansion, and the reader-list
// popover's Element look (per-reader timestamps). Whitespace-normalized
// scans (the VerificationCardContractTest convention).
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
        const QString pane = normalized(
            read(QStringLiteral(QML_DIR "/TimelinePane.qml")));
        QVERIFY(!pane.isEmpty());
        // ONE list; each row states its own membership.
        QVERIFY(pane.contains(QStringLiteral("ROOMS AND SPACES")));
        QVERIFY(pane.contains(
            QStringLiteral("objectName: \"spaceUnifiedChildRow\"")));
        const int row = pane.indexOf(
            QStringLiteral("objectName: \"spaceUnifiedChildRow\""));
        QVERIFY(row >= 0);
        QVERIFY(pane.indexOf(QStringLiteral("qsTr(\"Joined\")"), row) > row);
        QVERIFY(pane.indexOf(QStringLiteral("qsTr(\"Suggested\")"), row)
                > row);
        // Selection UI gated on the REAL m.space.child capability, never
        // offered optimistically.
        QVERIFY(pane.contains(
            QStringLiteral("objectName: \"spaceChildSelectBox\"")));
        QVERIFY(pane.contains(
            QStringLiteral("app.roomInfo.canManageSpaceChildren")));
        QVERIFY(pane.contains(
            QStringLiteral("objectName: \"spaceChildRemoveSelectedButton\"")));
        QVERIFY(pane.contains(
            QStringLiteral("objectName: \"spaceChildSuggestToggleButton\"")));
        QVERIFY(pane.contains(
            QStringLiteral("app.spaces.setSpaceChildSuggested(")));
        // Search over names and descriptions.
        QVERIFY(pane.contains(
            QStringLiteral("objectName: \"spaceChildFilterField\"")));
        // Dedup by room id: right after a Join succeeds, sync marks the
        // room joined while the /hierarchy refetch is still in flight —
        // the stale offer row must not render next to the Joined row.
        QVERIFY(pane.contains(
            QStringLiteral("if (seen[uo.roomId] === true) continue")));
    }

    // The banner's view controls must drive something that EXISTS.
    //
    // They shipped once toggling a setting nothing read: the crop/expand
    // property was lost from an edit that aborted before writing, so the
    // banner stayed full-size and the button did nothing — which is exactly
    // how it was reported. Nothing in a build or a test suite noticed,
    // because a QML binding to a missing property is a runtime warning in a
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
        // ...hiding has a way back, or it is a trap...
        QVERIFY(pane.contains(
            QStringLiteral("objectName: \"spaceBannerShowButton\"")));

        // ...and each one's setting is actually READ by the banner, not
        // merely written by the button.
        QVERIFY2(pane.contains(
                     QStringLiteral("readonly property bool expanded: "
                                    "app.settings.spaceBannerExpanded")),
                 "the banner does not read spaceBannerExpanded");
        QVERIFY2(pane.contains(QStringLiteral(
                     "visible: app.settings.spaceBannersVisible")),
                 "the banner does not read spaceBannersVisible");

        // Cropped is the DEFAULT presentation: the fixed strip is the
        // else-branch, and expanding is what takes the picture's own shape.
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
        // TapHandlers are non-exclusive across subtrees: a select tap
        // must not ALSO open the row (the same class as the emoji-picker
        // and facepile fixes).
        const QString pane = normalized(
            read(QStringLiteral(QML_DIR "/TimelinePane.qml")));
        const int row = pane.indexOf(
            QStringLiteral("objectName: \"spaceUnifiedChildRow\""));
        QVERIFY(row >= 0);
        const QString scope = pane.mid(row, 2500);
        QVERIFY(scope.contains(
            QStringLiteral("unifiedRow.mapToItem( selectBox,")));
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
        QVERIFY(rail.contains(QStringLiteral("toggleSpaceExpansion(")));
        QVERIFY(rail.contains(QStringLiteral("showMoreRooms(")));
        // Opening a room from the expansion activates its space first —
        // the room-list column filters by activeSpaceId, and openRoom
        // itself never touches it.
        const int open = rail.indexOf(
            QStringLiteral("app.spaces.activeSpaceId = spaceItem.ownSpaceId"));
        QVERIFY(open >= 0);
        QVERIFY(rail.indexOf(QStringLiteral("app.openRoom("), open) > open);
        // Expansion state survives delegate recycling (rail root, not the
        // delegate) and is cleared on account switch.
        QVERIFY(rail.contains(QStringLiteral("property var railExpansion")));
        QVERIFY(rail.contains(
            QStringLiteral("root.railExpansion = ({})")));
    }

    void railTileHandlersAreScopedToTheTileBand()
    {
        // The tile's tap must not fire for taps in the expansion rows
        // below the tile, NOR for taps on the chevron badge —
        // TapHandlers are non-exclusive across subtrees, and without the
        // chevron exclusion a chevron click would also navigate. There
        // is deliberately NO double-tap (2026-08-19 maintainer request:
        // the arrow is the one expansion trigger; a single tap on a real
        // space opens its overview, replacing the chat view).
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
            "if (spaceItem.isRealSpace) app.openSpaceHome(spaceItem.ownSpaceId)")));
    }

    // 2026-08-19: the jump-to-live history trim is Element's
    // jumpToLiveTimeline() policy — rebuild at the live edge instead of
    // scrolling a huge backlog. It is an EXPLICIT user action and must be
    // reachable from exactly one place: the far branch of goToLatest().
    // Wiring it to scrolling or pagination would reset a reader's timeline
    // out from under them, so this pins the single call site.
    void historyTrimFiresOnlyFromTheFarJumpToLatest()
    {
        const QString pane = normalized(
            read(QStringLiteral(QML_DIR "/TimelinePane.qml")));
        QVERIFY(!pane.isEmpty());
        // Exactly ONE call site in the whole pane.
        QCOMPARE(pane.count(QStringLiteral("app.trimHistoryAndJumpToLive()")),
                 1);
        // ...and it sits inside goToLatest(), AFTER the near-glide branch
        // returns — i.e. it is the far case only.
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

    // 2026-08-19: speculative media (full-payload prefetch, and the poster
    // extraction that materializes one) must gate on the view having
    // SETTLED, not merely on a row being on screen — a live capture showed
    // ~120 MB pulled by one 15-second gesture because every row that swept
    // past armed a prefetch. Thumbnails stay ungated on purpose.
    void speculativeMediaGatesOnSettleNotMerelyOnScreen()
    {
        const QString pane = normalized(
            read(QStringLiteral(QML_DIR "/TimelinePane.qml")));
        const QString delegate = normalized(
            read(QStringLiteral(QML_DIR "/MessageDelegate.qml")));
        const QString audio = normalized(
            read(QStringLiteral(QML_DIR "/AudioPlayerCard.qml")));
        QVERIFY(!pane.isEmpty() && !delegate.isEmpty() && !audio.isEmpty());

        // The pane owns the one definition, derived from the existing
        // scroll-session state rather than a second notion of "busy".
        QVERIFY(pane.contains(QStringLiteral(
            "readonly property bool speculativeMediaAllowed: !userScrollActive")));
        // Both speculative call sites in the delegate consult it...
        QVERIFY(delegate.contains(QStringLiteral(
            "} else if (root.speculativeMediaAllowed) {")));
        QVERIFY(delegate.contains(QStringLiteral(
            "if (root.speculativeMediaAllowed && playbackAvailable")));
        // ...and neither gates the PAYLOAD on bare on-screen-ness any more.
        QVERIFY(!delegate.contains(QStringLiteral(
            "if (root.rowOnScreen && playbackAvailable")));
        // The thumbnail branch is deliberately still ungated.
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

    // 2026-08-19: with a row window active, a jump must restore the LIVE
    // EDGE before addressing a row by id. releaseAll() only lifts the pacing
    // cap — it leaves the window's skip — so a jump to a recent message
    // would resolve to "no such row" and silently do nothing.
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
        // The window must never claim "at bottom" while it hides the newest
        // message, or follow-latest latches onto a false latest.
        QVERIFY(pane.contains(
            QStringLiteral("if (rowWindowSkip > 0) return false")));
        // And the row mapping must account for the skip, or every
        // id-addressed navigation silently resolves to nothing.
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
        // Per-reader read time comes ONLY from the receipt's own tsMs;
        // absence renders nothing, never a fabricated time.
        QVERIFY(scope.contains(QStringLiteral("formatReadTime(")));
        QVERIFY(scope.contains(QStringLiteral("modelData.tsMs")));
        QVERIFY(scope.contains(QStringLiteral("if (!tsMs || tsMs <= 0)")));
        // The honest "+N" tail survives the restyle.
        QVERIFY(scope.contains(QStringLiteral("names not loaded")));
    }
};

QTEST_MAIN(ElementParityContractTest)
#include "ElementParityContractTest.moc"
