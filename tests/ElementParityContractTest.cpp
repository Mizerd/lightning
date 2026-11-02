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
        QVERIFY(lobby.indexOf(QStringLiteral("SuggestedChip {"), row) > row);
        QVERIFY(!lobby.contains(QStringLiteral("\"Joined\")")));
        QVERIFY(lobby.contains(QStringLiteral("qsTr(\"%1, not joined\")")));
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
        QVERIFY(lobby.contains(
            QStringLiteral("objectName: \"spaceChildFilterField\"")));
        QVERIFY(pane.contains(QStringLiteral("app.spaces.lobbySections(")));
        const int topic = lobby.indexOf(
            QStringLiteral("objectName: \"spaceLobbyRowTopic\""));
        QVERIFY(topic > 0);
        QVERIFY(lobby.mid(topic, 600).contains(
            QStringLiteral("textFormat: Text.PlainText")));
    }

    void theBannerViewControlsDriveSomethingThatExists()
    {
        const QString pane = normalized(
            read(QStringLiteral(QML_DIR "/TimelinePane.qml")));
        QVERIFY(!pane.isEmpty());

        QVERIFY(pane.contains(
            QStringLiteral("objectName: \"spaceBannerExpandButton\"")));
        QVERIFY(pane.contains(
            QStringLiteral("objectName: \"spaceBannerHideButton\"")));
        QVERIFY(pane.contains(
            QStringLiteral("objectName: \"spaceBannerShowButton\"")));

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
        QVERIFY(lobby.contains(
            QStringLiteral("gesturePolicy: TapHandler.WithinBounds")));
        QVERIFY(lobby.contains(
            QStringLiteral("var bands = [sectionMenuButton, sectionSelectBox]")));
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
        QVERIFY(rail.contains(QStringLiteral(
            "app.railLayout.toggleSpaceExpanded( spaceItem.spaceId)")));
        QVERIFY(rail.contains(QStringLiteral("showMoreRooms(")));
        const int open = rail.indexOf(
            QStringLiteral("app.spaces.activeSpaceId = spaceItem.spaceId"));
        QVERIFY(open >= 0);
        QVERIFY(rail.indexOf(QStringLiteral("app.openRoom("), open) > open);
        QVERIFY(rail.contains(QStringLiteral("property var railReveal")));
        QVERIFY(rail.contains(
            QStringLiteral("root.railReveal = ({})")));
    }

    void railTileHandlersAreScopedToTheTileBand()
    {
        const QString rail = normalized(
            read(QStringLiteral(QML_DIR "/SpacesRail.qml")));
        QVERIFY(rail.contains(QStringLiteral(
            "if (eventPoint.position.y > spaceItem.tileBandHeight) return")));
        QVERIFY(rail.contains(QStringLiteral(
            "spaceItem.mapToItem(expandChevronArea,")));
        QVERIFY(rail.contains(QStringLiteral(
            "if (pointOnChevron(eventPoint)) return")));
        QVERIFY(!rail.contains(QStringLiteral("onDoubleTapped")));
        QVERIFY(rail.contains(QStringLiteral(
            "if (spaceItem.isRealSpace) app.openSpaceHome(spaceItem.spaceId)")));
        QVERIFY(rail.contains(QStringLiteral("enabled: !root.dragging")));
    }

    void historyTrimFiresOnlyFromTheFarJumpToLatest()
    {
        const QString pane = normalized(
            read(QStringLiteral(QML_DIR "/TimelinePane.qml")));
        QVERIFY(!pane.isEmpty());
        QCOMPARE(pane.count(QStringLiteral("app.trimHistoryAndJumpToLive()")),
                 1);
        const int jump = pane.indexOf(QStringLiteral("function goToLatest()"));
        QVERIFY(jump >= 0);
        const int call =
            pane.indexOf(QStringLiteral("app.trimHistoryAndJumpToLive()"),
                         jump);
        QVERIFY(call > jump);
        const QString scope = pane.mid(jump, call - jump);
        QVERIFY(scope.contains(QStringLiteral("smoothJumpViewports")));
        QVERIFY(scope.contains(QStringLiteral("animateTo(")));
        QVERIFY(pane.indexOf(QStringLiteral("settleAtLatest()"), call) > call);
        const int wheel =
            pane.indexOf(QStringLiteral("objectName: \"timelineWheelHandler\""));
        QVERIFY(wheel >= 0);
        QVERIFY(wheel > call);
    }

    void speculativeMediaGatesOnSettleNotMerelyOnScreen()
    {
        const QString pane = normalized(
            read(QStringLiteral(QML_DIR "/TimelinePane.qml")));
        const QString delegate = normalized(
            read(QStringLiteral(QML_DIR "/MessageDelegate.qml")));
        const QString audio = normalized(
            read(QStringLiteral(QML_DIR "/AudioPlayerCard.qml")));
        QVERIFY(!pane.isEmpty() && !delegate.isEmpty() && !audio.isEmpty());

        QVERIFY(pane.contains(QStringLiteral(
            "readonly property bool speculativeMediaAllowed: !userScrollActive")));
        QVERIFY(delegate.contains(QStringLiteral(
            "} else if (root.speculativeMediaAllowed) {")));
        QVERIFY(delegate.contains(QStringLiteral(
            "if (root.speculativeMediaAllowed && playbackAvailable")));
        QVERIFY(!delegate.contains(QStringLiteral(
            "if (root.rowOnScreen && playbackAvailable")));
        QVERIFY(delegate.contains(QStringLiteral(
            "if (model.mediaThumbAvailable === true) {")));
        QVERIFY(audio.contains(QStringLiteral(
            "if (rowOnScreen && prefetchAllowed")));
        QVERIFY(audio.contains(QStringLiteral(
            "onPrefetchAllowedChanged: if (prefetchAllowed) maybePrefetch()")));
        QVERIFY(delegate.contains(QStringLiteral(
            "prefetchAllowed: root.speculativeMediaAllowed")));
    }

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
        QVERIFY(pane.contains(
            QStringLiteral("if (rowWindowSkip > 0) return false")));
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
        QVERIFY(scope.contains(QStringLiteral("formatReadTime(")));
        QVERIFY(scope.contains(QStringLiteral("modelData.tsMs")));
        QVERIFY(scope.contains(QStringLiteral("if (!tsMs || tsMs <= 0)")));
        QVERIFY(scope.contains(QStringLiteral("names not loaded")));
    }

    void mutedRoomsDrawNoPillOnHomeOrInTheLobby()
    {
        const QString home = normalized(
            read(QStringLiteral(QML_DIR "/HomePane.qml")));
        const QString lobby = normalized(
            read(QStringLiteral(QML_DIR "/SpaceLobby.qml")));
        for (const QString &src : {home, lobby}) {
            QVERIFY(src.contains(QStringLiteral(
                "app.settings.roomNotificationMode(id)")));
            QVERIFY(src.contains(QStringLiteral("notificationMode === 2")));
            QVERIFY(src.contains(QStringLiteral("onRoomNotificationModeChanged")));
        }
        QVERIFY(home.contains(QStringLiteral(
            "visible: !recentRow.muted && (recentRow.modelData.highlightCount || 0) > 0")));
        QVERIFY(home.contains(QStringLiteral(
            "visible: !recentRow.muted && recentRow.modelData.hasUnread === true")));
        QVERIFY(lobby.contains(QStringLiteral(
            "visible: !row.muted && Number(row.modelData.highlightCount || 0) > 0")));
        QVERIFY(lobby.contains(QStringLiteral(
            "visible: !row.muted && row.modelData.hasUnread === true")));
    }
};

QTEST_MAIN(ElementParityContractTest)
#include "ElementParityContractTest.moc"
