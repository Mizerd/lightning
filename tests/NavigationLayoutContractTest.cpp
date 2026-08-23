// 2026-08-23 navigation layouts: the host/presenter contract.
//
// A source-scan suite, deliberately. What it defends is not behaviour inside
// one component but the SEPARATION between them, and that separation is a
// property of which file contains what — which no runtime assertion can see.
//
// The invariants, each one a defect that would ship silently:
//
//  * The host owns the chrome. If a presenter grows its own workspace header
//    or search field, the two layouts fork and one of them stops getting
//    fixes. RoomsPanel keeps the header; neither presenter may declare one.
//  * A presenter never reaches up into the host by id. The extracted Classic
//    list originally called `leaveRoomConfirm.openFor(...)` and
//    `newConversationDialog.openDialog()` — resolved by scope, from a
//    delegate, through a parent chain. That is exactly how the reader
//    popover's click ended up silently dead (2026-08-19), and it breaks
//    without a warning the moment the component is instantiated anywhere
//    else. Signals only.
//  * Exactly one presenter is INSTANTIATED. Two visibility-gated room lists
//    is two sets of avatar fetches for one visible column, so the host uses
//    Loaders whose `active` is the layout choice.
//  * Channels never renders at Home. It shows one Space's hierarchy, so with
//    no Space there is nothing to show — and the host must fall back to
//    Classic rather than presenting an empty column.
//  * Classic is the DEFAULT and the clamp target. It is the layout that works
//    in an account with no Spaces at all.
//  * The Channels tokens are DERIVED. A new required key in eleven palettes
//    is how a theme ends up with one undefined colour and a transparent row.
#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QtTest/QtTest>

namespace {

QString read(const QString &name)
{
    QFile file(QStringLiteral(QML_DIR "/") + name);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

/// The file with its comments removed.
///
/// Every BAN assertion below must scan this, never the raw source. A comment
/// that NAMES the thing it explains why we do not do is the documented way a
/// ban regex fires on prose — it has cost this repo real time twice, and it
/// punishes exactly the comments worth writing.
QString withoutComments(const QString &source)
{
    QString out = source;
    // Block comments first, so a `//` inside one is not treated as a line.
    out.remove(QRegularExpression(QStringLiteral("/\\*.*?\\*/"),
                                  QRegularExpression::DotMatchesEverythingOption));
    out.remove(QRegularExpression(QStringLiteral("(?m)^\\s*//.*$")));
    // Trailing comments too, but not a `//` inside a string literal — which
    // in these files only ever appears in a URL, and there are none.
    out.remove(QRegularExpression(QStringLiteral("(?m)\\s//[^\"']*$")));
    return out;
}

QString readSrc(const QString &relative)
{
    QFile file(QStringLiteral(SRC_DIR "/") + relative);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

} // namespace

class NavigationLayoutContractTest : public QObject
{
    Q_OBJECT

private slots:
    void bothPresentersExistAndTheHostChoosesBetweenThem()
    {
        const QString host = read(QStringLiteral("RoomsPanel.qml"));
        QVERIFY(!host.isEmpty());
        QVERIFY(!read(QStringLiteral("RoomListClassicPresenter.qml")).isEmpty());
        QVERIFY(!read(QStringLiteral("RoomChannelsPresenter.qml")).isEmpty());

        QVERIFY2(host.contains(QStringLiteral("RoomListClassicPresenter {")),
                 "the host never instantiates the Classic presenter");
        QVERIFY2(host.contains(QStringLiteral("RoomChannelsPresenter {")),
                 "the host never instantiates the Channels presenter");
        QVERIFY2(host.contains(QStringLiteral("roomNavigationLayout")),
                 "the host never reads the layout preference");
    }

    void theHostKeepsTheChromeAndNeitherPresenterDeclaresItsOwn()
    {
        // The workspace header, the search field and the create/discover
        // dialogs are the host's. A presenter that grows its own is how the
        // two layouts stop getting the same fixes.
        const QString host = read(QStringLiteral("RoomsPanel.qml"));
        QVERIFY(host.contains(QStringLiteral("headerRow")));
        QVERIFY(host.contains(QStringLiteral("newConversationDialog")));

        for (const QString &name :
             { QStringLiteral("RoomListClassicPresenter.qml"),
               QStringLiteral("RoomChannelsPresenter.qml") }) {
            const QString presenter = withoutComments(read(name));
            QVERIFY2(!presenter.contains(QStringLiteral("id: headerRow")),
                     qPrintable(name + " declares its own workspace header"));
            QVERIFY2(!presenter.contains(QStringLiteral("NewConversationDialog")),
                     qPrintable(name + " declares its own create dialog"));
            QVERIFY2(!presenter.contains(QStringLiteral("VoiceConnectedBar")),
                     qPrintable(name + " declares its own call footer"));
        }
    }

    void noPresenterReachesUpIntoTheHostById()
    {
        // THE regression this suite is really for. These four ids live in
        // RoomsPanel and were resolved by scope from inside a delegate — the
        // 2026-08-19 dead-click shape, and silent the moment the component is
        // instantiated anywhere else.
        const QStringList hostIds = {
            QStringLiteral("newConversationDialog"),
            QStringLiteral("discoverJoinDialog"),
            QStringLiteral("leaveRoomConfirm"),
            QStringLiteral("roomLinkClipboard"),
        };
        for (const QString &name :
             { QStringLiteral("RoomListClassicPresenter.qml"),
               QStringLiteral("RoomChannelsPresenter.qml") }) {
            const QString presenter = withoutComments(read(name));
            QVERIFY(!presenter.isEmpty());
            for (const QString &hostId : hostIds) {
                QVERIFY2(!presenter.contains(hostId),
                         qPrintable(name + " reaches the host's '" + hostId
                                    + "' by id instead of by signal"));
            }
        }
        // ...and it asks by signal instead.
        const QString classic =
            read(QStringLiteral("RoomListClassicPresenter.qml"));
        QVERIFY(classic.contains(QStringLiteral("signal leaveRoomRequested")));
        QVERIFY(classic.contains(QStringLiteral("signal roomLinkCopyRequested")));
        QVERIFY(classic.contains(QStringLiteral("signal createRequested")));
        QVERIFY(classic.contains(QStringLiteral("signal roomActivated")));
    }

    void onlyOnePresenterIsInstantiated()
    {
        // Loader-gated, not visibility-gated: the layout that is not chosen
        // must build no ListView, no delegates and no empty state. Two live
        // room lists is two sets of avatar fetches for one visible column.
        const QString host = read(QStringLiteral("RoomsPanel.qml"));
        QString flat = host;
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        QVERIFY2(flat.contains(QStringLiteral(
                     "active: !parent.channelsUsable")),
                 "the Classic presenter is not gated on the layout choice");
        QVERIFY2(flat.contains(QStringLiteral(
                     "active: parent.channelsUsable")),
                 "the Channels presenter is not gated on the layout choice");
    }

    void channelsNeverRendersWithoutASpace()
    {
        // Channels shows ONE Space's hierarchy. At Home there is no
        // hierarchy, so the host falls back to Classic — rendering an empty
        // column instead would state something only this layout can state
        // ("this space has no channels") about a situation it does not apply
        // to ("you are not in a space").
        const QString host = read(QStringLiteral("RoomsPanel.qml"));
        QString flat = host;
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        QVERIFY2(flat.contains(QStringLiteral("spaceId.length > 0")),
                 "the host never requires a Space before using Channels");
        // And Settings says so, rather than leaving it to be discovered.
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(settings.contains(QStringLiteral("always use Classic")));
    }

    void classicIsTheDefaultAndTheClampTarget()
    {
        // 0 is Classic, and an out-of-range stored value must land there —
        // Classic is the layout that works in an account with no Spaces.
        const QString manager =
            readSrc(QStringLiteral("app/SettingsManager.cpp"));
        QVERIFY(!manager.isEmpty());
        const int at = manager.indexOf(
            QStringLiteral("SettingsManager::roomNavigationLayout"));
        QVERIFY2(at >= 0, "roomNavigationLayout has no accessor");
        // Wide enough to clear the explanatory comment above the return.
        const QString accessor = withoutComments(manager.mid(at, 900));
        QVERIFY2(accessor.contains(QStringLiteral("kRoomNavLayout, 0")),
                 "the default is not Classic");
        QVERIFY2(accessor.contains(QStringLiteral("? 0 :")),
                 "an out-of-range value does not clamp to Classic");
    }

    void theSettingIsAccountScoped()
    {
        // appearanceValue, like every other Appearance choice: a Space-heavy
        // work account and a DM-only personal account should not be forced
        // into one shape. And the switch must RE-ANNOUNCE it, or the column
        // keeps the previous account's layout.
        const QString manager =
            readSrc(QStringLiteral("app/SettingsManager.cpp"));
        const int at = manager.indexOf(
            QStringLiteral("SettingsManager::roomNavigationLayout"));
        QVERIFY(at >= 0);
        QVERIFY2(manager.mid(at, 400).contains(
                     QStringLiteral("appearanceValue")),
                 "the layout is not account-scoped");
        QVERIFY2(manager.contains(
                     QStringLiteral("Q_EMIT roomNavigationLayoutChanged();")),
                 "the layout is never announced");
        // Specifically on the account switch, next to the other per-account
        // appearance re-announcements.
        const int switchAt =
            manager.indexOf(QStringLiteral("Q_EMIT messageLayoutChanged();"));
        QVERIFY(switchAt >= 0);
        QVERIFY2(manager.mid(switchAt, 200)
                     .contains(QStringLiteral("roomNavigationLayoutChanged")),
                 "the layout is not re-announced on an account switch");
    }

    void everyChannelsTokenIsDerivedFromAnExistingOne()
    {
        // No new required palette key. Eleven palettes with one missing key
        // is how a theme ends up drawing a transparent row, and the
        // no-QML-warnings gate is the only thing that catches it.
        const QString theme = read(QStringLiteral("AppTheme.qml"));
        QVERIFY(!theme.isEmpty());
        const QStringList tokens = {
            QStringLiteral("channelCategoryText"),
            QStringLiteral("channelText"),
            QStringLiteral("channelTextUnread"),
            QStringLiteral("channelSelected"),
            QStringLiteral("channelSelectedText"),
            QStringLiteral("channelHover"),
            QStringLiteral("channelUnreadMark"),
        };
        QString flat = theme;
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        for (const QString &token : tokens) {
            const QString declaration =
                QStringLiteral("readonly property color ") + token + ":";
            QVERIFY2(flat.contains(declaration),
                     qPrintable(token + " is not declared"));
            const int at = flat.indexOf(declaration);
            const QString body = flat.mid(at, 220);
            QVERIFY2(body.contains(QStringLiteral("!== undefined")),
                     qPrintable(token + " does not fall back when a palette "
                                        "omits it"));
        }
    }

    void theChannelRowNeverClaimsUnknownEncryption()
    {
        // The lock glyph is a claim. It may only be drawn for encryption the
        // client KNOWS about; "not established yet" gets the plain hash.
        const QString manager =
            readSrc(QStringLiteral("spaces/SpaceManager.cpp"));
        const int at = manager.indexOf(
            QStringLiteral("SpaceManager::directChildRoomsDetailed"));
        QVERIFY2(at >= 0, "there is no direct-children accessor");
        QString body = manager.mid(at, 2000);
        body.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        QVERIFY2(body.contains(
                     QStringLiteral("it->encrypted && it->encryptionKnown")),
                 "encryption is reported without checking it is known");
    }

    void everyEmptyCapableLabelInTheNewRowsSitsBehindALoader()
    {
        // These are PER-ROW delegates. A never-laid-out empty Text keeps
        // ItemObservesViewport forever and makes Qt walk the whole
        // instantiated tree on every scroll frame — the single most
        // expensive QML mistake recorded in this repo.
        for (const QString &name : { QStringLiteral("ChannelDelegate.qml"),
                                     QStringLiteral("ChannelCategoryHeader.qml") }) {
            const QString source = read(name);
            QVERIFY2(!source.isEmpty(), qPrintable(name));
            QString flat = source;
            flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                         QStringLiteral(" "));
            // Every Label in these two files is inside a Loader's
            // sourceComponent, so the count of Labels must not exceed the
            // count of Loaders that wrap one.
            const int labels = flat.count(QStringLiteral("Label {"));
            const int wrapped =
                flat.count(QStringLiteral("sourceComponent: Label {"));
            QVERIFY2(labels == wrapped,
                     qPrintable(QStringLiteral("%1 has %2 Labels but only %3 "
                                               "behind a Loader")
                                    .arg(name)
                                    .arg(labels)
                                    .arg(wrapped)));
        }
    }

    void theUnreadPillIsSharedRatherThanReimplemented()
    {
        // Two hand-rolled pills is how one layout says "3" in danger ink and
        // the other in accent ink for the same room.
        QVERIFY(!read(QStringLiteral("UnreadBadge.qml")).isEmpty());
        QVERIFY(read(QStringLiteral("ChannelDelegate.qml"))
                    .contains(QStringLiteral("UnreadBadge {")));
        QVERIFY(read(QStringLiteral("ChannelCategoryHeader.qml"))
                    .contains(QStringLiteral("UnreadBadge {")));
        // Muting silences the count, never the mention: somebody naming you
        // is not noise.
        const QString badge = read(QStringLiteral("UnreadBadge.qml"));
        QVERIFY(badge.contains(QStringLiteral("root.mention ? AppTheme.dangerText")));
    }

    void aCollapsedCategoryStillReportsWhatItHides()
    {
        // Collapsing to save space must not silently mute a group.
        const QString header = read(QStringLiteral("ChannelCategoryHeader.qml"));
        QVERIFY(header.contains(QStringLiteral("hiddenHighlight")));
        QVERIFY(header.contains(QStringLiteral("hiddenUnread")));
        QString flat = header;
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        QVERIFY2(flat.contains(QStringLiteral("active: root.collapsed")),
                 "the hidden-activity indicator is not tied to collapse");
    }

    void aMutedChannelKeepsItsUnreadWeightButLosesItsPill()
    {
        // The user asked not to be counted at, not to be lied to about
        // whether anything happened.
        const QString row = read(QStringLiteral("ChannelDelegate.qml"));
        QString flat = row;
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        QVERIFY(flat.contains(QStringLiteral(
            "readonly property bool showsPill: root.highlightCount > 0 && !root.muted")));
        // And it re-reads the mode on an id change, because reuseItems is on
        // and a recycled row would otherwise inherit the previous room's.
        QVERIFY(row.contains(QStringLiteral("onRoomIdChanged")));
        QVERIFY(row.contains(QStringLiteral("refreshNotificationMode")));
    }

    void theChannelsPresenterDrawsNoSecondGrouping()
    {
        // The MODEL is already ordered and grouped by the hierarchy. A
        // ListView section header on top of the category rows would draw the
        // same grouping twice.
        const QString presenter = withoutComments(
            read(QStringLiteral("RoomChannelsPresenter.qml")));
        QVERIFY2(!presenter.contains(QStringLiteral("section.property")),
                 "the Channels list adds a second grouping mechanism");
    }

    void settingsOffersBothLayoutsAsPreviews()
    {
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(settings.contains(QStringLiteral("navLayoutClassicCard")));
        QVERIFY(settings.contains(QStringLiteral("navLayoutChannelsCard")));
        // A diagram, not a live instance: a real presenter in a settings card
        // would need a room list and would change while you looked at it.
        const QString card = withoutComments(
            read(QStringLiteral("NavigationLayoutCard.qml")));
        QVERIFY(!card.isEmpty());
        QVERIFY2(!card.contains(QStringLiteral("app.roomList")),
                 "the preview card binds to the real room list");
        QVERIFY2(!card.contains(QStringLiteral("RoomListClassicPresenter")),
                 "the preview card instantiates a real presenter");
    }
};

QTEST_MAIN(NavigationLayoutContractTest)
#include "NavigationLayoutContractTest.moc"
