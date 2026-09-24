// 2026-08-18 tester report #2 — contract pins for the round's QML
// surfaces. Whitespace-normalized scans (the VerificationCardContractTest
// convention): only predicate text is pinned, never formatting.
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
        // The tail line exists and is driven by the TRUTHFUL uncapped
        // total minus the delivered names — never fabricated entries.
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
        // Nested subspaces (2026-09-23: a SECTION each in the lobby, built
        // by SpaceManager::lobbySections): a nested sub-space row drills in,
        // the unjoined offer names itself a Space, and a successful sub-space
        // join drills in.
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
        // The rail's rows became a real QAbstractListModel in 2026-08-25
        // (RailEntryModel, so a preview drag can MOVE rows rather than reset
        // them), so the roles are required properties on the delegate rather
        // than fields on a `modelData` map.
        //
        // AND THE INDENT IS GONE, deliberately. This used to require a centre
        // OFFSET per level; on 2026-09-18 that offset was reported as "they
        // keep sticking out more and more and create like a wave pattern".
        // Depth is carried by nested tinted REGIONS now — one per ancestor,
        // each inset inside the one containing it, saturating so a deep tree
        // does not walk towards black — and every tile sits on one x.
        // What survives is the part that was always the point: the nesting
        // LEVEL is what the rail renders the hierarchy from.
        QVERIFY(rail.contains(QStringLiteral("required property int level")));
        QVERIFY2(rail.contains(QStringLiteral("bandLayers")),
                 "the rail no longer derives anything from a row's depth");
        QVERIFY2(!rail.contains(
                     QStringLiteral("anchors.horizontalCenterOffset:")),
                 "a tile is offset per level again — that is the wave");
        // AND THE FOLDER PATH DOES NOT GET AN EXEMPTION. It had one:
        // `tileIndent` moved a filed Space 7px right "so the folder's
        // container band has an edge to show", in the same file whose comment
        // said hierarchy depth is not an offset. Measured on a capture at
        // 7.5px off the shared axis. The property is gone, so the scan is for
        // its absence.
        QVERIFY2(!rail.contains(QStringLiteral("tileIndent")),
                 "a tile is offset again, in the folder path this time");
        // THE FOLDER'S CONTAINER IS ON THE SAME LADDER AS THE HIERARCHY
        // REGIONS, which is what replaced the inset a filed Space used to
        // carry. Two earlier versions of this assertion chased that inset —
        // first its literal `7`, then its scaling — and both were pinning a
        // mechanism that should not have existed: a folder and a hierarchy
        // region say the same thing, so they are one device, and a container
        // drawn in raw units against a scaled ladder came out NARROWER than
        // the regions it contains.
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
