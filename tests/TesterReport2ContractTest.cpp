// Contract pins for several QML surfaces. Whitespace-normalized scans (the
// VerificationCardContractTest convention): only predicate text is pinned,
// never formatting.
#include <QFile>
#include <QRegularExpression>
#include <QtTest>

class TesterReport2ContractTest : public QObject
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
    void copyImageActionIsGatedLikeSaveAs()
    {
        const QString norm = normalized(
            read(QStringLiteral(QML_DIR "/MessageDelegate.qml")));
        QVERIFY(!norm.isEmpty());
        const int item = norm.indexOf(
            QStringLiteral("objectName: \"copyImageMenuItem\""));
        QVERIFY(item >= 0);
        const QString scope = norm.mid(item, 500);
        QVERIFY(scope.contains(QStringLiteral("model.isImage === true")));
        QVERIFY(scope.contains(
            QStringLiteral("model.mediaSourceAvailable === true")));
        QVERIFY(scope.contains(
            QStringLiteral("app.copyImageToClipboard(model.mediaKey")));
    }

    void replyQuotesAndComposerBannerShowImageThumbs()
    {
        const QString delegate = normalized(
            read(QStringLiteral(QML_DIR "/MessageDelegate.qml")));
        QVERIFY(delegate.contains(QStringLiteral("model.replyToMediaKey")));
        QVERIFY(delegate.contains(QStringLiteral(
            "app.mediaBridge.mediaSource( model.replyToMediaKey, \"thumb\")")));
        const QString composer = normalized(
            read(QStringLiteral(QML_DIR "/MessageComposerBar.qml")));
        QVERIFY(composer.contains(
            QStringLiteral("app.composer.replyingToMediaKey")));
    }

    void receiptListPopoverIsHonestAboutTheCap()
    {
        const QString pane = normalized(
            read(QStringLiteral(QML_DIR "/TimelinePane.qml")));
        QVERIFY(pane.contains(
            QStringLiteral("objectName: \"receiptListPopover\"")));
        // The tail line is driven by the uncapped total minus the delivered
        // names, never fabricated entries.
        QVERIFY(pane.contains(QStringLiteral("names not loaded")));
        QVERIFY(pane.contains(QStringLiteral(
            "Math.max(0, totalOthers - readers.length)")));
        const QString delegate = normalized(
            read(QStringLiteral(QML_DIR "/MessageDelegate.qml")));
        QVERIFY(delegate.contains(
            QStringLiteral("root.timelineView.openReceiptList(")));
    }

    void spaceHomeGainsInviteAndNestedSubspaces()
    {
        const QString pane = normalized(
            read(QStringLiteral(QML_DIR "/TimelinePane.qml")));
        const int invite = pane.indexOf(
            QStringLiteral("objectName: \"spaceInviteButton\""));
        QVERIFY(invite >= 0);
        // Honestly permission-gated, and on the SPACE's own roster.
        const QString inviteScope = pane.mid(invite, 500);
        QVERIFY(inviteScope.contains(
            QStringLiteral("app.roomInfo.canInvite")));
        QVERIFY(inviteScope.contains(QStringLiteral(
            "app.roomInfo.roomId === spaceHome.spaceId")));
        // Nested subspaces (a section each in the lobby, built by
        // SpaceManager::lobbySections): a nested sub-space row drills in, the
        // unjoined offer names itself a Space, and a successful sub-space join
        // drills in.
        const QString lobby = normalized(
            read(QStringLiteral(QML_DIR "/SpaceLobby.qml")));
        QVERIFY(lobby.contains(
            QStringLiteral("objectName: \"spaceUnifiedChildRow\"")));
        QVERIFY(pane.contains(
            QStringLiteral("app.spaces.lobbySubspaceIds(spaceId)")));
        QVERIFY(pane.contains(
            QStringLiteral("onOpenSpaceRequested: (roomId) => "
                           "app.spaces.activeSpaceId = roomId")));
        QVERIFY(pane.contains(QStringLiteral("function onSpaceJoined(")));
        QVERIFY(lobby.contains(
            QStringLiteral("Space · %n room(s) inside")));
    }

    void railIndentsNestedSpaces()
    {
        const QString rail = normalized(
            read(QStringLiteral(QML_DIR "/SpacesRail.qml")));
        // The rail's rows come from RailEntryModel, so the roles are required
        // properties on the delegate. Depth is drawn as nested tinted regions,
        // not an indent: every tile sits on one x and the nesting level drives
        // the regions.
        QVERIFY(rail.contains(QStringLiteral("required property int level")));
        QVERIFY2(rail.contains(QStringLiteral("bandLayers")),
                 "the rail no longer derives anything from a row's depth");
        QVERIFY2(!rail.contains(
                     QStringLiteral("anchors.horizontalCenterOffset:")),
                 "a tile is offset per level again — that is the wave");
        // No tile offset in the folder path either.
        QVERIFY2(!rail.contains(QStringLiteral("tileIndent")),
                 "a tile is offset again, in the folder path this time");
        // The folder's container is on the same inset ladder as the hierarchy
        // regions.
        QVERIFY2(rail.contains(QStringLiteral("root.bandInset(0)")),
                 "the folder container has its own geometry again, so it can "
                 "drift out of step with the regions nested inside it");
        QVERIFY2(!rail.contains(QStringLiteral("anchors.leftMargin: 6")),
                 "the folder container is inset in RAW units against a "
                 "scaled ladder, so it renders narrower than the regions it "
                 "is supposed to contain at every interface size");
    }

    void roomAvatarsFallBackToInitialsNeverHash()
    {
        // The '#' glyph fallback is retired everywhere: Avatar renders
        // initials, and no caller re-enables the glyph.
        const QString avatar = normalized(
            read(QStringLiteral(QML_DIR "/Avatar.qml")));
        QVERIFY(!avatar.contains(
            QStringLiteral("? \"#\" :")));
        for (const char *file :
             { "/RoomDelegate.qml", "/HomePane.qml", "/TimelinePane.qml" }) {
            const QString src = normalized(
                read(QStringLiteral(QML_DIR) + QLatin1String(file)));
            QVERIFY2(!src.contains(QStringLiteral("roomGlyph: true")),
                     file);
        }
    }
};

QTEST_MAIN(TesterReport2ContractTest)
#include "TesterReport2ContractTest.moc"
