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
        // Nested subspaces (2026-08-19: now rows of the unified
        // "Rooms and spaces" list): joined sub-space rows drill in, the
        // unjoined offer names itself a Space, and a successful
        // sub-space join drills in.
        QVERIFY(pane.contains(
            QStringLiteral("objectName: \"spaceUnifiedChildRow\"")));
        QVERIFY(pane.contains(
            QStringLiteral("app.spaces.childSpacesDetailed(spaceId)")));
        QVERIFY(pane.contains(QStringLiteral("function onSpaceJoined(")));
        QVERIFY(pane.contains(
            QStringLiteral("Space · %n room(s) inside")));
    }

    void railIndentsNestedSpaces()
    {
        const QString rail = normalized(
            read(QStringLiteral(QML_DIR "/SpacesRail.qml")));
        // The rail's rows became a real QAbstractListModel in 2026-08-25
        // (RailEntryModel, so a preview drag can MOVE rows rather than reset
        // them), so the roles are required properties on the delegate rather
        // than fields on a `modelData` map. What this case is here to hold is
        // unchanged: the nesting LEVEL is what drives the indent, and the
        // indent is a centre offset rather than a margin.
        QVERIFY(rail.contains(QStringLiteral("required property int level")));
        QVERIFY(rail.contains(QStringLiteral("hierarchyChild ?")));
        QVERIFY(rail.contains(
            QStringLiteral("anchors.horizontalCenterOffset:")));
        // And a Space inside a folder is indented by the same mechanism, so
        // the two groupings cannot render as two different kinds of nesting.
        QVERIFY(rail.contains(QStringLiteral("tileIndent")));
        QVERIFY(rail.contains(QStringLiteral("inFolder ? 7 : 0")));
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
