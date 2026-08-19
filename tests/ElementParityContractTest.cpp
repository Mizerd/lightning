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
