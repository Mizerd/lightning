
#include "matrix/MatrixClient.h"
#include "models/TimelineModel.h"
#include "profile/UserProfileResolver.h"

#include <QScopeGuard>
#include <QTranslator>
#include <QtTest/QtTest>

namespace {

const QString kRoom = QStringLiteral("!room:example.org");

TimelineEvent makeEvent(const QString &eventId, const QString &body,
                        TimelineEvent::Status status = TimelineEvent::Sent)
{
    TimelineEvent e;
    e.eventId = eventId;
    e.itemId = QStringLiteral("uid-") + eventId;
    e.roomId = kRoom;
    e.sender = QStringLiteral("@alice:example.org");
    e.body = body;
    e.timestamp = QDateTime::fromMSecsSinceEpoch(1700000000000);
    e.status = status;
    return e;
}

class FakeClient : public MatrixClient
{
    Q_OBJECT
public:
    explicit FakeClient(QObject *parent = nullptr) : MatrixClient(parent) {}

    QList<TimelineEvent> mirror;
    QStringList retriedTransactions;
    QStringList cancelledTransactions;
    bool cancelSupported = true;
    QStringList typingUsers;
    QHash<QString, QString> displayNames;
    QHash<QString, QString> avatarMxc;
    mutable int displayNameLookups = 0;

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override
    {
        return QStringLiteral("@me:example.org");
    }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &roomId) const override
    {
        return (roomId == kRoom || (!mirrorAlias.isEmpty()
                                    && roomId == mirrorAlias))
            ? mirror : QList<TimelineEvent>{};
    }
    QString mirrorAlias;
    QHash<QString, QHash<QString, QString>> roomDisplayNames;
    QHash<QString, QHash<QString, QString>> roomAvatarMxc;
    mutable QStringList displayNameRoomsAsked;
    QString displayNameFor(const QString &roomId,
                           const QString &userId) const override
    {
        ++displayNameLookups;
        displayNameRoomsAsked.append(roomId);
        const auto scoped = roomDisplayNames.constFind(roomId);
        if (scoped != roomDisplayNames.constEnd()) {
            const auto name = scoped->constFind(userId);
            if (name != scoped->constEnd())
                return name.value();
        }
        if (!roomDisplayNames.isEmpty())
            return userId;
        return displayNames.value(userId, userId);
    }
    QString avatarMxcFor(const QString &roomId,
                         const QString &userId) const override
    {
        const auto scoped = roomAvatarMxc.constFind(roomId);
        if (scoped != roomAvatarMxc.constEnd())
            return scoped->value(userId);
        if (!roomAvatarMxc.isEmpty())
            return {};
        return avatarMxc.value(userId);
    }
    QStringList typingUsersFor(const QString &) const override { return typingUsers; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override
    {
        return {};
    }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &, const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    QStringList profileFetches;
    quint64 fetchUserProfile(const QString &userId) override
    {
        profileFetches.append(userId);
        return static_cast<quint64>(profileFetches.size());
    }
    QList<QPair<QString, QString>> readReceipts;
    void sendReadReceipt(const QString &roomId, const QString &eventId) override
    {
        readReceipts.append({ roomId, eventId });
    }
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }
    void retryFailedSend(const QString &, const QString &txn) override
    {
        retriedTransactions.append(txn);
    }
    bool supportsCancelSend() const override { return cancelSupported; }
    // Does not remove the row: an abort can lose the race with the server,
    // and only the backend's own answer may drop the item.
    void cancelSend(const QString &, const QString &txn) override
    {
        cancelledTransactions.append(txn);
    }
};

}

class TimelineModelDiffTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void consecutiveDeletionsCollapseIntoOneRow();
    void init();
    void cleanup();

    void voiceReplyPreviewsKeepCopyTextAsTheFileName();
    void aQuoteWithNoEmbeddedDetailsIsReadFromTheLoadedTarget();
    void aQuoteFollowsItsTargetWhenTheTargetChanges();
    void hiddenActivityKeepsSecurityChangesAndTheirDateVisible();
    void realCountExcludesVirtualRows();
    void aGalleryRowListsEveryItemForTheDelegateAndTheViewer();
    void appendAddsRow();
    void prependAddsRowsAtTop();
    void insertAtAddsRowInPlace();
    void changedAtUpdatesRowInPlace();
    void removedAtRemovesRow();
    void truncatedToShortens();
    void resetReloadsFromClient();
    void staleRoomSignalsIgnored();
    void invalidIndexSelfHeals();
    void undecryptableToDecryptedInPlace();
    void receiptsOnBodilessRowsLandOnTheRowAbove();
    void localEchoReconciliation();
    void retrySendRoutesTransaction();
    void retrySendIgnoresNonFailedRows();
    void cancelSendRoutesSendingAndFailedRowsOnly();
    void cancelSendNeverRemovesTheRowItself();
    void uploadProgressIsMinusOneUntilTheSdkReportsATotal();
    void loggedOutClearsModel();
    void stableRoleData();
    void roomMentionPresentationNeedsPushHighlight();
    void typingTextFormatsByCount();
    void senderIdentityRolesUseSdkProfileAndSafeAvatarMxc();
    void senderGroupingBoundariesAreDeterministic();
    void readMarkerDoesNotBreakSenderGroupButVisibleRowsDo();
    void senderGroupsBreakAtThreadContextAndFollowTheWindowPolicy();
    void anUndecryptableThreadReplyIsANoticeNotAMessage();
    void mediaAndPaginationPreserveSenderGrouping();
    void groupingRefreshCoalescesAcrossPrependBurst();
    void memberProfileUpdateEmitsIdentityRoles();
    void replyToSenderResolvesDisplayNameWithLocalpartFallback();
    void threadRoleIndexTracksEveryStructuralMutation();
    void clientSwitchDoesNotLeakSenderProfile();
    void messageActionsUseStableIdentityAndSafeMetadata();
    void staleAndInapplicableMessageActionsAreRejected();
    void moderatorsMayRemoveOthersMessagesButNeverEditThem();
    void anUndecryptableOwnMessageCannotBeEdited();
    void theWaveformRoleCarriesTheRawBuckets();
    void aSenderNameEqualToTheMxidFallsThroughToARealName();
    void pollRolesExposeOutcomeAndEndPermission();
    void readReceiptsRoleResolvesExcludesSelfAndSortsNewestFirst();
    void reactorsUseSeenNamesAndResolveProfilesWithRepaint();
    void aReaderMissingFromTheMemberCacheKeepsTheNameTheTimelineShows();
    void aSenderNameLosesItsDirectionOverridesFromEverySource();
    void theQuoteTypingLineAndPillsLoseDirectionOverridesToo();
    void aReaderKnownNowhereGetsTheirGlobalProfileAndTheChipRepaints();
    void theSentCheckFollowsTheNewestOwnMessageUntilSomeoneReadsIt();
    void theSentCheckGoesWhenTheOtherUserAnswersWithAReceiptOnTheirNewRow();
    void readReceiptsUpdateViaSetDiffAndMemberHydration();
    void memberHydrationRerendersOnlyRowsWhoseMentionsChanged();
    void aReaderShowsOnOneHostOnlyAndTheOldHostIsReannounced();
    void theSeenByListIsCumulativeFromTheClickedMessage();
    void removingTheNewestReceiptRowHandsTheReaderBackToTheOlderHost();
    void receiptOnlySetKeepsThreadIndexWithoutRebuild();
    void readMarkerRowsStayReceiptFreeWithoutIndexDrift();
    void twoReadersAdvanceIndependentlyWithoutLoss();
    void searchFindsMatchesAndNavigatesWithWrap();
    void searchUpdatesOnPaginationInsert();
    void searchClearsOnRoomSwitchAndEnd();
    void searchSurvivesEditAndExcludesRedacted();
    void messageSegmentsSplitCodeBlocksAndAreComputedOnce();
    void messageSegmentsStayEmptyAndFreeForOrdinaryBodies();
    void reactionRolesNameTheReactorsAndKeepTheUncappedTotal();
    // A thread model's roomId is the composite `room ␟ thread ␟ root`; no
    // room-keyed lookup may be made with it.
    void threadModelResolvesEveryIdentityAgainstTheRealRoom();
    void memberHydrationReachesAThreadModel();
    void anOrdinaryRoomModelStillAsksWithItsOwnRoomId();
    void markVisibleAsReadNeverSendsARoomReceiptForAThread();

private:
    QString openThreadModel();
    FakeClient *m_client = nullptr;
    TimelineModel *m_model = nullptr;
};

void TimelineModelDiffTest::init()
{
    m_client = new FakeClient(this);
    m_model = new TimelineModel(this);
    m_model->setClient(m_client);
    m_client->mirror = { makeEvent(QStringLiteral("$e0"), QStringLiteral("m0")),
                         makeEvent(QStringLiteral("$e1"), QStringLiteral("m1")) };
    m_model->setRoomId(kRoom);
    QCOMPARE(m_model->rowCount(), 2);
}

void TimelineModelDiffTest::roomMentionPresentationNeedsPushHighlight()
{
    const QString chip = m_model->markRoomMention(
        QStringLiteral("@room"), QStringLiteral("#e13239"),
        QStringLiteral("#ffffff"));
    QVERIFY(chip.contains(QStringLiteral("background-color:#e13239")));
    QVERIFY(chip.contains(QStringLiteral("color:#ffffff")));
    const QString escaped = m_model->markRoomMention(
        QStringLiteral("@room"), QStringLiteral("#e13239\" onclick=\"x"),
        QStringLiteral("#ffffff"));
    QVERIFY(!escaped.contains(QStringLiteral("onclick")));

    TimelineEvent room = makeEvent(QStringLiteral("$room-mention"),
                                   QStringLiteral("@room update"));
    room.mentionsRoom = true;
    room.pushVerdict = TimelineEvent::PushVerdict::Notify;
    m_client->mirror.append(room);
    Q_EMIT m_client->eventAppended(kRoom, room);
    const QModelIndex row = m_model->index(2);
    QCOMPARE(m_model->data(row, TimelineModel::MentionsRoomRole).toBool(), true);
    QCOMPARE(m_model->data(row, TimelineModel::RoomMentionHighlightedRole).toBool(), false);
    QCOMPARE(m_model->data(row, TimelineModel::PushHighlightedRole).toBool(), false);

    room.pushHighlight = true;
    room.roomMentionHighlighted = true;
    m_client->mirror[2] = room;
    Q_EMIT m_client->eventReplaced(kRoom, room.eventId, room);
    QCOMPARE(m_model->data(row, TimelineModel::RoomMentionHighlightedRole).toBool(), true);
    QCOMPARE(m_model->data(row, TimelineModel::PushHighlightedRole).toBool(), true);
}

void TimelineModelDiffTest::aGalleryRowListsEveryItemForTheDelegateAndTheViewer()
{
    TimelineEvent gallery = makeEvent(QStringLiteral("$g"), QString{});
    gallery.type = TimelineEvent::Image;
    gallery.mediaKey = QStringLiteral("$g");
    gallery.mediaFilename = QStringLiteral("before.png");
    gallery.mediaSourceAvailable = true;
    GalleryItem a;
    a.mediaKey = QStringLiteral("$g");
    a.kind = QStringLiteral("image");
    a.filename = QStringLiteral("before.png");
    a.width = 1280;
    GalleryItem b;
    b.mediaKey = QStringLiteral("$g#item1");
    b.kind = QStringLiteral("image");
    b.filename = QStringLiteral("after.png");
    b.thumbAvailable = true;
    gallery.galleryItems = { a, b };
    TimelineEvent reply = makeEvent(QStringLiteral("$r"), QStringLiteral("nice"));
    reply.replyToEventId = QStringLiteral("$g");
    reply.replyToKind = QStringLiteral("image");
    reply.replyToCount = 2;
    m_client->mirror = { gallery, reply };
    Q_EMIT m_client->timelineReset(kRoom);
    QCOMPARE(m_model->rowCount(), 2);

    int galleryRow = -1;
    int replyRow = -1;
    for (int r = 0; r < m_model->rowCount(); ++r) {
        const QString id = m_model->data(m_model->index(r), TimelineModel::EventIdRole)
                               .toString();
        if (id == QLatin1String("$g")) galleryRow = r;
        if (id == QLatin1String("$r")) replyRow = r;
    }
    QVERIFY(galleryRow >= 0 && replyRow >= 0);

    const QVariantList items =
        m_model->data(m_model->index(galleryRow), TimelineModel::GalleryItemsRole)
            .toList();
    QCOMPARE(items.size(), 2);
    QCOMPARE(items.at(1).toMap().value(QStringLiteral("mediaKey")).toString(),
             QStringLiteral("$g#item1"));
    QCOMPARE(items.at(1).toMap().value(QStringLiteral("filename")).toString(),
             QStringLiteral("after.png"));
    QVERIFY(items.at(1).toMap().value(QStringLiteral("thumbAvailable")).toBool());
    QVERIFY(m_model->roleNames().value(TimelineModel::GalleryItemsRole)
            == QByteArrayLiteral("galleryItems"));

    const QVariantList images = m_model->imageEntries();
    QCOMPARE(images.size(), 2);
    QCOMPARE(images.at(0).toMap().value(QStringLiteral("mediaKey")).toString(),
             QStringLiteral("$g"));
    QCOMPARE(images.at(1).toMap().value(QStringLiteral("mediaKey")).toString(),
             QStringLiteral("$g#item1"));

    QCOMPARE(m_model->data(m_model->index(replyRow), TimelineModel::ReplyToKindRole)
                 .toString(), QStringLiteral("image"));
    QCOMPARE(m_model->data(m_model->index(replyRow), TimelineModel::ReplyToCountRole)
                 .toInt(), 2);
    QVERIFY(m_model->data(m_model->index(replyRow), TimelineModel::GalleryItemsRole)
                .toList().isEmpty());
}

void TimelineModelDiffTest::cleanup()
{
    delete m_model;
    delete m_client;
    m_model = nullptr;
    m_client = nullptr;
}

void TimelineModelDiffTest::consecutiveDeletionsCollapseIntoOneRow()
{
    auto redacted = [](const QString &id) {
        auto e = makeEvent(id, QString());
        e.redacted = true;
        return e;
    };
    m_client->mirror = {
        makeEvent(QStringLiteral("$a"), QStringLiteral("m0")),
        redacted(QStringLiteral("$r1")),
        redacted(QStringLiteral("$r2")),
        redacted(QStringLiteral("$r3")),
        makeEvent(QStringLiteral("$b"), QStringLiteral("m1")),
        redacted(QStringLiteral("$r4")),
        makeEvent(QStringLiteral("$c"), QStringLiteral("m2")),
    };
    Q_EMIT m_client->timelineReset(kRoom);
    QCOMPARE(m_model->rowCount(), 7);

    const auto leader = [this](int row) {
        return m_model->data(m_model->index(row),
                             TimelineModel::DeletedGroupLeaderRole).toBool();
    };
    const auto count = [this](int row) {
        return m_model->data(m_model->index(row),
                             TimelineModel::DeletedGroupCountRole).toInt();
    };

    QVERIFY(leader(1));
    QCOMPARE(count(1), 3);
    QVERIFY(!leader(2));
    QVERIFY(!leader(3));
    QCOMPARE(count(2), 0);
    QCOMPARE(count(3), 0);

    QVERIFY(leader(5));
    QCOMPARE(count(5), 1);

    QVERIFY(leader(0));
    QVERIFY(leader(4));
    QVERIFY(leader(6));
    QCOMPARE(count(0), 0);
}

void TimelineModelDiffTest::realCountExcludesVirtualRows()
{
    TimelineEvent divider;
    divider.type = TimelineEvent::DateDivider;
    divider.itemId = QStringLiteral("uid-divider");
    divider.roomId = kRoom;
    divider.timestamp = QDateTime::fromMSecsSinceEpoch(1700000000000);
    QVERIFY2(divider.isVirtual(), "fixture assumption: a date divider is virtual");

    m_client->mirror = { makeEvent(QStringLiteral("$root"), QStringLiteral("root")),
                         divider,
                         makeEvent(QStringLiteral("$r1"), QStringLiteral("reply 1")),
                         makeEvent(QStringLiteral("$r2"), QStringLiteral("reply 2")) };
    m_model->setRoomId(QStringLiteral("!other:example.org"));
    m_model->setRoomId(kRoom);

    QCOMPARE(m_model->rowCount(), 4);
    QCOMPARE(m_model->property("count").toInt(), 4);
    QCOMPARE(m_model->property("realCount").toInt(), 3);
    QCOMPARE(m_model->property("realCount").toInt() - 1, 2);
}

void TimelineModelDiffTest::appendAddsRow()
{
    const auto e = makeEvent(QStringLiteral("$e2"), QStringLiteral("m2"));
    m_client->mirror.append(e);
    Q_EMIT m_client->eventAppended(kRoom, e);
    QCOMPARE(m_model->rowCount(), 3);
    QCOMPARE(m_model->data(m_model->index(2), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$e2"));
}

void TimelineModelDiffTest::prependAddsRowsAtTop()
{
    const QList<TimelineEvent> older = {
        makeEvent(QStringLiteral("$old0"), QStringLiteral("o0")),
        makeEvent(QStringLiteral("$old1"), QStringLiteral("o1")),
    };
    for (int i = older.size() - 1; i >= 0; --i)
        m_client->mirror.prepend(older.at(i));
    Q_EMIT m_client->eventsPrepended(kRoom, older);
    QCOMPARE(m_model->rowCount(), 4);
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$old0"));
    QCOMPARE(m_model->data(m_model->index(2), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$e0"));
}

void TimelineModelDiffTest::insertAtAddsRowInPlace()
{
    const auto e = makeEvent(QStringLiteral("$mid"), QStringLiteral("mid"));
    m_client->mirror.insert(1, e);
    Q_EMIT m_client->eventInsertedAt(kRoom, 1, e);
    QCOMPARE(m_model->rowCount(), 3);
    QCOMPARE(m_model->data(m_model->index(1), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$mid"));
}

void TimelineModelDiffTest::changedAtUpdatesRowInPlace()
{
    auto e = makeEvent(QStringLiteral("$e1"), QStringLiteral("edited"));
    e.edited = true;
    m_client->mirror[1] = e;
    QSignalSpy spy(m_model, &QAbstractItemModel::dataChanged);
    Q_EMIT m_client->eventChangedAt(kRoom, 1, e);
    QCOMPARE(m_model->rowCount(), 2);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(m_model->data(m_model->index(1), TimelineModel::BodyRole)
                 .toString(),
             QStringLiteral("edited"));
    QVERIFY(m_model->data(m_model->index(1), TimelineModel::EditedRole)
                .toBool());
}

void TimelineModelDiffTest::removedAtRemovesRow()
{
    m_client->mirror.removeAt(0);
    Q_EMIT m_client->eventRemovedAt(kRoom, 0);
    QCOMPARE(m_model->rowCount(), 1);
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$e1"));
}

void TimelineModelDiffTest::truncatedToShortens()
{
    m_client->mirror.append(makeEvent(QStringLiteral("$e2"), QStringLiteral("m2")));
    Q_EMIT m_client->eventAppended(kRoom, m_client->mirror.last());
    QCOMPARE(m_model->rowCount(), 3);
    while (m_client->mirror.size() > 1)
        m_client->mirror.removeLast();
    Q_EMIT m_client->eventsTruncatedTo(kRoom, 1);
    QCOMPARE(m_model->rowCount(), 1);
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$e0"));
}

void TimelineModelDiffTest::resetReloadsFromClient()
{
    m_client->mirror = { makeEvent(QStringLiteral("$fresh"),
                                   QStringLiteral("fresh")) };
    Q_EMIT m_client->timelineReset(kRoom);
    QCOMPARE(m_model->rowCount(), 1);
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$fresh"));
}

void TimelineModelDiffTest::staleRoomSignalsIgnored()
{
    const QString other = QStringLiteral("!other:example.org");
    Q_EMIT m_client->eventAppended(other, makeEvent(QStringLiteral("$x"),
                                                    QStringLiteral("x")));
    Q_EMIT m_client->eventInsertedAt(other, 0,
                                     makeEvent(QStringLiteral("$y"),
                                               QStringLiteral("y")));
    Q_EMIT m_client->eventRemovedAt(other, 0);
    Q_EMIT m_client->eventsTruncatedTo(other, 0);
    QCOMPARE(m_model->rowCount(), 2);
}

void TimelineModelDiffTest::invalidIndexSelfHeals()
{
    m_client->mirror = { makeEvent(QStringLiteral("$only"),
                                   QStringLiteral("only")) };
    Q_EMIT m_client->eventChangedAt(kRoom, 99,
                                    makeEvent(QStringLiteral("$z"),
                                              QStringLiteral("z")));
    QCOMPARE(m_model->rowCount(), 1);
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$only"));

    Q_EMIT m_client->eventRemovedAt(kRoom, -2);
    QCOMPARE(m_model->rowCount(), 1);
    Q_EMIT m_client->eventsTruncatedTo(kRoom, 12);
    QCOMPARE(m_model->rowCount(), 1);
}

void TimelineModelDiffTest::undecryptableToDecryptedInPlace()
{
    TimelineEvent utd = makeEvent(QStringLiteral("$enc"), QStringLiteral(""));
    utd.isEncrypted = true;
    utd.undecryptable = true;
    utd.body = QStringLiteral("[unable to decrypt yet]");
    m_client->mirror.append(utd);
    Q_EMIT m_client->eventAppended(kRoom, utd);
    QCOMPARE(m_model->rowCount(), 3);
    QVERIFY(m_model->data(m_model->index(2), TimelineModel::UndecryptableRole)
                .toBool());

    TimelineEvent decrypted = makeEvent(QStringLiteral("$enc"),
                                        QStringLiteral("secret hello"));
    decrypted.itemId = utd.itemId;
    decrypted.isEncrypted = true;
    decrypted.isDecrypted = true;
    m_client->mirror[2] = decrypted;
    Q_EMIT m_client->eventChangedAt(kRoom, 2, decrypted);

    QCOMPARE(m_model->rowCount(), 3);
    QVERIFY(!m_model->data(m_model->index(2), TimelineModel::UndecryptableRole)
                 .toBool());
    QVERIFY(m_model->data(m_model->index(2), TimelineModel::IsDecryptedRole)
                .toBool());
    QCOMPARE(m_model->data(m_model->index(2), TimelineModel::BodyRole)
                 .toString(),
             QStringLiteral("secret hello"));
    QCOMPARE(m_model->data(m_model->index(2), TimelineModel::ItemIdRole)
                 .toString(),
             utd.itemId);
}

void TimelineModelDiffTest::localEchoReconciliation()
{
    TimelineEvent echo = makeEvent(QString(), QStringLiteral("outgoing"),
                                   TimelineEvent::Sending);
    echo.itemId = QStringLiteral("uid-echo");
    echo.transactionId = QStringLiteral("txn1");
    echo.isLocalEcho = true;
    m_client->mirror.append(echo);
    Q_EMIT m_client->eventAppended(kRoom, echo);
    QCOMPARE(m_model->rowCount(), 3);
    QCOMPARE(m_model->data(m_model->index(2), TimelineModel::StatusRole)
                 .toInt(),
             int(TimelineEvent::Sending));
    QVERIFY(m_model->data(m_model->index(2), TimelineModel::IsLocalEchoRole)
                .toBool());

    TimelineEvent sent = echo;
    sent.eventId = QStringLiteral("$server");
    sent.status = TimelineEvent::Sent;
    sent.isLocalEcho = false;
    m_client->mirror[2] = sent;
    Q_EMIT m_client->eventChangedAt(kRoom, 2, sent);
    QCOMPARE(m_model->rowCount(), 3);
    QCOMPARE(m_model->data(m_model->index(2), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$server"));
    QCOMPARE(m_model->data(m_model->index(2), TimelineModel::StatusRole)
                 .toInt(),
             int(TimelineEvent::Sent));

    int occurrences = 0;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        if (m_model->data(m_model->index(row), TimelineModel::EventIdRole)
                .toString()
            == QStringLiteral("$server"))
            ++occurrences;
    }
    QCOMPARE(occurrences, 1);
}

void TimelineModelDiffTest::retrySendRoutesTransaction()
{
    TimelineEvent failed = makeEvent(QString(), QStringLiteral("nope"),
                                     TimelineEvent::Failed);
    failed.transactionId = QStringLiteral("txn-failed");
    failed.sendErrorCategory = QStringLiteral("network");
    m_client->mirror.append(failed);
    Q_EMIT m_client->eventAppended(kRoom, failed);

    m_model->retrySend(2);
    QCOMPARE(m_client->retriedTransactions.size(), 1);
    QCOMPARE(m_client->retriedTransactions.first(),
             QStringLiteral("txn-failed"));
}

void TimelineModelDiffTest::retrySendIgnoresNonFailedRows()
{
    m_model->retrySend(0);
    m_model->retrySend(99);
    m_model->retrySend(-1);
    QVERIFY(m_client->retriedTransactions.isEmpty());
}

void TimelineModelDiffTest::cancelSendRoutesSendingAndFailedRowsOnly()
{
    TimelineEvent sending = makeEvent(QString(), QStringLiteral("uploading"),
                                      TimelineEvent::Sending);
    sending.transactionId = QStringLiteral("txn-sending");
    m_client->mirror.append(sending);
    Q_EMIT m_client->eventAppended(kRoom, sending);
    TimelineEvent failed = makeEvent(QString(), QStringLiteral("nope"),
                                     TimelineEvent::Failed);
    failed.transactionId = QStringLiteral("txn-failed");
    m_client->mirror.append(failed);
    Q_EMIT m_client->eventAppended(kRoom, failed);

    QVERIFY(m_model->canCancelSend(2));
    QVERIFY(m_model->canCancelSend(3));
    QVERIFY(!m_model->canCancelSend(0));
    QVERIFY(!m_model->canCancelSend(99));
    QVERIFY(!m_model->canCancelSend(-1));

    m_model->cancelSend(2);
    m_model->cancelSend(3);
    m_model->cancelSend(0);
    m_model->cancelSend(99);
    m_model->cancelSend(-1);
    QCOMPARE(m_client->cancelledTransactions,
             (QStringList{ QStringLiteral("txn-sending"),
                           QStringLiteral("txn-failed") }));

    m_client->cancelSupported = false;
    QVERIFY(!m_model->canCancelSend(2));
    m_client->cancelledTransactions.clear();
    m_model->cancelSend(2);
    QVERIFY(m_client->cancelledTransactions.isEmpty());
}

void TimelineModelDiffTest::cancelSendNeverRemovesTheRowItself()
{
    TimelineEvent sending = makeEvent(QString(), QStringLiteral("uploading"),
                                      TimelineEvent::Sending);
    sending.transactionId = QStringLiteral("txn-sending");
    m_client->mirror.append(sending);
    Q_EMIT m_client->eventAppended(kRoom, sending);
    const int before = m_model->rowCount();

    m_model->cancelSend(before - 1);
    QCOMPARE(m_model->rowCount(), before);
    QCOMPARE(m_model->data(m_model->index(before - 1),
                           TimelineModel::StatusRole).toInt(),
             static_cast<int>(TimelineEvent::Sending));
}

void TimelineModelDiffTest::uploadProgressIsMinusOneUntilTheSdkReportsATotal()
{
    TimelineEvent sending = makeEvent(QString(), QStringLiteral("uploading"),
                                      TimelineEvent::Sending);
    sending.transactionId = QStringLiteral("txn-sending");
    m_client->mirror.append(sending);
    Q_EMIT m_client->eventAppended(kRoom, sending);
    const int row = m_model->rowCount() - 1;
    const auto progress = [this, row] {
        return m_model->data(m_model->index(row),
                             TimelineModel::UploadProgressRole).toDouble();
    };
    QCOMPARE(progress(), -1.0);

    sending.uploadedBytes = 0;
    sending.uploadTotalBytes = 400;
    m_client->mirror[row] = sending;
    Q_EMIT m_client->eventChangedAt(kRoom, row, sending);
    QCOMPARE(progress(), 0.0);

    sending.uploadedBytes = 100;
    m_client->mirror[row] = sending;
    Q_EMIT m_client->eventChangedAt(kRoom, row, sending);
    QCOMPARE(progress(), 0.25);

    sending.uploadedBytes = 900;
    m_client->mirror[row] = sending;
    Q_EMIT m_client->eventChangedAt(kRoom, row, sending);
    QCOMPARE(progress(), 1.0);
}

void TimelineModelDiffTest::loggedOutClearsModel()
{
    m_client->logout();
    QCOMPARE(m_model->rowCount(), 0);
    QCOMPARE(m_model->roomId(), QString());
}

void TimelineModelDiffTest::stableRoleData()
{
    const auto names = m_model->roleNames();
    QVERIFY(names.contains(TimelineModel::ItemIdRole));
    QCOMPARE(names.value(TimelineModel::ItemIdRole),
             QByteArrayLiteral("itemId"));
    QCOMPARE(names.value(TimelineModel::IsLocalEchoRole),
             QByteArrayLiteral("isLocalEcho"));
    QCOMPARE(names.value(TimelineModel::SendErrorRole),
             QByteArrayLiteral("sendErrorCategory"));
    QCOMPARE(names.value(TimelineModel::IsVirtualRole),
             QByteArrayLiteral("isVirtual"));
    QCOMPARE(names.value(TimelineModel::SenderAvatarMxcRole),
             QByteArrayLiteral("senderAvatarMxc"));
    QCOMPARE(names.value(TimelineModel::ShowSenderIdentityRole),
             QByteArrayLiteral("showSenderIdentity"));
    QCOMPARE(names.value(TimelineModel::StableEventIdRole),
             QByteArrayLiteral("stableEventId"));
    QCOMPARE(names.value(TimelineModel::EventIdRole),
             QByteArrayLiteral("eventId"));
    QCOMPARE(names.value(TimelineModel::UndecryptableRole),
             QByteArrayLiteral("undecryptable"));
}

void TimelineModelDiffTest::pollRolesExposeOutcomeAndEndPermission()
{
    TimelineEvent poll = makeEvent(QStringLiteral("$poll"),
                                   QStringLiteral("Favourite colour?"));
    poll.type = TimelineEvent::Poll;
    poll.pollQuestion = QStringLiteral("Favourite colour?");
    poll.pollKind = QStringLiteral("disclosed");
    poll.pollMaxSelections = 2;
    poll.pollTotalVoters = 3;
    PollAnswer blue;
    blue.id = QStringLiteral("a1");
    blue.text = QStringLiteral("Blue");
    blue.count = 2;
    blue.byMe = true;
    PollAnswer green;
    green.id = QStringLiteral("a2");
    green.text = QStringLiteral("Green");
    green.count = 1;
    poll.pollAnswers = { blue, green };
    m_client->mirror.append(poll);
    Q_EMIT m_client->eventAppended(kRoom, poll);

    const auto idx = m_model->index(2);
    QVERIFY(m_model->data(idx, TimelineModel::IsPollRole).toBool());
    QCOMPARE(m_model->data(idx, TimelineModel::PollQuestionRole).toString(),
             QStringLiteral("Favourite colour?"));
    QCOMPARE(m_model->data(idx, TimelineModel::PollKindRole).toString(),
             QStringLiteral("disclosed"));
    QCOMPARE(m_model->data(idx, TimelineModel::PollMaxSelectionsRole).toInt(),
             2);
    QCOMPARE(m_model->data(idx, TimelineModel::PollTotalVotersRole).toInt(),
             3);
    QVERIFY(!m_model->data(idx, TimelineModel::PollEndedRole).toBool());
    const QVariantList answers =
        m_model->data(idx, TimelineModel::PollAnswersRole).toList();
    QCOMPARE(answers.size(), 2);
    const QVariantMap first = answers.first().toMap();
    QCOMPARE(first.value(QStringLiteral("id")).toString(),
             QStringLiteral("a1"));
    QCOMPARE(first.value(QStringLiteral("count")).toInt(), 2);
    QVERIFY(first.value(QStringLiteral("byMe")).toBool());

    QVERIFY(!m_model->data(idx, TimelineModel::CanEndPollRole).toBool());

    TimelineEvent own = poll;
    own.eventId = QStringLiteral("$own-poll");
    own.itemId = QStringLiteral("uid-own-poll");
    own.sender = QStringLiteral("@me:example.org");
    m_client->mirror.append(own);
    Q_EMIT m_client->eventAppended(kRoom, own);
    const auto ownIdx = m_model->index(3);
    QVERIFY(m_model->data(ownIdx, TimelineModel::CanEndPollRole).toBool());

    TimelineEvent ended = own;
    ended.pollEnded = true;
    m_client->mirror[3] = ended;
    Q_EMIT m_client->eventChangedAt(kRoom, 3, ended);
    QVERIFY(m_model->data(ownIdx, TimelineModel::PollEndedRole).toBool());
    QVERIFY(!m_model->data(ownIdx, TimelineModel::CanEndPollRole).toBool());
    const auto names = m_model->roleNames();
    QCOMPARE(names.value(TimelineModel::IsPollRole),
             QByteArrayLiteral("isPoll"));
    QCOMPARE(names.value(TimelineModel::PollAnswersRole),
             QByteArrayLiteral("pollAnswers"));
    QCOMPARE(names.value(TimelineModel::CanEndPollRole),
             QByteArrayLiteral("canEndPoll"));
}

void TimelineModelDiffTest::readReceiptsRoleResolvesExcludesSelfAndSortsNewestFirst()
{
    m_client->displayNames.insert(QStringLiteral("@carol:example.org"),
                                  QStringLiteral("Carol"));
    m_client->avatarMxc.insert(QStringLiteral("@carol:example.org"),
                               QStringLiteral("mxc://example.org/carol"));

    TimelineEvent read = makeEvent(QStringLiteral("$read"),
                                   QStringLiteral("seen by others"));
    read.readBy = {
        { QStringLiteral("@me:example.org"),      Q_INT64_C(1700000005000) },
        { QStringLiteral("@alice:example.org"),   Q_INT64_C(1700000004000) },
        { QStringLiteral("@carol:example.org"),   Q_INT64_C(1700000001000) },
        { QStringLiteral("@unknown:example.org"), Q_INT64_C(1700000003000) },
    };
    m_client->mirror.append(read);
    Q_EMIT m_client->eventAppended(kRoom, read);

    const auto idx = m_model->index(2);
    const QVariantList receipts =
        m_model->data(idx, TimelineModel::ReadReceiptsRole).toList();
    QCOMPARE(receipts.size(), 3);
    QCOMPARE(receipts.at(0).toMap().value(QStringLiteral("userId")).toString(),
             QStringLiteral("@alice:example.org"));
    const QVariantMap unknown = receipts.at(1).toMap();
    QCOMPARE(unknown.value(QStringLiteral("userId")).toString(),
             QStringLiteral("@unknown:example.org"));
    QCOMPARE(unknown.value(QStringLiteral("tsMs")).toLongLong(),
             Q_INT64_C(1700000003000));
    QCOMPARE(unknown.value(QStringLiteral("displayName")).toString(),
             QStringLiteral("unknown"));
    QVERIFY(unknown.value(QStringLiteral("avatarMxc")).toString().isEmpty());
    const QVariantMap carol = receipts.at(2).toMap();
    QCOMPARE(carol.value(QStringLiteral("displayName")).toString(),
             QStringLiteral("Carol"));
    QCOMPARE(carol.value(QStringLiteral("avatarMxc")).toString(),
             QStringLiteral("mxc://example.org/carol"));

    QCOMPARE(m_model->data(idx, TimelineModel::ReadReceiptsTotalRole).toInt(),
             3);

    TimelineEvent bySender = makeEvent(QStringLiteral("$by-sender"),
                                       QStringLiteral("alice's message"));
    bySender.readBy = {
        { QStringLiteral("@carol:example.org"), Q_INT64_C(1700000006000) },
    };
    bySender.readByTotal = 30;
    m_client->mirror.append(bySender);
    Q_EMIT m_client->eventAppended(kRoom, bySender);
    const auto senderIdx = m_model->index(3);
    const QVariantList onOwn =
        m_model->data(senderIdx, TimelineModel::ReadReceiptsRole).toList();
    QCOMPARE(onOwn.size(), 1);
    QCOMPARE(onOwn.first().toMap().value(QStringLiteral("userId")).toString(),
             QStringLiteral("@carol:example.org"));
    QCOMPARE(m_model->data(senderIdx,
                           TimelineModel::ReadReceiptsTotalRole).toInt(),
             30);

    QVERIFY(m_model->data(m_model->index(0), TimelineModel::ReadReceiptsRole)
                .toList()
                .isEmpty());
    QCOMPARE(m_model->data(m_model->index(0),
                           TimelineModel::ReadReceiptsTotalRole).toInt(),
             0);
    QCOMPARE(m_model->roleNames().value(TimelineModel::ReadReceiptsRole),
             QByteArrayLiteral("readReceipts"));
    QCOMPARE(m_model->roleNames().value(TimelineModel::ReadReceiptsTotalRole),
             QByteArrayLiteral("readReceiptsTotal"));
}

void TimelineModelDiffTest::reactorsUseSeenNamesAndResolveProfilesWithRepaint()
{
    auto *profiles = new UserProfileResolver(m_model);
    profiles->setClient(m_client);
    m_model->setProfileResolver(profiles);
    const QString seen = QStringLiteral("@seen:example.org");
    const QString unknown = QStringLiteral("@unknown:example.org");
    auto spoke = makeEvent(QStringLiteral("$spoke"), QStringLiteral("hello"));
    spoke.sender = seen;
    spoke.senderDisplayName = QStringLiteral("Seen Name");
    auto reacted = makeEvent(QStringLiteral("$reacted"), QStringLiteral("answer"));
    Reaction reaction;
    reaction.key = QStringLiteral("+");
    reaction.count = 2;
    reaction.senders = { seen, unknown };
    reacted.reactions = { reaction };
    m_client->mirror = { spoke, reacted, reacted };
    Q_EMIT m_client->timelineReset(kRoom);
    const auto names = [this](int row) {
        return m_model->data(m_model->index(row), TimelineModel::ReactionsRole)
            .toList().first().toMap().value(QStringLiteral("reactorNames")).toStringList();
    };
    for (int i = 0; i < 3; ++i)
        QCOMPARE(names(1), (QStringList{ QStringLiteral("Seen Name"), QStringLiteral("unknown") }));
    QCOMPARE(m_client->profileFetches, QStringList{ unknown });
    QSignalSpy changed(m_model, &QAbstractItemModel::dataChanged);
    Q_EMIT m_client->userProfileFinished(1, true, unknown, QStringLiteral("Global Name"),
                                         QString(), QString());
    for (int row : { 1, 2 }) {
        bool repainted = false;
        for (const auto &args : std::as_const(changed)) {
            if (args.at(0).value<QModelIndex>().row() <= row
                && args.at(1).value<QModelIndex>().row() >= row
                && args.at(2).value<QList<int>>().contains(TimelineModel::ReactionsRole))
                repainted = true;
        }
        QVERIFY(repainted);
        QCOMPARE(names(row), (QStringList{ QStringLiteral("Seen Name"), QStringLiteral("Global Name") }));
    }
    m_client->displayNames[seen] = QStringLiteral("Room Name");
    QCOMPARE(names(1).first(), QStringLiteral("Room Name"));
    QCOMPARE(m_client->profileFetches, QStringList{ unknown });
    m_model->setProfileResolver(nullptr);
}

void TimelineModelDiffTest::aSenderNameLosesItsDirectionOverridesFromEverySource()
{
    const QString rlo = QString(QChar(0x202E));
    const QString mallory = QStringLiteral("@mallory:example.org");
    TimelineEvent carried = makeEvent(QStringLiteral("$bidi-carried"),
                                      QStringLiteral("hi"));
    carried.sender = mallory;
    carried.senderDisplayName = QStringLiteral("Mal") + rlo
                                + QStringLiteral("gpj.exe");
    m_client->mirror.append(carried);
    Q_EMIT m_client->eventAppended(kRoom, carried);
    QCOMPARE(m_model->data(m_model->index(2),
                           TimelineModel::SenderDisplayNameRole).toString(),
             QStringLiteral("Malgpj.exe"));

    const QString eve = QStringLiteral("@eve:example.org");
    m_client->displayNames.insert(eve, QString(QChar(0x2067))
                                           + QStringLiteral("Eve")
                                           + QString(QChar(0x2069)));
    TimelineEvent bare = makeEvent(QStringLiteral("$bidi-bare"),
                                   QStringLiteral("yo"));
    bare.sender = eve;
    m_client->mirror.append(bare);
    Q_EMIT m_client->eventAppended(kRoom, bare);
    QCOMPARE(m_model->data(m_model->index(3),
                           TimelineModel::SenderDisplayNameRole).toString(),
             QStringLiteral("Eve"));
}

void TimelineModelDiffTest::theQuoteTypingLineAndPillsLoseDirectionOverridesToo()
{
    const QString rlo = QString(QChar(0x202E));
    const QString mallory = QStringLiteral("@mallory:example.org");
    const QString hostile = QStringLiteral("Mal") + rlo
                            + QStringLiteral("gpj.exe");
    const QString ghost = QStringLiteral("@ghost:example.org");
    m_client->displayNames.insert(mallory, hostile);
    m_client->displayNames.insert(ghost, rlo + QString(QChar(0x2066)));

    TimelineEvent carried = makeEvent(QStringLiteral("$q-carried"),
                                      QStringLiteral("re"));
    carried.replyToSenderId = mallory;
    carried.replyToSender = hostile;
    TimelineEvent looked = makeEvent(QStringLiteral("$q-looked"),
                                     QStringLiteral("re"));
    looked.replyToSenderId = mallory;
    TimelineEvent legacy = makeEvent(QStringLiteral("$q-legacy"),
                                     QStringLiteral("re"));
    legacy.replyToSender = mallory;
    TimelineEvent blank = makeEvent(QStringLiteral("$q-blank"),
                                    QStringLiteral("re"));
    blank.replyToSenderId = ghost;
    blank.replyToSender = rlo;
    blank.sender = ghost;
    blank.senderDisplayName = rlo;
    TimelineEvent pill = makeEvent(QStringLiteral("$q-pill"),
                                   QStringLiteral("hi m"));
    pill.formattedBody = QStringLiteral(
        "<a href=\"https://matrix.to/#/@mallory:example.org\">@m</a> hi");
    m_client->mirror = { carried, looked, legacy, blank, pill };
    Q_EMIT m_client->timelineReset(kRoom);
    QCOMPARE(m_model->rowCount(), 5);

    const QString clean = QStringLiteral("Malgpj.exe");
    for (int row = 0; row < 3; ++row) {
        QCOMPARE(m_model->data(m_model->index(row),
                               TimelineModel::ReplyToSenderRole).toString(),
                 clean);
    }
    QCOMPARE(m_model->data(m_model->index(3),
                           TimelineModel::ReplyToSenderRole).toString(),
             QStringLiteral("ghost"));
    QCOMPARE(m_model->data(m_model->index(3),
                           TimelineModel::SenderDisplayNameRole).toString(),
             QStringLiteral("ghost"));
    QCOMPARE(m_model->data(m_model->index(4),
                           TimelineModel::FormattedBodyRole).toString(),
             QStringLiteral("<a href=\"mention:@mallory:example.org\">@")
                 + clean + QStringLiteral("</a> hi"));

    m_client->typingUsers = { mallory };
    Q_EMIT m_client->typingChanged(kRoom);
    QCOMPARE(m_model->typingText(), clean + QStringLiteral(" is typing…"));
    m_client->typingUsers = { ghost };
    Q_EMIT m_client->typingChanged(kRoom);
    QCOMPARE(m_model->typingText(), QStringLiteral("ghost is typing…"));
}

void TimelineModelDiffTest::aReaderMissingFromTheMemberCacheKeepsTheNameTheTimelineShows()
{
    const QString grok = QStringLiteral("@grok:example.org");
    TimelineEvent spoke = makeEvent(QStringLiteral("$grok-spoke"),
                                    QStringLiteral("hello"));
    spoke.sender = grok;
    spoke.senderDisplayName = QStringLiteral("Grok AI");
    spoke.senderAvatarUrl = QStringLiteral("mxc://example.org/grok");
    m_client->mirror.append(spoke);
    Q_EMIT m_client->eventAppended(kRoom, spoke);

    TimelineEvent read = makeEvent(QStringLiteral("$read-by-grok"),
                                   QStringLiteral("answer"));
    read.readBy = { { grok, Q_INT64_C(1700000009000) } };
    m_client->mirror.append(read);
    Q_EMIT m_client->eventAppended(kRoom, read);

    const QModelIndex idx = m_model->index(3);
    const QVariantList receipts =
        m_model->data(idx, TimelineModel::ReadReceiptsRole).toList();
    QCOMPARE(receipts.size(), 1);
    const QVariantMap reader = receipts.first().toMap();
    QCOMPARE(reader.value(QStringLiteral("userId")).toString(), grok);
    QCOMPARE(reader.value(QStringLiteral("displayName")).toString(),
             m_model->data(m_model->index(2),
                           TimelineModel::SenderDisplayNameRole).toString());
    QCOMPARE(reader.value(QStringLiteral("displayName")).toString(),
             QStringLiteral("Grok AI"));
    QCOMPARE(reader.value(QStringLiteral("avatarMxc")).toString(),
             QStringLiteral("mxc://example.org/grok"));

    m_client->displayNames.insert(grok, QStringLiteral("Grok (room)"));
    QCOMPARE(m_model->data(idx, TimelineModel::ReadReceiptsRole).toList()
                 .first().toMap().value(QStringLiteral("displayName"))
                 .toString(),
             QStringLiteral("Grok (room)"));
}

void TimelineModelDiffTest::aReaderKnownNowhereGetsTheirGlobalProfileAndTheChipRepaints()
{
    auto *profiles = new UserProfileResolver(m_model);
    profiles->setClient(m_client);
    m_model->setProfileResolver(profiles);

    const QString bob = QStringLiteral("@bob:example.org");
    TimelineEvent read = makeEvent(QStringLiteral("$read-by-bob"),
                                   QStringLiteral("seen"));
    read.readBy = { { bob, Q_INT64_C(1700000009000) } };
    m_client->mirror.append(read);
    Q_EMIT m_client->eventAppended(kRoom, read);
    const QModelIndex idx = m_model->index(2);

    for (int i = 0; i < 3; ++i) {
        QCOMPARE(m_model->data(idx, TimelineModel::ReadReceiptsRole).toList()
                     .first().toMap().value(QStringLiteral("displayName"))
                     .toString(),
                 QStringLiteral("bob"));
    }
    QCOMPARE(m_client->profileFetches, QStringList{ bob });

    QSignalSpy changed(m_model, &QAbstractItemModel::dataChanged);
    Q_EMIT m_client->userProfileFinished(1, true, bob, QStringLiteral("Bob B."),
                                         QStringLiteral("mxc://example.org/bob"),
                                         QString());
    bool repainted = false;
    for (const auto &args : std::as_const(changed)) {
        const auto roles = args.at(2).value<QList<int>>();
        if (args.at(0).value<QModelIndex>().row() <= idx.row()
            && args.at(1).value<QModelIndex>().row() >= idx.row()
            && roles.contains(TimelineModel::ReadReceiptsRole))
            repainted = true;
    }
    QVERIFY2(repainted, "the receipt row was not told its reader resolved");
    const QVariantMap reader =
        m_model->data(idx, TimelineModel::ReadReceiptsRole).toList()
            .first().toMap();
    QCOMPARE(reader.value(QStringLiteral("displayName")).toString(),
             QStringLiteral("Bob B."));
    QCOMPARE(reader.value(QStringLiteral("avatarMxc")).toString(),
             QStringLiteral("mxc://example.org/bob"));
    m_model->setProfileResolver(nullptr);
}

void TimelineModelDiffTest::theSeenByListIsCumulativeFromTheClickedMessage()
{
    auto a = makeEvent(QStringLiteral("$a"), QStringLiteral("one"));
    a.readBy = { { QStringLiteral("@bob:example.org"), Q_INT64_C(1700000001000) } };
    auto b = makeEvent(QStringLiteral("$b"), QStringLiteral("two"));
    b.readBy = { { QStringLiteral("@carol:example.org"), Q_INT64_C(1700000002000) },
                 { QStringLiteral("@me:example.org"),    Q_INT64_C(1700000002500) } };
    auto c = makeEvent(QStringLiteral("$c"), QStringLiteral("three"));
    c.readBy = { { QStringLiteral("@dave:example.org"), Q_INT64_C(1700000003000) } };
    m_client->mirror = { a, b, c };
    Q_EMIT m_client->timelineReset(kRoom);

    auto ids = [this](const QString &eventId) {
        QStringList out;
        const QVariantMap m = m_model->cumulativeReadersForEvent(eventId);
        for (const QVariant &v : m.value(QStringLiteral("readers")).toList())
            out << v.toMap().value(QStringLiteral("userId")).toString();
        return out;
    };
    QCOMPARE(ids(QStringLiteral("$c")),
             QStringList{ QStringLiteral("@dave:example.org") });
    QCOMPARE(ids(QStringLiteral("$b")),
             (QStringList{ QStringLiteral("@dave:example.org"),
                           QStringLiteral("@carol:example.org") }));
    QCOMPARE(ids(QStringLiteral("$a")),
             (QStringList{ QStringLiteral("@dave:example.org"),
                           QStringLiteral("@carol:example.org"),
                           QStringLiteral("@bob:example.org") }));
    const QVariantMap all = m_model->cumulativeReadersForEvent(QStringLiteral("$a"));
    QCOMPARE(all.value(QStringLiteral("total")).toInt(), 3);
    QCOMPARE(all.value(QStringLiteral("readers")).toList().at(2).toMap()
                 .value(QStringLiteral("tsMs")).toLongLong(),
             Q_INT64_C(1700000001000));
    QVERIFY(ids(QStringLiteral("$missing")).isEmpty());
}

void TimelineModelDiffTest::aReaderShowsOnOneHostOnlyAndTheOldHostIsReannounced()
{
    auto older = makeEvent(QStringLiteral("$older"), QStringLiteral("first"));
    older.readBy = { { QStringLiteral("@bob:example.org"), Q_INT64_C(1700000001000) } };
    auto newer = makeEvent(QStringLiteral("$newer"), QStringLiteral("second"));
    m_client->mirror = { older, newer };
    Q_EMIT m_client->timelineReset(kRoom);

    auto readers = [this](int row) {
        QStringList out;
        const QVariantList list =
            m_model->data(m_model->index(row), TimelineModel::ReadReceiptsRole).toList();
        for (const QVariant &v : list)
            out << v.toMap().value(QStringLiteral("userId")).toString();
        return out;
    };
    QCOMPARE(readers(0), QStringList{ QStringLiteral("@bob:example.org") });
    QCOMPARE(readers(1), QStringList{});

    newer.readBy = { { QStringLiteral("@bob:example.org"), Q_INT64_C(1700000002000) } };
    m_client->mirror[1] = newer;
    QSignalSpy changed(m_model, &QAbstractItemModel::dataChanged);
    Q_EMIT m_client->eventChangedAt(kRoom, 1, newer);

    QCOMPARE(readers(1), QStringList{ QStringLiteral("@bob:example.org") });
    QCOMPARE(readers(0), QStringList{});
    bool olderAnnounced = false;
    for (const auto &sig : changed) {
        if (sig.at(0).toModelIndex().row() == 0
            && sig.at(2).value<QList<int>>().contains(TimelineModel::ReadReceiptsRole))
            olderAnnounced = true;
    }
    QVERIFY2(olderAnnounced, "the host Bob left must be told to drop him");
}

void TimelineModelDiffTest::removingTheNewestReceiptRowHandsTheReaderBackToTheOlderHost()
{
    auto first = makeEvent(QStringLiteral("$first"), QStringLiteral("one"));
    first.readBy = { { QStringLiteral("@bob:example.org"), Q_INT64_C(1700000001000) } };
    auto middle = makeEvent(QStringLiteral("$middle"), QStringLiteral("two"));
    auto last = makeEvent(QStringLiteral("$last"), QStringLiteral("three"));
    last.readBy = { { QStringLiteral("@bob:example.org"), Q_INT64_C(1700000003000) } };
    m_client->mirror = { first, middle, last };
    Q_EMIT m_client->timelineReset(kRoom);

    auto readers = [this](int row) {
        QStringList out;
        const QVariantList list =
            m_model->data(m_model->index(row), TimelineModel::ReadReceiptsRole).toList();
        for (const QVariant &v : list)
            out << v.toMap().value(QStringLiteral("userId")).toString();
        return out;
    };
    QCOMPARE(readers(0), QStringList{});
    QCOMPARE(readers(2), QStringList{ QStringLiteral("@bob:example.org") });

    m_client->mirror.removeAt(2);
    QSignalSpy changed(m_model, &QAbstractItemModel::dataChanged);
    Q_EMIT m_client->eventRemovedAt(kRoom, 2);

    QCOMPARE(m_model->rowCount(), 2);
    QCOMPARE(readers(0), QStringList{ QStringLiteral("@bob:example.org") });
    bool firstAnnounced = false;
    for (const auto &sig : changed) {
        if (sig.at(0).toModelIndex().row() == 0
            && sig.at(2).value<QList<int>>().contains(TimelineModel::ReadReceiptsRole))
            firstAnnounced = true;
    }
    QVERIFY2(firstAnnounced, "the older host must be told Bob is back on it");
}

void TimelineModelDiffTest::memberHydrationRerendersOnlyRowsWhoseMentionsChanged()
{
    auto pill = makeEvent(QStringLiteral("$pill"), QStringLiteral("hi bob"));
    pill.formattedBody = QStringLiteral(
        "<a href=\"https://matrix.to/#/@bob:example.org\">@bob</a> hi");
    auto plain = makeEvent(QStringLiteral("$plain"), QStringLiteral("plain"));
    auto carol = makeEvent(QStringLiteral("$carol"), QStringLiteral("x"));
    carol.formattedBody = QStringLiteral(
        "<a href=\"https://matrix.to/#/@carol:example.org\">@Carol</a>");
    m_client->mirror = { pill, plain, carol };
    Q_EMIT m_client->timelineReset(kRoom);
    for (int row = 0; row < 3; ++row)
        m_model->data(m_model->index(row), TimelineModel::FormattedBodyRole);
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::FormattedBodyRole)
                 .toString(),
             QStringLiteral("<a href=\"mention:@bob:example.org\">@bob</a> hi"));

    m_client->displayNames.insert(QStringLiteral("@bob:example.org"),
                                  QStringLiteral("Bob Builder"));
    QSignalSpy changed(m_model, &QAbstractItemModel::dataChanged);
    Q_EMIT m_client->membersChanged(kRoom);

    int bodyAnnouncements = 0;
    QSet<int> bodyRows;
    bool identitySweep = false;
    for (const auto &sig : changed) {
        const QList<int> roles = sig.at(2).value<QList<int>>();
        const int first = sig.at(0).toModelIndex().row();
        const int last = sig.at(1).toModelIndex().row();
        if (roles.contains(TimelineModel::FormattedBodyRole)
            || roles.contains(TimelineModel::MessageSegmentsRole)) {
            ++bodyAnnouncements;
            for (int r = first; r <= last; ++r)
                bodyRows.insert(r);
        }
        if (roles.contains(TimelineModel::SenderDisplayNameRole)
            && first == 0 && last == 2)
            identitySweep = true;
    }
    QCOMPARE(bodyAnnouncements, 1);
    QCOMPARE(bodyRows, QSet<int>{ 0 });
    QVERIFY2(identitySweep, "sender identity is still refreshed on every row");
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::FormattedBodyRole)
                 .toString(),
             QStringLiteral(
                 "<a href=\"mention:@bob:example.org\">@Bob Builder</a> hi"));
    QCOMPARE(m_model->data(m_model->index(2), TimelineModel::FormattedBodyRole)
                 .toString(),
             QStringLiteral("<a href=\"mention:@carol:example.org\">@carol</a>"));

    changed.clear();
    Q_EMIT m_client->membersChanged(kRoom);
    for (const auto &sig : changed) {
        const QList<int> roles = sig.at(2).value<QList<int>>();
        QVERIFY(!roles.contains(TimelineModel::FormattedBodyRole));
        QVERIFY(!roles.contains(TimelineModel::MessageSegmentsRole));
    }
}

void TimelineModelDiffTest::readReceiptsUpdateViaSetDiffAndMemberHydration()
{
    TimelineEvent read = makeEvent(QStringLiteral("$e1"),
                                   QStringLiteral("m1"));
    read.readBy = {
        { QStringLiteral("@bob:example.org"), Q_INT64_C(1700000001000) },
    };
    m_client->mirror[1] = read;
    QSignalSpy spy(m_model, &QAbstractItemModel::dataChanged);
    Q_EMIT m_client->eventChangedAt(kRoom, 1, read);

    QCOMPARE(m_model->rowCount(), 2);
    QCOMPARE(spy.count(), 1);
    QVERIFY(spy.at(0).at(2).value<QVector<int>>().isEmpty());
    QVariantList receipts =
        m_model->data(m_model->index(1), TimelineModel::ReadReceiptsRole)
            .toList();
    QCOMPARE(receipts.size(), 1);
    QCOMPARE(receipts.first().toMap()
                 .value(QStringLiteral("displayName")).toString(),
             QStringLiteral("bob"));

    m_client->displayNames.insert(QStringLiteral("@bob:example.org"),
                                  QStringLiteral("Bob"));
    spy.clear();
    Q_EMIT m_client->membersChanged(kRoom);
    QVERIFY(spy.count() >= 1);
    const auto roles = spy.at(0).at(2).value<QVector<int>>();
    QVERIFY(roles.contains(TimelineModel::ReadReceiptsRole));
    receipts = m_model->data(m_model->index(1),
                             TimelineModel::ReadReceiptsRole).toList();
    QCOMPARE(receipts.first().toMap()
                 .value(QStringLiteral("displayName")).toString(),
             QStringLiteral("Bob"));

    TimelineEvent cleared = read;
    cleared.readBy.clear();
    m_client->mirror[1] = cleared;
    Q_EMIT m_client->eventChangedAt(kRoom, 1, cleared);
    QVERIFY(m_model->data(m_model->index(1), TimelineModel::ReadReceiptsRole)
                .toList()
                .isEmpty());
}

void TimelineModelDiffTest::receiptOnlySetKeepsThreadIndexWithoutRebuild()
{
    TimelineEvent reply = makeEvent(QStringLiteral("$reply"),
                                    QStringLiteral("in thread"));
    reply.threadRootId = QStringLiteral("$e0");
    m_client->mirror.append(reply);
    Q_EMIT m_client->eventAppended(kRoom, reply);
    const auto rootIdx = m_model->index(0);
    QVERIFY(m_model->data(rootIdx, TimelineModel::IsThreadRootRole).toBool());
    QCOMPARE(m_model->data(rootIdx,
                           TimelineModel::ThreadReplyCountRole).toInt(), 1);

    TimelineEvent withReceipt = reply;
    withReceipt.readBy = {
        { QStringLiteral("@bob:example.org"), Q_INT64_C(1700000002000) },
    };
    m_client->mirror[2] = withReceipt;
    Q_EMIT m_client->eventChangedAt(kRoom, 2, withReceipt);
    QVERIFY(m_model->data(rootIdx, TimelineModel::IsThreadRootRole).toBool());
    QCOMPARE(m_model->data(rootIdx,
                           TimelineModel::ThreadReplyCountRole).toInt(), 1);
    QCOMPARE(m_model->data(m_model->index(2),
                           TimelineModel::ReadReceiptsRole).toList().size(),
             1);

    TimelineEvent detached = withReceipt;
    detached.threadRootId.clear();
    m_client->mirror[2] = detached;
    Q_EMIT m_client->eventChangedAt(kRoom, 2, detached);
    QVERIFY(!m_model->data(rootIdx, TimelineModel::IsThreadRootRole).toBool());
    QCOMPARE(m_model->data(rootIdx,
                           TimelineModel::ThreadReplyCountRole).toInt(), 0);
}

void TimelineModelDiffTest::twoReadersAdvanceIndependentlyWithoutLoss()
{
    const auto readersOf = [this](int row) {
        QStringList ids;
        const QVariantList receipts =
            m_model->data(m_model->index(row),
                          TimelineModel::ReadReceiptsRole).toList();
        for (const QVariant &r : receipts)
            ids.append(r.toMap().value(QStringLiteral("userId")).toString());
        return ids;
    };
    const QString anna = QStringLiteral("@anna:example.org");
    const QString ben = QStringLiteral("@ben:example.org");

    TimelineEvent m1 = makeEvent(QStringLiteral("$e1"), QStringLiteral("m1"));
    m1.readBy = { { anna, Q_INT64_C(1700000001000) },
                  { ben, Q_INT64_C(1700000002000) } };
    m_client->mirror[1] = m1;
    Q_EMIT m_client->eventChangedAt(kRoom, 1, m1);
    QCOMPARE(readersOf(1), QStringList({ ben, anna }));

    TimelineEvent m2 = makeEvent(QStringLiteral("$e2"), QStringLiteral("m2"));
    m_client->mirror.append(m2);
    Q_EMIT m_client->eventAppended(kRoom, m2);
    TimelineEvent m1OnlyBen = m1;
    m1OnlyBen.readBy = { { ben, Q_INT64_C(1700000002000) } };
    m_client->mirror[1] = m1OnlyBen;
    Q_EMIT m_client->eventChangedAt(kRoom, 1, m1OnlyBen);
    TimelineEvent m2Anna = m2;
    m2Anna.readBy = { { anna, Q_INT64_C(1700000003000) } };
    m_client->mirror[2] = m2Anna;
    Q_EMIT m_client->eventChangedAt(kRoom, 2, m2Anna);
    QCOMPARE(readersOf(1), QStringList({ ben }));
    QCOMPARE(readersOf(2), QStringList({ anna }));

    TimelineEvent m1Empty = m1OnlyBen;
    m1Empty.readBy.clear();
    m_client->mirror[1] = m1Empty;
    Q_EMIT m_client->eventChangedAt(kRoom, 1, m1Empty);
    TimelineEvent m2Both = m2Anna;
    m2Both.readBy = { { anna, Q_INT64_C(1700000003000) },
                      { ben, Q_INT64_C(1700000004000) } };
    m_client->mirror[2] = m2Both;
    Q_EMIT m_client->eventChangedAt(kRoom, 2, m2Both);
    QVERIFY(readersOf(1).isEmpty());
    QCOMPARE(readersOf(2), QStringList({ ben, anna }));

    TimelineEvent older = makeEvent(QStringLiteral("$older"),
                                    QStringLiteral("history"));
    m_client->mirror.insert(0, older);
    Q_EMIT m_client->eventInsertedAt(kRoom, 0, older);
    QCOMPARE(m_model->data(m_model->index(3),
                           TimelineModel::EventIdRole).toString(),
             QStringLiteral("$e2"));
    QCOMPARE(readersOf(3), QStringList({ ben, anna }));
    QVERIFY(readersOf(0).isEmpty());

    m_client->displayNames.insert(anna, QStringLiteral("Anna"));
    m_client->displayNames.insert(ben, QStringLiteral("Ben"));
    Q_EMIT m_client->membersChanged(kRoom);
    QCOMPARE(readersOf(3), QStringList({ ben, anna }));
    const QVariantList hydrated =
        m_model->data(m_model->index(3),
                      TimelineModel::ReadReceiptsRole).toList();
    QCOMPARE(hydrated.at(0).toMap()
                 .value(QStringLiteral("displayName")).toString(),
             QStringLiteral("Ben"));
    QCOMPARE(hydrated.at(1).toMap()
                 .value(QStringLiteral("displayName")).toString(),
             QStringLiteral("Anna"));
}

void TimelineModelDiffTest::theSentCheckFollowsTheNewestOwnMessageUntilSomeoneReadsIt()
{
    const auto sent = [this](int row) {
        return m_model->data(m_model->index(row), TimelineModel::SentReceiptRole)
            .toBool();
    };
    QCOMPARE(m_model->roleNames().value(TimelineModel::SentReceiptRole),
             QByteArrayLiteral("sentReceipt"));
    QVERIFY(!sent(0));
    QVERIFY(!sent(1));

    TimelineEvent mine = makeEvent(QStringLiteral("$mine"), QStringLiteral("hi"));
    mine.sender = QStringLiteral("@me:example.org");
    m_client->mirror.append(mine);
    Q_EMIT m_client->eventAppended(kRoom, mine);
    QVERIFY2(sent(2), "the newest own message carries the sent check");

    TimelineEvent echo = makeEvent(QStringLiteral("local:1"), QStringLiteral("typing"),
                                   TimelineEvent::Sending);
    echo.sender = QStringLiteral("@me:example.org");
    echo.isLocalEcho = true;
    m_client->mirror.append(echo);
    Q_EMIT m_client->eventAppended(kRoom, echo);
    QVERIFY(sent(2));
    QVERIFY(!sent(3));

    QSignalSpy changed(m_model, &QAbstractItemModel::dataChanged);
    TimelineEvent second = makeEvent(QStringLiteral("$mine2"), QStringLiteral("again"));
    second.sender = QStringLiteral("@me:example.org");
    m_client->mirror.append(second);
    Q_EMIT m_client->eventAppended(kRoom, second);
    QVERIFY(!sent(2));
    QVERIFY(sent(4));
    bool oldRowTold = false;
    for (const auto &sig : changed) {
        if (sig.at(0).toModelIndex().row() == 2
            && sig.at(2).value<QList<int>>().contains(TimelineModel::SentReceiptRole))
            oldRowTold = true;
    }
    QVERIFY2(oldRowTold, "the row that lost the check was not announced");

    TimelineEvent theirs = makeEvent(QStringLiteral("$theirs"), QStringLiteral("yo"));
    theirs.readBy = { { QStringLiteral("@alice:example.org"), Q_INT64_C(1700000009000) } };
    m_client->mirror.append(theirs);
    Q_EMIT m_client->eventAppended(kRoom, theirs);
    QVERIFY2(!sent(4), "a read message kept its sent check");

    TimelineEvent unread = theirs;
    unread.readBy.clear();
    m_client->mirror[5] = unread;
    Q_EMIT m_client->eventChangedAt(kRoom, 5, unread);
    QVERIFY(sent(4));
    TimelineEvent readNow = mine;
    readNow.eventId = second.eventId;
    readNow.itemId = second.itemId;
    readNow.readBy = { { QStringLiteral("@alice:example.org"), Q_INT64_C(1700000010000) } };
    m_client->mirror[4] = readNow;
    Q_EMIT m_client->eventChangedAt(kRoom, 4, readNow);
    QVERIFY2(!sent(4), "a receipt delivered by a Set did not hide the check");
    readNow.readBy = { { QStringLiteral("@me:example.org"), Q_INT64_C(1700000011000) } };
    m_client->mirror[4] = readNow;
    Q_EMIT m_client->eventChangedAt(kRoom, 4, readNow);
    QVERIFY(sent(4));
}

void TimelineModelDiffTest::theSentCheckGoesWhenTheOtherUserAnswersWithAReceiptOnTheirNewRow()
{
    TimelineEvent mine = makeEvent(QStringLiteral("$mine"), QStringLiteral("hi"));
    mine.sender = QStringLiteral("@me:example.org");
    m_client->mirror.append(mine);
    Q_EMIT m_client->eventAppended(kRoom, mine);
    const int mineRow = static_cast<int>(m_model->rowCount()) - 1;
    const auto sent = [this](int row) {
        return m_model->data(m_model->index(row), TimelineModel::SentReceiptRole)
            .toBool();
    };
    QVERIFY(sent(mineRow));

    bool viewSawInsertion = false;
    QObject::connect(m_model, &QAbstractItemModel::rowsInserted, m_model,
                     [&] { viewSawInsertion = true; });
    bool announcedTooEarly = false;
    bool announced = false;
    QObject::connect(m_model, &QAbstractItemModel::dataChanged, m_model,
                     [&](const QModelIndex &top, const QModelIndex &,
                         const QList<int> &roles) {
        if (top.row() == mineRow && roles.contains(TimelineModel::SentReceiptRole)) {
            announced = true;
            if (!viewSawInsertion)
                announcedTooEarly = true;
        }
    });

    for (int i = 0; i < 4; ++i) {
        TimelineEvent theirs = makeEvent(QStringLiteral("$theirs%1").arg(i),
                                         QStringLiteral("reply %1").arg(i));
        if (i == 3) {
            theirs.readBy = { { QStringLiteral("@alice:example.org"),
                                Q_INT64_C(1700000020000) } };
        }
        m_client->mirror.append(theirs);
        viewSawInsertion = false;
        Q_EMIT m_client->eventAppended(kRoom, theirs);
    }
    QVERIFY2(!sent(mineRow), "the check stayed under an answered message");
    QVERIFY2(announced, "the row that lost the check was never announced");
    QVERIFY2(!announcedTooEarly,
             "the check's removal was announced before the view had seen the "
             "insertion, so a real view drops it");
}

void TimelineModelDiffTest::readMarkerRowsStayReceiptFreeWithoutIndexDrift()
{
    TimelineEvent marker;
    marker.roomId = kRoom;
    marker.itemId = QStringLiteral("read-marker-stable");
    marker.type = TimelineEvent::ReadMarker;
    TimelineEvent read = makeEvent(QStringLiteral("$after"),
                                   QStringLiteral("after the marker"));
    read.readBy = {
        { QStringLiteral("@bob:example.org"), Q_INT64_C(1700000002000) },
    };
    m_client->mirror.insert(1, marker);
    Q_EMIT m_client->eventInsertedAt(kRoom, 1, marker);
    m_client->mirror.append(read);
    Q_EMIT m_client->eventAppended(kRoom, read);

    QCOMPARE(m_model->rowCount(), 4);
    QVERIFY(m_model->data(m_model->index(1), TimelineModel::IsVirtualRole)
                .toBool());
    QVERIFY(m_model->data(m_model->index(1), TimelineModel::ReadReceiptsRole)
                .toList()
                .isEmpty());
    QCOMPARE(m_model->data(m_model->index(2), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$e1"));
    QCOMPARE(m_model->data(m_model->index(3), TimelineModel::ReadReceiptsRole)
                 .toList()
                 .size(),
             1);
    QVERIFY(m_model->data(m_model->index(2),
                          TimelineModel::ContinuesSenderGroupRole).toBool());
}

void TimelineModelDiffTest::typingTextFormatsByCount()
{
    m_client->displayNames.insert(QStringLiteral("@a:example.org"),
                                  QStringLiteral("Alice"));
    m_client->displayNames.insert(QStringLiteral("@b:example.org"),
                                  QStringLiteral("Bob"));

    m_client->typingUsers = {};
    Q_EMIT m_client->typingChanged(kRoom);
    QCOMPARE(m_model->typingText(), QString{});

    m_client->typingUsers = { QStringLiteral("@me:example.org") };
    Q_EMIT m_client->typingChanged(kRoom);
    QCOMPARE(m_model->typingText(), QString{});

    m_client->typingUsers = { QStringLiteral("@a:example.org") };
    Q_EMIT m_client->typingChanged(kRoom);
    QCOMPARE(m_model->typingText(), QStringLiteral("Alice is typing…"));

    m_client->typingUsers = { QStringLiteral("@a:example.org"),
                              QStringLiteral("@b:example.org") };
    Q_EMIT m_client->typingChanged(kRoom);
    QCOMPARE(m_model->typingText(),
             QStringLiteral("Alice and Bob are typing…"));

    m_client->typingUsers = { QStringLiteral("@a:example.org"),
                              QStringLiteral("@b:example.org"),
                              QStringLiteral("@c:example.org") };
    Q_EMIT m_client->typingChanged(kRoom);
    QCOMPARE(m_model->typingText(),
             QStringLiteral("3 people are typing…"));
}

void TimelineModelDiffTest::senderIdentityRolesUseSdkProfileAndSafeAvatarMxc()
{
    auto profiled = makeEvent(QStringLiteral("$profiled"), QStringLiteral("hello"));
    profiled.senderDisplayName = QStringLiteral("Alice Smith");
    profiled.senderAvatarUrl = QStringLiteral("mxc://example.org/alice-avatar");
    m_client->mirror = { profiled };
    Q_EMIT m_client->timelineReset(kRoom);

    const QModelIndex row = m_model->index(0);
    QCOMPARE(m_model->data(row, TimelineModel::SenderRole).toString(),
             QStringLiteral("@alice:example.org"));
    QCOMPARE(m_model->data(row, TimelineModel::SenderDisplayNameRole).toString(),
             QStringLiteral("Alice Smith"));
    QCOMPARE(m_model->data(row, TimelineModel::SenderInitialsRole).toString(),
             QStringLiteral("AS"));
    QCOMPARE(m_model->data(row, TimelineModel::SenderAvatarMxcRole).toString(),
             QStringLiteral("mxc://example.org/alice-avatar"));
    QCOMPARE(m_model->data(row, TimelineModel::StableEventIdRole).toString(),
             QStringLiteral("uid-$profiled"));
    QVERIFY(m_model->data(row, TimelineModel::BeginsSenderGroupRole).toBool());
    QVERIFY(m_model->data(row, TimelineModel::ShowSenderIdentityRole).toBool());
    QVERIFY(m_model->data(row, TimelineModel::EndsSenderGroupRole).toBool());
}

void TimelineModelDiffTest::senderGroupingBoundariesAreDeterministic()
{
    const QDateTime base(QDate(2026, 7, 13), QTime(12, 0), QTimeZone::UTC);
    auto first = makeEvent(QStringLiteral("$first"), QStringLiteral("one"));
    first.timestamp = base;
    auto continuation = makeEvent(QStringLiteral("$second"), QStringLiteral("two"));
    continuation.timestamp = base.addSecs(60);
    auto senderChange = makeEvent(QStringLiteral("$third"), QStringLiteral("three"));
    senderChange.sender = QStringLiteral("@bob:example.org");
    senderChange.timestamp = base.addSecs(120);
    auto timeGap = makeEvent(QStringLiteral("$fourth"), QStringLiteral("four"));
    timeGap.sender = senderChange.sender;
    timeGap.timestamp = senderChange.timestamp.addSecs(5 * 60);
    auto beforeMidnight = makeEvent(QStringLiteral("$fifth"), QStringLiteral("five"));
    beforeMidnight.sender = QStringLiteral("@carol:example.org");
    beforeMidnight.timestamp = QDateTime(QDate(2026, 7, 13), QTime(23, 59), QTimeZone::UTC);
    auto afterMidnight = makeEvent(QStringLiteral("$sixth"), QStringLiteral("six"));
    afterMidnight.sender = beforeMidnight.sender;
    afterMidnight.timestamp = beforeMidnight.timestamp.addSecs(120);
    m_client->mirror = { first, continuation, senderChange, timeGap,
                         beforeMidnight, afterMidnight };
    Q_EMIT m_client->timelineReset(kRoom);

    QVERIFY(m_model->data(m_model->index(0), TimelineModel::BeginsSenderGroupRole).toBool());
    QVERIFY(m_model->data(m_model->index(1), TimelineModel::ContinuesSenderGroupRole).toBool());
    QVERIFY(m_model->data(m_model->index(1), TimelineModel::EndsSenderGroupRole).toBool());
    QVERIFY(m_model->data(m_model->index(2), TimelineModel::BeginsSenderGroupRole).toBool());
    QVERIFY(m_model->data(m_model->index(3), TimelineModel::BeginsSenderGroupRole).toBool());
    QVERIFY(m_model->data(m_model->index(5), TimelineModel::BeginsSenderGroupRole).toBool());

    first.sender = QStringLiteral("@me:example.org");
    continuation.sender = first.sender;
    m_client->mirror = { first, continuation };
    Q_EMIT m_client->timelineReset(kRoom);
    QVERIFY(m_model->data(m_model->index(0), TimelineModel::IsOwnRole).toBool());
    QVERIFY(m_model->data(m_model->index(0), TimelineModel::BeginsSenderGroupRole).toBool());
    QVERIFY(m_model->data(m_model->index(1), TimelineModel::ContinuesSenderGroupRole).toBool());
}

void TimelineModelDiffTest::senderGroupsBreakAtThreadContextAndFollowTheWindowPolicy()
{
    const QDateTime base(QDate(2026, 7, 13), QTime(12, 0), QTimeZone::UTC);
    auto room = makeEvent(QStringLiteral("$room"), QStringLiteral("in the room"));
    room.timestamp = base;
    auto inThread = makeEvent(QStringLiteral("$reply"), QStringLiteral("in a thread"));
    inThread.timestamp = base.addSecs(30);
    inThread.threadRootId = QStringLiteral("$root");
    auto inThreadToo = makeEvent(QStringLiteral("$reply2"), QStringLiteral("same thread"));
    inThreadToo.timestamp = base.addSecs(40);
    inThreadToo.threadRootId = QStringLiteral("$root");
    auto later = makeEvent(QStringLiteral("$later"), QStringLiteral("twenty minutes on"));
    later.timestamp = inThreadToo.timestamp.addSecs(20 * 60);
    later.threadRootId = QStringLiteral("$root");
    m_client->mirror = { room, inThread, inThreadToo, later };
    Q_EMIT m_client->timelineReset(kRoom);

    QVERIFY2(m_model->data(m_model->index(1), TimelineModel::BeginsSenderGroupRole).toBool(),
             "a thread reply after the same sender's room message must start a group");
    QVERIFY(m_model->data(m_model->index(2), TimelineModel::ContinuesSenderGroupRole).toBool());
    QVERIFY(m_model->data(m_model->index(3), TimelineModel::BeginsSenderGroupRole).toBool());

    QSignalSpy changed(m_model, &QAbstractItemModel::dataChanged);
    m_model->setSenderGroupWindowSeconds(60 * 60);
    QCOMPARE(changed.count(), 1);
    QVERIFY(m_model->data(m_model->index(3), TimelineModel::ContinuesSenderGroupRole).toBool());
    QVERIFY(m_model->data(m_model->index(1), TimelineModel::BeginsSenderGroupRole).toBool());
    m_model->setSenderGroupWindowSeconds(TimelineModel::kDefaultSenderGroupWindowSeconds);
    QVERIFY(m_model->data(m_model->index(3), TimelineModel::BeginsSenderGroupRole).toBool());
}

void TimelineModelDiffTest::readMarkerDoesNotBreakSenderGroupButVisibleRowsDo()
{
    const QDateTime base = QDateTime::fromMSecsSinceEpoch(1700000000000);
    auto first = makeEvent(QStringLiteral("$first"), QStringLiteral("one"));
    first.timestamp = base;
    TimelineEvent marker;
    marker.roomId = kRoom;
    marker.itemId = QStringLiteral("read-marker-stable");
    marker.type = TimelineEvent::ReadMarker;
    auto second = makeEvent(QStringLiteral("$second"), QStringLiteral("two"));
    second.timestamp = base.addSecs(30);
    TimelineEvent activity;
    activity.roomId = kRoom;
    activity.sender = first.sender;
    activity.type = TimelineEvent::StateChange;
    activity.timestamp = base.addSecs(40);
    auto third = makeEvent(QStringLiteral("$third"), QStringLiteral("three"));
    third.timestamp = base.addSecs(50);
    TimelineEvent divider;
    divider.roomId = kRoom;
    divider.type = TimelineEvent::DateDivider;
    auto fourth = makeEvent(QStringLiteral("$fourth"), QStringLiteral("four"));
    fourth.timestamp = base.addSecs(60);
    m_client->mirror = { first, marker, second, activity, third, divider, fourth };
    Q_EMIT m_client->timelineReset(kRoom);

    QVERIFY(m_model->data(m_model->index(2), TimelineModel::ContinuesSenderGroupRole).toBool());
    QCOMPARE(m_model->stableIdAt(1), QStringLiteral("read-marker-stable"));
    QVERIFY(m_model->data(m_model->index(4), TimelineModel::BeginsSenderGroupRole).toBool());
    QVERIFY(m_model->data(m_model->index(6), TimelineModel::BeginsSenderGroupRole).toBool());
    QVERIFY(!m_model->data(m_model->index(3), TimelineModel::ShowSenderIdentityRole).toBool());
}

void TimelineModelDiffTest::anUndecryptableThreadReplyIsANoticeNotAMessage()
{
    const QDateTime base = QDateTime::fromMSecsSinceEpoch(1700000000000);
    auto first = makeEvent(QStringLiteral("$first"), QStringLiteral("one"));
    first.timestamp = base;
    auto reply = makeEvent(QStringLiteral("$reply"), QString());
    reply.sender = QStringLiteral("@bob:example.org");
    reply.timestamp = base.addSecs(20);
    reply.undecryptable = true;
    reply.threadRootId = QStringLiteral("$root");
    ReadReceipt reader;
    reader.userId = QStringLiteral("@carol:example.org");
    reader.tsMs = 1700000030000;
    reply.readBy = { reader };
    auto second = makeEvent(QStringLiteral("$second"), QStringLiteral("two"));
    second.timestamp = base.addSecs(40);
    auto plainUtd = makeEvent(QStringLiteral("$plainutd"), QString());
    plainUtd.timestamp = base.addSecs(50);
    plainUtd.undecryptable = true;
    m_client->mirror = { first, reply, second, plainUtd };
    Q_EMIT m_client->timelineReset(kRoom);
    QCOMPARE(m_model->rowCount(), 4);

    const auto awaiting = [this](int row) {
        return m_model->data(m_model->index(row),
                             TimelineModel::ThreadReplyAwaitingKeysRole).toBool();
    };
    QVERIFY(awaiting(1));
    QVERIFY(!awaiting(0));
    QVERIFY(!awaiting(3));
    QCOMPARE(m_model->data(m_model->index(1), TimelineModel::ThreadRootIdRole)
                 .toString(), QStringLiteral("$root"));
    QVERIFY(m_model->data(m_model->index(2),
                          TimelineModel::ContinuesSenderGroupRole).toBool());
    QVERIFY(m_model->data(m_model->index(1), TimelineModel::ReadReceiptsRole)
                .toList().isEmpty());
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::ReadReceiptsRole)
                 .toList().size(), 1);

    openThreadModel();
    Q_EMIT m_client->timelineReset(MatrixClient::threadTimelineId(
        kRoom, QStringLiteral("$root")));
    QCOMPARE(m_model->rowCount(), 4);
    QVERIFY(!awaiting(1));
}

void TimelineModelDiffTest::mediaAndPaginationPreserveSenderGrouping()
{
    const QDateTime base = QDateTime::fromMSecsSinceEpoch(1700000000000);
    auto image = makeEvent(QStringLiteral("$image"), QStringLiteral("cat.gif"));
    image.type = TimelineEvent::Image;
    image.timestamp = base.addSecs(30);
    m_client->mirror = { image };
    Q_EMIT m_client->timelineReset(kRoom);

    auto older = makeEvent(QStringLiteral("$older"), QStringLiteral("older"));
    older.timestamp = base;
    m_client->mirror.prepend(older);
    Q_EMIT m_client->eventsPrepended(kRoom, QList<TimelineEvent>{ older });
    QVERIFY(m_model->data(m_model->index(1), TimelineModel::ContinuesSenderGroupRole).toBool());
    QVERIFY(m_model->data(m_model->index(1), TimelineModel::EndsSenderGroupRole).toBool());

    auto remoteEcho = image;
    remoteEcho.eventId = QStringLiteral("$remote-image");
    remoteEcho.isLocalEcho = false;
    m_client->mirror[1] = remoteEcho;
    Q_EMIT m_client->eventReplaced(kRoom, QStringLiteral("$image"), remoteEcho);
    QVERIFY(m_model->data(m_model->index(1), TimelineModel::ContinuesSenderGroupRole).toBool());
}

void TimelineModelDiffTest::groupingRefreshCoalescesAcrossPrependBurst()
{
    const QDateTime base = QDateTime::fromMSecsSinceEpoch(1700000000000);
    auto isGroupingChange = [](const QList<QVariant> &args) {
        const auto roles = args.at(2).value<QList<int>>();
        return roles.contains(TimelineModel::BeginsSenderGroupRole);
    };
    QSignalSpy changed(m_model, &QAbstractItemModel::dataChanged);
    auto countGrouping = [&] {
        int n = 0;
        for (const auto &sig : changed)
            if (isGroupingChange(sig))
                ++n;
        return n;
    };

    const int kPage = 12;
    for (int i = kPage - 1; i >= 0; --i) {
        auto older = makeEvent(QStringLiteral("$old%1").arg(i),
                               QStringLiteral("o%1").arg(i));
        older.timestamp = base.addSecs(-60 * (kPage - i));
        m_client->mirror.prepend(older);
        Q_EMIT m_client->eventsPrepended(kRoom, QList<TimelineEvent>{ older });
    }
    QCOMPARE(m_model->rowCount(), 2 + kPage);

    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$old0"));

    QVERIFY(countGrouping() > 0);
    const int rows = m_model->rowCount();
    for (const auto &sig : changed) {
        if (!isGroupingChange(sig))
            continue;
        const int first = sig.at(0).toModelIndex().row();
        const int last = sig.at(1).toModelIndex().row();
        QVERIFY(first >= 0);
        QVERIFY(last < rows);
        QVERIFY2(last - first + 1 <= 4,
                 "grouping refresh must stay local to its boundary");
    }

    changed.clear();
    auto live = makeEvent(QStringLiteral("$live"), QStringLiteral("live"));
    live.timestamp = base.addSecs(120);
    m_client->mirror.append(live);
    Q_EMIT m_client->eventAppended(kRoom, live);
    QVERIFY(countGrouping() > 0);
    for (const auto &sig : changed) {
        if (!isGroupingChange(sig))
            continue;
        const int first = sig.at(0).toModelIndex().row();
        const int last = sig.at(1).toModelIndex().row();
        QVERIFY2(last - first + 1 <= 4,
                 "grouping refresh must stay local to its boundary");
    }
}

void TimelineModelDiffTest::memberProfileUpdateEmitsIdentityRoles()
{
    m_client->mirror[0].senderDisplayName.clear();
    m_client->mirror[0].senderAvatarUrl.clear();
    Q_EMIT m_client->timelineReset(kRoom);
    m_client->displayNames.insert(QStringLiteral("@alice:example.org"),
                                  QStringLiteral("Alice Updated"));
    m_client->avatarMxc.insert(QStringLiteral("@alice:example.org"),
                               QStringLiteral("mxc://example.org/new-avatar"));
    QSignalSpy changed(m_model, &QAbstractItemModel::dataChanged);
    Q_EMIT m_client->membersChanged(kRoom);
    QCOMPARE(changed.count(), 1);
    const QList<int> roles = changed.first().at(2).value<QList<int>>();
    QVERIFY(roles.contains(TimelineModel::SenderDisplayNameRole));
    QVERIFY(roles.contains(TimelineModel::SenderInitialsRole));
    QVERIFY(roles.contains(TimelineModel::SenderAvatarMxcRole));
    QVERIFY(!roles.contains(TimelineModel::FormattedBodyRole));
    QVERIFY(!roles.contains(TimelineModel::MessageSegmentsRole));
    QVERIFY(roles.contains(TimelineModel::ReplyToSenderRole));
    QVERIFY(roles.contains(TimelineModel::ThreadLatestSenderDisplayNameRole));
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::SenderInitialsRole).toString(),
             QStringLiteral("AU"));
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::SenderAvatarMxcRole).toString(),
             QStringLiteral("mxc://example.org/new-avatar"));
}

void TimelineModelDiffTest::aQuoteWithNoEmbeddedDetailsIsReadFromTheLoadedTarget()
{
    auto target = makeEvent(QStringLiteral("$target"), QStringLiteral("the original"));
    target.sender = QStringLiteral("@bob:example.org");
    auto reply = makeEvent(QStringLiteral("$reply"), QStringLiteral("answer"));
    reply.replyToEventId = target.eventId;
    m_client->mirror = { reply };
    Q_EMIT m_client->timelineReset(kRoom);
    const auto role = [&](const QString &id, int r) {
        for (int i = 0; i < m_model->rowCount(); ++i) {
            if (m_model->eventIdAt(i) == id)
                return m_model->data(m_model->index(i), r);
        }
        return QVariant();
    };
    QVERIFY(role(reply.eventId, TimelineModel::ReplyToPreviewRole).toString().isEmpty());

    QSignalSpy changed(m_model, &QAbstractItemModel::dataChanged);
    m_client->mirror = { target, reply };
    Q_EMIT m_client->eventInsertedAt(kRoom, 0, target);
    bool quoteRefreshed = false;
    for (const auto &args : changed) {
        const QModelIndex first = args.at(0).value<QModelIndex>();
        if (m_model->eventIdAt(first.row()) == reply.eventId
            && args.at(2).value<QList<int>>().contains(
                TimelineModel::ReplyToPreviewRole))
            quoteRefreshed = true;
    }
    QVERIFY2(quoteRefreshed, "the quote was not re-read when its target arrived");
    QCOMPARE(role(reply.eventId, TimelineModel::ReplyToPreviewRole).toString(),
             QStringLiteral("the original"));
    QCOMPARE(role(reply.eventId, TimelineModel::ReplyToSenderIdRole).toString(),
             QStringLiteral("@bob:example.org"));

    auto own = makeEvent(QStringLiteral("$own"), QStringLiteral("x"));
    own.replyToEventId = target.eventId;
    own.replyToPreview = QStringLiteral("embedded wins");
    own.replyToSender = QStringLiteral("carol");
    m_client->mirror = { target, reply, own };
    Q_EMIT m_client->timelineReset(kRoom);
    QCOMPARE(role(own.eventId, TimelineModel::ReplyToPreviewRole).toString(),
             QStringLiteral("embedded wins"));
}

void TimelineModelDiffTest::aQuoteFollowsItsTargetWhenTheTargetChanges()
{
    auto target = makeEvent(QStringLiteral("$t"), QStringLiteral("first text"));
    auto reply = makeEvent(QStringLiteral("$r"), QStringLiteral("answer"));
    reply.replyToEventId = target.eventId;
    auto own = makeEvent(QStringLiteral("$o"), QStringLiteral("x"));
    own.replyToEventId = target.eventId;
    own.replyToPreview = QStringLiteral("embedded");
    own.replyToMediaKey = QStringLiteral("$cached-image");
    own.replyToKind = QStringLiteral("image");
    m_client->mirror = { target, reply, own };
    Q_EMIT m_client->timelineReset(kRoom);
    const auto rowOf = [&](const QString &id) {
        for (int i = 0; i < m_model->rowCount(); ++i) {
            if (m_model->eventIdAt(i) == id)
                return i;
        }
        return -1;
    };
    const auto preview = [&](const QString &id) {
        return m_model->data(m_model->index(rowOf(id)),
                             TimelineModel::ReplyToPreviewRole).toString();
    };
    QCOMPARE(preview(reply.eventId), QStringLiteral("first text"));

    QSignalSpy changed(m_model, &QAbstractItemModel::dataChanged);
    auto edited = target;
    edited.body = QStringLiteral("edited text");
    m_client->mirror = { edited, reply, own };
    Q_EMIT m_client->eventChangedAt(kRoom, rowOf(target.eventId), edited);
    QCOMPARE(preview(reply.eventId), QStringLiteral("edited text"));
    bool replyReread = false, ownReread = false;
    for (const auto &args : changed) {
        const int row = args.at(0).value<QModelIndex>().row();
        const auto roles = args.at(2).value<QList<int>>();
        if (!roles.contains(TimelineModel::ReplyToPreviewRole))
            continue;
        replyReread |= m_model->eventIdAt(row) == reply.eventId;
        ownReread |= m_model->eventIdAt(row) == own.eventId;
    }
    QVERIFY2(replyReread, "the quote was not re-read when its target changed");
    QVERIFY2(!ownReread, "a reply with embedded details was re-read");
    QCOMPARE(preview(own.eventId), QStringLiteral("embedded"));

    auto gone = edited;
    gone.redacted = true;
    m_client->mirror = { gone, reply, own };
    Q_EMIT m_client->eventChangedAt(kRoom, rowOf(target.eventId), gone);
    QVERIFY(preview(reply.eventId).isEmpty());
    QVERIFY(preview(own.eventId).isEmpty());
    const QModelIndex ownIndex = m_model->index(rowOf(own.eventId));
    QVERIFY(m_model->data(ownIndex, TimelineModel::ReplyToRedactedRole).toBool());
    QVERIFY(m_model->data(ownIndex, TimelineModel::ReplyToMediaKeyRole)
                .toString().isEmpty());
    QVERIFY(m_model->data(ownIndex, TimelineModel::ReplyToKindRole)
                .toString().isEmpty());
}

void TimelineModelDiffTest::hiddenActivityKeepsSecurityChangesAndTheirDateVisible()
{
    auto date = makeEvent(QStringLiteral("$date"), QString{});
    date.type = TimelineEvent::DateDivider;
    auto join = makeEvent(QStringLiteral("$join"), QStringLiteral("joined"));
    join.type = TimelineEvent::StateChange;
    join.stateKind = QStringLiteral("membership");
    auto grant = makeEvent(QStringLiteral("$grant"), QStringLiteral("Permissions changed"));
    grant.type = TimelineEvent::StateChange;
    grant.stateKind = QStringLiteral("m.room.power_levels");
    m_client->mirror = { date, join, grant };
    Q_EMIT m_client->timelineReset(kRoom);
    m_model->setShowRoomActivity(false);

    const QVariantList entries = m_model->data(m_model->index(1),
        TimelineModel::StateGroupEntriesRole).toList();
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().toMap().value(QStringLiteral("eventId")).toString(),
             grant.eventId);
    QVERIFY(m_model->data(m_model->index(0),
        TimelineModel::DividerIntroducesVisibleContentRole).toBool());
    QVERIFY(m_model->activityKindVisible(grant.stateKind));
    QVERIFY(!m_model->activityKindVisible(join.stateKind));
}

void TimelineModelDiffTest::voiceReplyPreviewsKeepCopyTextAsTheFileName()
{
    auto voice = makeEvent(QStringLiteral("$voice"), QStringLiteral("voice-1.ogg"));
    voice.type = TimelineEvent::Audio;
    voice.mediaIsVoice = true;
    auto audio = makeEvent(QStringLiteral("$audio"), QStringLiteral("song.ogg"));
    audio.type = TimelineEvent::Audio;
    auto quote = makeEvent(QStringLiteral("$quote"), QStringLiteral("reply"));
    quote.replyToEventId = voice.eventId;
    quote.replyToPreview = voice.body;
    quote.replyToKind = QStringLiteral("audio");
    m_client->mirror = { voice, audio, quote };
    Q_EMIT m_client->timelineReset(kRoom);
    QCOMPARE(m_model->replyPreviewForEvent(voice.eventId), QStringLiteral("Voice message"));
    QCOMPARE(m_model->visibleTextForEvent(voice.eventId), voice.body);
    QCOMPARE(m_model->replyPreviewForEvent(audio.eventId), audio.body);
    QCOMPARE(m_model->replyPreviewForEvent(QStringLiteral("$missing")), QString());
    int row = -1;
    for (int i = 0; i < m_model->rowCount(); ++i) {
        if (m_model->eventIdAt(i) == quote.eventId)
            row = i;
    }
    QVERIFY(row >= 0);
    QCOMPARE(m_model->data(m_model->index(row), TimelineModel::ReplyToPreviewRole).toString(),
             QStringLiteral("Voice message"));

    struct VoiceTranslator : QTranslator {
        QString translate(const char *context, const char *source, const char *,
                          int) const override
        {
            if (QByteArray(context) == "EventPreview"
                && QByteArray(source) == "Voice message")
                return QStringLiteral("Balso žinutė");
            return {};
        }
        bool isEmpty() const override { return false; }
    } translator;
    QVERIFY(QCoreApplication::installTranslator(&translator));
    const auto removeTranslator = qScopeGuard([&] {
        QCoreApplication::removeTranslator(&translator);
    });
    QCOMPARE(m_model->replyPreviewForEvent(voice.eventId), QStringLiteral("Balso žinutė"));
    QCOMPARE(m_model->data(m_model->index(row), TimelineModel::ReplyToPreviewRole).toString(),
             QStringLiteral("Balso žinutė"));
    QCOMPARE(m_model->visibleTextForEvent(voice.eventId), voice.body);
    QCOMPARE(m_model->replyPreviewForEvent(audio.eventId), audio.body);

    voice.redacted = true;
    m_client->mirror = { voice, audio, quote };
    Q_EMIT m_client->timelineReset(kRoom);
    QVERIFY(m_model->replyPreviewForEvent(voice.eventId).isEmpty());
}

void TimelineModelDiffTest::replyToSenderResolvesDisplayNameWithLocalpartFallback()
{
    auto reply = makeEvent(QStringLiteral("$reply"), QStringLiteral("hi"));
    reply.replyToEventId = QStringLiteral("$orig");
    reply.replyToSender = QStringLiteral("@maya:example.org");
    m_client->mirror = { reply };
    Q_EMIT m_client->timelineReset(kRoom);

    QCOMPARE(m_model->data(m_model->index(0),
                           TimelineModel::ReplyToSenderRole).toString(),
             QStringLiteral("maya"));

    m_client->displayNames.insert(QStringLiteral("@maya:example.org"),
                                  QStringLiteral("Maya Chen"));
    Q_EMIT m_client->membersChanged(kRoom);
    QCOMPARE(m_model->data(m_model->index(0),
                           TimelineModel::ReplyToSenderRole).toString(),
             QStringLiteral("Maya Chen"));

    auto root = makeEvent(QStringLiteral("$root"), QStringLiteral("topic"));
    root.isThreadRoot = true;
    root.threadLatestSender = QStringLiteral("@maya:example.org");
    m_client->mirror = { root };
    Q_EMIT m_client->timelineReset(kRoom);
    QCOMPARE(m_model->data(m_model->index(0),
                           TimelineModel::ThreadLatestSenderDisplayNameRole)
                 .toString(),
             QStringLiteral("Maya Chen"));
    m_client->mirror[0].threadLatestSenderDisplayName =
        QStringLiteral("Maya (SDK)");
    Q_EMIT m_client->timelineReset(kRoom);
    QCOMPARE(m_model->data(m_model->index(0),
                           TimelineModel::ThreadLatestSenderDisplayNameRole)
                 .toString(),
             QStringLiteral("Maya (SDK)"));
}

void TimelineModelDiffTest::clientSwitchDoesNotLeakSenderProfile()
{
    m_client->mirror[0].senderDisplayName = QStringLiteral("Alice Account A");
    m_client->mirror[0].senderAvatarUrl = QStringLiteral("mxc://a/avatar");
    Q_EMIT m_client->timelineReset(kRoom);

    FakeClient other;
    auto otherEvent = makeEvent(QStringLiteral("$other"), QStringLiteral("other"));
    otherEvent.sender = QStringLiteral("@mallory:other.org");
    otherEvent.senderDisplayName = QStringLiteral("Mallory Account B");
    otherEvent.senderAvatarUrl = QStringLiteral("mxc://b/avatar");
    other.mirror = { otherEvent };
    m_model->setClient(&other);
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::SenderDisplayNameRole).toString(),
             QStringLiteral("Mallory Account B"));
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::SenderAvatarMxcRole).toString(),
             QStringLiteral("mxc://b/avatar"));
    m_model->setClient(nullptr);
}

void TimelineModelDiffTest::messageActionsUseStableIdentityAndSafeMetadata()
{
    auto own = makeEvent(QStringLiteral("$own:example.org"),
                         QStringLiteral("visible text"));
    own.sender = QStringLiteral("@me:example.org");
    own.edited = true;
    own.isEncrypted = true;
    own.isDecrypted = true;
    own.replyToEventId = QStringLiteral("$target:example.org");
    m_client->mirror = { own };
    Q_EMIT m_client->loginSucceeded(QStringLiteral("@me:example.org"));
    Q_EMIT m_client->timelineReset(kRoom);

    QCOMPARE(m_model->visibleTextForEvent(own.eventId),
             QStringLiteral("visible text"));
    const QString link = m_model->messagePermalink(own.eventId);
    QCOMPARE(link, QStringLiteral(
        "https://matrix.to/#/!room:example.org/$own:example.org"));
    QVERIFY(!link.contains(QStringLiteral("token"), Qt::CaseInsensitive));
    QVERIFY(m_model->canEditEvent(own.eventId));
    QVERIFY(m_model->canRedactEvent(own.eventId));

    const QVariantMap details = m_model->messageDetails(own.eventId);
    QCOMPARE(details.value(QStringLiteral("eventId")).toString(), own.eventId);
    QCOMPARE(details.value(QStringLiteral("roomId")).toString(), kRoom);
    QCOMPARE(details.value(QStringLiteral("senderId")).toString(), own.sender);
    QCOMPARE(details.value(QStringLiteral("encryption")).toString(),
             QStringLiteral("Encrypted"));
    QCOMPARE(details.value(QStringLiteral("decryption")).toString(),
             QStringLiteral("Decrypted"));
    QCOMPARE(details.value(QStringLiteral("replyTargetId")).toString(),
             own.replyToEventId);
    QVERIFY(!details.contains(QStringLiteral("body")));
    QVERIFY(!details.contains(QStringLiteral("mediaUrl")));
    QVERIFY(!details.contains(QStringLiteral("rawJson")));
}

void TimelineModelDiffTest::staleAndInapplicableMessageActionsAreRejected()
{
    auto remote = makeEvent(QStringLiteral("$remote:example.org"),
                            QStringLiteral("remote"));
    auto activity = makeEvent(QStringLiteral("$state:example.org"),
                              QStringLiteral("joined"));
    activity.type = TimelineEvent::StateChange;
    auto redacted = makeEvent(QStringLiteral("$redacted:example.org"),
                              QStringLiteral("must not copy"));
    redacted.sender = QStringLiteral("@me:example.org");
    redacted.redacted = true;
    m_client->mirror = { remote, activity, redacted };
    Q_EMIT m_client->loginSucceeded(QStringLiteral("@me:example.org"));
    Q_EMIT m_client->timelineReset(kRoom);

    QVERIFY(!m_model->canEditEvent(remote.eventId));
    QVERIFY(!m_model->canRedactEvent(remote.eventId));
    QVERIFY(m_model->messageDetails(activity.eventId).isEmpty());
    QVERIFY(m_model->visibleTextForEvent(activity.eventId).isEmpty());
    QVERIFY(m_model->visibleTextForEvent(redacted.eventId).isEmpty());
    QVERIFY(!m_model->canRedactEvent(redacted.eventId));
    QVERIFY(m_model->messageDetails(QStringLiteral("$stale")).isEmpty());
    QVERIFY(m_model->messagePermalink(QStringLiteral("$stale")).isEmpty());
}

void TimelineModelDiffTest::moderatorsMayRemoveOthersMessagesButNeverEditThem()
{
    auto remote = makeEvent(QStringLiteral("$remote:example.org"),
                            QStringLiteral("remote"));
    auto activity = makeEvent(QStringLiteral("$state:example.org"),
                              QStringLiteral("joined"));
    activity.type = TimelineEvent::StateChange;
    auto gone = makeEvent(QStringLiteral("$gone:example.org"),
                          QStringLiteral("already removed"));
    gone.redacted = true;
    m_client->mirror = { remote, activity, gone };
    Q_EMIT m_client->loginSucceeded(QStringLiteral("@me:example.org"));
    Q_EMIT m_client->timelineReset(kRoom);

    QVERIFY(!m_model->canRedactEvent(remote.eventId));
    QVERIFY(!m_model->canRedactEvent(remote.eventId, false));
    QVERIFY(m_model->canRedactEvent(remote.eventId, true));
    QVERIFY(!m_model->canRedactEvent(activity.eventId, true));
    QVERIFY(!m_model->canRedactEvent(gone.eventId, true));
    QVERIFY(!m_model->canRedactEvent(QStringLiteral("$stale"), true));
    QVERIFY(!m_model->canEditEvent(remote.eventId));
}

void TimelineModelDiffTest::anUndecryptableOwnMessageCannotBeEdited()
{
    auto own = makeEvent(QStringLiteral("$utd:example.org"),
                         QStringLiteral("[unable to decrypt yet]"));
    own.sender = QStringLiteral("@me:example.org");
    own.isEncrypted = true;
    own.undecryptable = true;
    auto readable = makeEvent(QStringLiteral("$ok:example.org"),
                              QStringLiteral("hello"));
    readable.sender = QStringLiteral("@me:example.org");
    m_client->mirror = { own, readable };
    Q_EMIT m_client->loginSucceeded(QStringLiteral("@me:example.org"));
    Q_EMIT m_client->timelineReset(kRoom);

    QVERIFY(m_model->canEditEvent(readable.eventId));
    QVERIFY(!m_model->canEditEvent(own.eventId));
    QVERIFY(m_model->canRedactEvent(own.eventId));
}

void TimelineModelDiffTest::theWaveformRoleCarriesTheRawBuckets()
{
    auto voice = makeEvent(QStringLiteral("$voice:example.org"),
                           QStringLiteral("voice.ogg"));
    voice.type = TimelineEvent::Audio;
    voice.mediaIsVoice = true;
    voice.mediaWaveform = { 0, 50, 100 };
    m_client->mirror = { voice };
    Q_EMIT m_client->timelineReset(kRoom);

    const QVariantList wf =
        m_model->data(m_model->index(0), TimelineModel::MediaWaveformRole)
            .toList();
    QCOMPARE(wf.size(), 3);
    QCOMPARE(wf.at(0).toDouble(), 0.0);
    QCOMPARE(wf.at(1).toDouble(), 50.0);
    QCOMPARE(wf.at(2).toDouble(), 100.0);
}

void TimelineModelDiffTest::aSenderNameEqualToTheMxidFallsThroughToARealName()
{
    auto early = makeEvent(QStringLiteral("$early"), QStringLiteral("H bk074 29"));
    early.sender = QStringLiteral("@bk074:localhs.test");
    early.senderDisplayName = early.sender;
    m_client->mirror = { early };
    Q_EMIT m_client->timelineReset(kRoom);

    QCOMPARE(m_model->data(m_model->index(0),
                           TimelineModel::SenderDisplayNameRole).toString(),
             QStringLiteral("bk074"));
    QCOMPARE(m_model->data(m_model->index(0),
                           TimelineModel::SenderInitialsRole).toString(),
             QStringLiteral("B"));

    auto named = makeEvent(QStringLiteral("$named"), QStringLiteral("H bk074 30"));
    named.sender = early.sender;
    named.senderDisplayName = QStringLiteral("Bee Seventy-Four");
    m_client->mirror = { early, named };
    Q_EMIT m_client->timelineReset(kRoom);
    QCOMPARE(m_model->data(m_model->index(0),
                           TimelineModel::SenderDisplayNameRole).toString(),
             QStringLiteral("Bee Seventy-Four"));

    m_client->displayNames.insert(early.sender, QStringLiteral("bk074 (member)"));
    Q_EMIT m_client->membersChanged(kRoom);
    QCOMPARE(m_model->data(m_model->index(0),
                           TimelineModel::SenderDisplayNameRole).toString(),
             QStringLiteral("bk074 (member)"));
}

void TimelineModelDiffTest::searchFindsMatchesAndNavigatesWithWrap()
{
    for (const auto &pair : { std::pair<QString, QString>{ "$e2", "hello world" },
                              std::pair<QString, QString>{ "$e3", "m3" } }) {
        auto e = makeEvent(pair.first, pair.second);
        m_client->mirror.append(e);
        Q_EMIT m_client->eventAppended(kRoom, e);
    }
    QSignalSpy spy(m_model, &TimelineModel::searchChanged);

    m_model->beginSearch(QStringLiteral("M"));
    QVERIFY(m_model->searchActive());
    QCOMPARE(m_model->searchResultCount(), 3);
    QCOMPARE(m_model->searchCurrentEventId(), QStringLiteral("$e3"));
    QCOMPARE(m_model->searchCurrentPosition(), 3);

    m_model->searchPrev();
    QCOMPARE(m_model->searchCurrentEventId(), QStringLiteral("$e1"));
    QCOMPARE(m_model->searchCurrentPosition(), 2);

    m_model->searchNext();
    QCOMPARE(m_model->searchCurrentEventId(), QStringLiteral("$e3"));
    m_model->searchNext();
    QCOMPARE(m_model->searchCurrentEventId(), QStringLiteral("$e0"));
    QCOMPARE(m_model->searchCurrentPosition(), 1);

    m_model->updateSearch(QStringLiteral("hello"));
    QCOMPARE(m_model->searchResultCount(), 1);
    QCOMPARE(m_model->searchCurrentEventId(), QStringLiteral("$e2"));
    QVERIFY(spy.count() >= 1);
}

void TimelineModelDiffTest::searchUpdatesOnPaginationInsert()
{
    m_model->beginSearch(QStringLiteral("m1"));
    QCOMPARE(m_model->searchResultCount(), 1);
    const QString selected = m_model->searchCurrentEventId();
    QCOMPARE(selected, QStringLiteral("$e1"));

    auto older = makeEvent(QStringLiteral("$old"), QStringLiteral("m1 older"));
    m_client->mirror.prepend(older);
    Q_EMIT m_client->eventsPrepended(kRoom, { older });

    QCOMPARE(m_model->searchResultCount(), 2);
    QCOMPARE(m_model->searchCurrentEventId(), selected);
}

void TimelineModelDiffTest::searchClearsOnRoomSwitchAndEnd()
{
    m_model->beginSearch(QStringLiteral("m0"));
    QVERIFY(m_model->searchActive());
    QCOMPARE(m_model->searchResultCount(), 1);

    m_model->endSearch();
    QVERIFY(!m_model->searchActive());
    QCOMPARE(m_model->searchResultCount(), 0);
    QVERIFY(m_model->searchQuery().isEmpty());
    QVERIFY(m_model->searchCurrentEventId().isEmpty());

    m_model->beginSearch(QStringLiteral("m0"));
    QVERIFY(m_model->searchActive());
    const QString other = QStringLiteral("!other:example.org");
    m_client->mirror.clear();
    m_model->setRoomId(other);
    QVERIFY(!m_model->searchActive());
    QCOMPARE(m_model->searchResultCount(), 0);
}

void TimelineModelDiffTest::searchSurvivesEditAndExcludesRedacted()
{
    m_model->beginSearch(QStringLiteral("needle"));
    QCOMPARE(m_model->searchResultCount(), 0);

    auto edited = makeEvent(QStringLiteral("$e1"),
                            QStringLiteral("has needle now"));
    edited.edited = true;
    m_client->mirror[1] = edited;
    Q_EMIT m_client->eventChangedAt(kRoom, 1, edited);
    QCOMPARE(m_model->searchResultCount(), 1);
    QCOMPARE(m_model->searchCurrentEventId(), QStringLiteral("$e1"));

    auto redacted = makeEvent(QStringLiteral("$e1"), QString());
    redacted.redacted = true;
    m_client->mirror[1] = redacted;
    Q_EMIT m_client->eventChangedAt(kRoom, 1, redacted);
    QCOMPARE(m_model->searchResultCount(), 0);
}

void TimelineModelDiffTest::threadRoleIndexTracksEveryStructuralMutation()
{
    const QString rootId = QStringLiteral("$root");
    auto root = makeEvent(rootId, QStringLiteral("topic"));
    auto reply = makeEvent(QStringLiteral("$r1"), QStringLiteral("reply one"));
    reply.threadRootId = rootId;
    auto plain = makeEvent(QStringLiteral("$p1"), QStringLiteral("unrelated"));

    m_client->mirror = { root, reply, plain };
    Q_EMIT m_client->timelineReset(kRoom);
    auto isRoot = [this](int row) {
        return m_model->data(m_model->index(row),
                             TimelineModel::IsThreadRootRole).toBool();
    };
    auto replies = [this](int row) {
        return m_model->data(m_model->index(row),
                             TimelineModel::ThreadReplyCountRole).toInt();
    };
    QVERIFY(isRoot(0));
    QCOMPARE(replies(0), 1);
    QVERIFY(!isRoot(2));
    QCOMPARE(replies(2), 0);

    auto reply2 = makeEvent(QStringLiteral("$r2"), QStringLiteral("reply two"));
    reply2.threadRootId = rootId;
    m_client->mirror.append(reply2);
    Q_EMIT m_client->eventAppended(kRoom, reply2);
    QCOMPARE(replies(0), 2);

    auto older = makeEvent(QStringLiteral("$r0"), QStringLiteral("older reply"));
    older.threadRootId = rootId;
    m_client->mirror.prepend(older);
    Q_EMIT m_client->eventsPrepended(kRoom, { older });
    QCOMPARE(replies(1), 3);
    QVERIFY(isRoot(1));

    m_client->mirror.removeAt(0);
    Q_EMIT m_client->eventRemovedAt(kRoom, 0);
    QCOMPARE(replies(0), 2);

    const int plainRow = 2;
    QCOMPARE(m_model->data(m_model->index(plainRow),
                           TimelineModel::EventIdRole).toString(),
             QStringLiteral("$p1"));
    auto revealed = plain;
    revealed.threadRootId = rootId;
    m_client->mirror[plainRow] = revealed;
    Q_EMIT m_client->eventChangedAt(kRoom, plainRow, revealed);
    QCOMPARE(replies(0), 3);
}

void TimelineModelDiffTest::messageSegmentsSplitCodeBlocksAndAreComputedOnce()
{
    TimelineEvent e = makeEvent(QStringLiteral("$code"),
                                QStringLiteral("before\nfn main() {}\nafter"));
    e.formattedBody = QStringLiteral(
        "<p>before <a href=\"https://matrix.to/#/@bob:example.org\">Bob</a></p>"
        "<pre><code class=\"language-rust\">fn main() {\n"
        "    println!(\"&lt;hi&gt;\");\n"
        "}</code></pre>"
        "<p>after</p>");
    m_client->mirror = { e };
    m_client->displayNames.insert(QStringLiteral("@bob:example.org"),
                                  QStringLiteral("Bob B"));
    Q_EMIT m_client->timelineReset(kRoom);
    QCOMPARE(m_model->rowCount(), 1);

    const QModelIndex idx = m_model->index(0);
    m_client->displayNameLookups = 0;
    const QVariantList segments =
        m_model->data(idx, TimelineModel::MessageSegmentsRole).toList();
    const int firstReadLookups = m_client->displayNameLookups;
    QVERIFY(firstReadLookups > 0);

    QCOMPARE(segments.size(), 3);
    QCOMPARE(segments.at(0).toMap().value(QStringLiteral("kind")).toInt(), 0);
    QCOMPARE(segments.at(1).toMap().value(QStringLiteral("kind")).toInt(), 1);
    QCOMPARE(segments.at(2).toMap().value(QStringLiteral("kind")).toInt(), 0);

    const QVariantMap code = segments.at(1).toMap();
    QCOMPARE(code.value(QStringLiteral("language")).toString(),
             QStringLiteral("rust"));
    QCOMPARE(code.value(QStringLiteral("text")).toString(),
             QStringLiteral("fn main() {\n    println!(\"<hi>\");\n}"));

    QVERIFY(segments.at(0).toMap().value(QStringLiteral("text")).toString()
                .contains(QStringLiteral("Bob B")));
    QVERIFY(segments.at(2).toMap().value(QStringLiteral("text")).toString()
                .contains(QStringLiteral("after")));

    m_client->displayNameLookups = 0;
    const QVariantList again =
        m_model->data(idx, TimelineModel::MessageSegmentsRole).toList();
    QCOMPARE(m_client->displayNameLookups, 0);
    QCOMPARE(again, segments);

    m_client->mirror[0].formattedBody =
        QStringLiteral("<pre><code>edited</code></pre>");
    Q_EMIT m_client->eventEdited(kRoom, e.eventId);
    const QVariantList edited =
        m_model->data(idx, TimelineModel::MessageSegmentsRole).toList();
    QCOMPARE(edited.size(), 1);
    QCOMPARE(edited.at(0).toMap().value(QStringLiteral("text")).toString(),
             QStringLiteral("edited"));
}

void TimelineModelDiffTest::messageSegmentsStayEmptyAndFreeForOrdinaryBodies()
{
    TimelineEvent plain = makeEvent(QStringLiteral("$plain"),
                                    QStringLiteral("hello Bob"));
    TimelineEvent rich = makeEvent(QStringLiteral("$rich"),
                                   QStringLiteral("hello Bob"));
    rich.formattedBody = QStringLiteral(
        "<em>hello</em> <a href=\"https://matrix.to/#/@bob:example.org\">Bob</a>"
        " and <code>inline</code>");
    TimelineEvent quoted = makeEvent(QStringLiteral("$quoted"),
                                     QStringLiteral("reply"));
    quoted.formattedBody =
        QStringLiteral("<mx-reply><pre>quoted</pre></mx-reply>reply");
    TimelineEvent gone = makeEvent(QStringLiteral("$gone"),
                                   QStringLiteral("removed"));
    gone.formattedBody = QStringLiteral("<pre><code>secret</code></pre>");
    gone.redacted = true;

    m_client->mirror = { plain, rich, quoted, gone };
    m_client->displayNames.insert(QStringLiteral("@bob:example.org"),
                                  QStringLiteral("Bob B"));
    Q_EMIT m_client->timelineReset(kRoom);
    QCOMPARE(m_model->rowCount(), 4);

    for (int row = 0; row < 4; ++row) {
        QVERIFY2(m_model->data(m_model->index(row),
                               TimelineModel::MessageSegmentsRole)
                     .toList().isEmpty(),
                 qPrintable(QStringLiteral("row %1 must keep the ordinary "
                                           "single-TextEdit path").arg(row)));
    }

    m_client->displayNameLookups = 0;
    m_model->data(m_model->index(1), TimelineModel::MessageSegmentsRole);
    QCOMPARE(m_client->displayNameLookups, 0);
    QVERIFY(m_model->data(m_model->index(1), TimelineModel::FormattedBodyRole)
                .toString().contains(QStringLiteral("Bob B")));
}

void TimelineModelDiffTest::reactionRolesNameTheReactorsAndKeepTheUncappedTotal()
{
    TimelineEvent e = makeEvent(QStringLiteral("$m0"), QStringLiteral("hi"));
    Reaction thumbs;
    thumbs.key = QString::fromUtf8("\U0001F44D");
    thumbs.count = 7;
    thumbs.byMe = true;
    thumbs.myEventId = QStringLiteral("$react0");
    thumbs.senders = { QStringLiteral("@me:example.org"),
                       QStringLiteral("@bob:example.org"),
                       QStringLiteral("@carol:example.org") };
    e.reactions = { thumbs };
    m_client->mirror = { e };
    m_client->displayNames.insert(QStringLiteral("@bob:example.org"),
                                  QStringLiteral("Bob B"));
    Q_EMIT m_client->timelineReset(kRoom);
    QCOMPARE(m_model->rowCount(), 1);

    auto bucketAt = [this](int row) {
        const QVariantList buckets =
            m_model->data(m_model->index(row), TimelineModel::ReactionsRole)
                .toList();
        return buckets.isEmpty() ? QVariantMap{} : buckets.at(0).toMap();
    };

    const QVariantMap bucket = bucketAt(0);
    QVERIFY(!bucket.isEmpty());
    QCOMPARE(bucket.value(QStringLiteral("key")).toString(), thumbs.key);
    QCOMPARE(bucket.value(QStringLiteral("count")).toInt(), 7);
    QCOMPARE(bucket.value(QStringLiteral("byMe")).toBool(), true);
    QCOMPARE(bucket.value(QStringLiteral("reactorTotal")).toInt(), 7);

    const QStringList names =
        bucket.value(QStringLiteral("reactorNames")).toStringList();
    QCOMPARE(names, QStringList({ QStringLiteral("me"),
                                  QStringLiteral("Bob B"),
                                  QStringLiteral("carol") }));
    for (const QString &name : names) {
        QVERIFY(!name.startsWith(QLatin1Char('@')));
        QVERIFY(!name.contains(QLatin1Char(':')));
    }

    QSignalSpy dataSpy(m_model, &QAbstractItemModel::dataChanged);
    m_client->displayNames.insert(QStringLiteral("@carol:example.org"),
                                  QStringLiteral("Carol C"));
    Q_EMIT m_client->membersChanged(kRoom);
    QVERIFY(!dataSpy.isEmpty());
    QVERIFY(dataSpy.first().at(2).value<QList<int>>().contains(
        TimelineModel::ReactionsRole));
    QCOMPARE(bucketAt(0).value(QStringLiteral("reactorNames")).toStringList(),
             QStringList({ QStringLiteral("me"), QStringLiteral("Bob B"),
                           QStringLiteral("Carol C") }));

    // Backends without reactor identities (mock/HTTP) carry an empty list,
    // never a fabricated name or guessed count.
    TimelineEvent bare = makeEvent(QStringLiteral("$m1"), QStringLiteral("yo"));
    Reaction anonymous;
    anonymous.key = QString::fromUtf8("\U0001F600");
    anonymous.count = 2;
    bare.reactions = { anonymous };
    m_client->mirror.append(bare);
    Q_EMIT m_client->eventAppended(kRoom, bare);
    const QVariantMap bareBucket = bucketAt(1);
    QVERIFY(!bareBucket.isEmpty());
    QVERIFY(bareBucket.value(QStringLiteral("reactorNames")).toStringList()
                .isEmpty());
    QCOMPARE(bareBucket.value(QStringLiteral("reactorTotal")).toInt(), 2);
}

void TimelineModelDiffTest::receiptsOnBodilessRowsLandOnTheRowAbove()
{
    const auto readers = [&](int row) {
        QStringList ids;
        const auto list = m_model->data(m_model->index(row),
                                        TimelineModel::ReadReceiptsRole).toList();
        for (const auto &v : list)
            ids << v.toMap().value(QStringLiteral("userId")).toString();
        return ids;
    };
    const auto total = [&](int row) {
        return m_model->data(m_model->index(row),
                             TimelineModel::ReadReceiptsTotalRole).toInt();
    };

    TimelineEvent msg = makeEvent(QStringLiteral("$m"), QStringLiteral("hello"));
    msg.readBy = { { QStringLiteral("@bob:example.org"), 1000 } };
    msg.readByTotal = 1;
    m_client->mirror.append(msg);
    Q_EMIT m_client->eventAppended(kRoom, msg);
    const int msgRow = m_model->rowCount() - 1;
    QCOMPARE(readers(msgRow), QStringList{ QStringLiteral("@bob:example.org") });

    TimelineEvent rtc = makeEvent(QStringLiteral("$rtc"), QString());
    rtc.type = TimelineEvent::StateChange;
    rtc.stateKind = QStringLiteral("org.matrix.msc3401.call.member");
    rtc.readBy = { { QStringLiteral("@carol:example.org"), 2000 } };
    rtc.readByTotal = 1;
    m_client->mirror.append(rtc);
    Q_EMIT m_client->eventAppended(kRoom, rtc);
    QCOMPARE(m_model->rowCount(), msgRow + 2);

    QVERIFY(readers(msgRow + 1).isEmpty());
    QCOMPARE(total(msgRow + 1), 0);
    QCOMPARE(readers(msgRow), (QStringList{ QStringLiteral("@carol:example.org"),
                                           QStringLiteral("@bob:example.org") }));
    QCOMPARE(total(msgRow), 2);

    TimelineEvent rtc2 = rtc;
    rtc2.readBy = { { QStringLiteral("@carol:example.org"), 2000 },
                    { QStringLiteral("@bob:example.org"), 3000 } };
    rtc2.readByTotal = 2;
    QSignalSpy changed(m_model, &TimelineModel::dataChanged);
    m_client->mirror[msgRow + 1] = rtc2;
    Q_EMIT m_client->eventChangedAt(kRoom, msgRow + 1, rtc2);
    bool hostAnnounced = false;
    for (const auto &args : changed) {
        if (args.at(0).toModelIndex().row() == msgRow)
            hostAnnounced = true;
    }
    QVERIFY2(hostAnnounced, "the row that shows the chips was not re-announced");
    QCOMPARE(readers(msgRow), (QStringList{ QStringLiteral("@bob:example.org"),
                                           QStringLiteral("@carol:example.org") }));
    QCOMPARE(total(msgRow), 2);

    TimelineEvent call = makeEvent(QStringLiteral("$call"), QString());
    call.type = TimelineEvent::CallEvent;
    call.readBy = { { QStringLiteral("@erin:example.org"), 4000 } };
    call.readByTotal = 1;
    m_client->mirror.append(call);
    Q_EMIT m_client->eventAppended(kRoom, call);
    const int callRow = m_model->rowCount() - 1;
    QCOMPARE(readers(callRow), QStringList{ QStringLiteral("@erin:example.org") });
    QCOMPARE(readers(msgRow), (QStringList{ QStringLiteral("@bob:example.org"),
                                           QStringLiteral("@carol:example.org") }));
}

QString TimelineModelDiffTest::openThreadModel()
{
    const QString threadId = MatrixClient::threadTimelineId(
        kRoom, QStringLiteral("$root"));
    m_client->mirrorAlias = threadId;
    for (auto &event : m_client->mirror)
        event.roomId = threadId;
    m_model->setRoomId(threadId);
    return threadId;
}

void TimelineModelDiffTest::threadModelResolvesEveryIdentityAgainstTheRealRoom()
{
    const QString alice = QStringLiteral("@alice:example.org");
    const QString bob = QStringLiteral("@bob:example.org");
    m_client->roomDisplayNames[kRoom] = { { alice, QStringLiteral("Alice A") },
                                          { bob, QStringLiteral("Bob B") } };
    m_client->roomAvatarMxc[kRoom] = { { alice, QStringLiteral("mxc://s/av") } };

    TimelineEvent reply = makeEvent(QStringLiteral("$r1"), QStringLiteral("hi"));
    reply.sender = alice;
    reply.senderDisplayName.clear();
    reply.replyToSenderId = bob;
    reply.readBy = { { bob, Q_INT64_C(1700000004000) } };
    reply.readByTotal = 1;
    Reaction reaction;
    reaction.key = QString::fromUtf8("\U0001F44D");
    reaction.count = 1;
    reaction.senders = { bob };
    reply.reactions = { reaction };
    m_client->mirror.append(reply);

    const QString threadId = openThreadModel();
    QCOMPARE(m_model->roomId(), threadId);
    const int row = m_model->rowCount() - 1;
    const QModelIndex idx = m_model->index(row);

    QCOMPARE(m_model->data(idx, TimelineModel::SenderDisplayNameRole).toString(),
             QStringLiteral("Alice A"));
    QCOMPARE(m_model->data(idx, TimelineModel::SenderAvatarMxcRole).toString(),
             QStringLiteral("mxc://s/av"));
    QCOMPARE(m_model->data(idx, TimelineModel::ReplyToSenderRole).toString(),
             QStringLiteral("Bob B"));

    const QVariantList receipts =
        m_model->data(idx, TimelineModel::ReadReceiptsRole).toList();
    QCOMPARE(receipts.size(), 1);
    QCOMPARE(receipts.first().toMap()
                 .value(QStringLiteral("displayName")).toString(),
             QStringLiteral("Bob B"));

    const QVariantList reactions =
        m_model->data(idx, TimelineModel::ReactionsRole).toList();
    QCOMPARE(reactions.size(), 1);
    QCOMPARE(reactions.first().toMap()
                 .value(QStringLiteral("reactorNames")).toStringList(),
             QStringList{ QStringLiteral("Bob B") });

    QVERIFY2(!m_client->displayNameRoomsAsked.contains(threadId),
             "a member lookup was made with the composite timeline id");
    QVERIFY(m_client->displayNameRoomsAsked.contains(kRoom));
}

void TimelineModelDiffTest::memberHydrationReachesAThreadModel()
{
    const QString alice = QStringLiteral("@alice:example.org");
    TimelineEvent reply = makeEvent(QStringLiteral("$r1"), QStringLiteral("hi"));
    reply.sender = alice;
    reply.senderDisplayName.clear();
    m_client->mirror.append(reply);
    openThreadModel();
    const int row = m_model->rowCount() - 1;
    const QModelIndex idx = m_model->index(row);
    QCOMPARE(m_model->data(idx, TimelineModel::SenderDisplayNameRole).toString(),
             QStringLiteral("alice"));

    m_client->roomDisplayNames[kRoom] = { { alice, QStringLiteral("Alice A") } };
    QSignalSpy changed(m_model, &TimelineModel::dataChanged);
    Q_EMIT m_client->membersChanged(kRoom);
    QVERIFY2(!changed.isEmpty(),
             "member hydration never reached the thread model");
    QCOMPARE(m_model->data(idx, TimelineModel::SenderDisplayNameRole).toString(),
             QStringLiteral("Alice A"));

    QSignalSpy other(m_model, &TimelineModel::dataChanged);
    Q_EMIT m_client->membersChanged(QStringLiteral("!elsewhere:example.org"));
    QVERIFY(other.isEmpty());
}

void TimelineModelDiffTest::anOrdinaryRoomModelStillAsksWithItsOwnRoomId()
{
    const QString alice = QStringLiteral("@alice:example.org");
    m_client->roomDisplayNames[kRoom] = { { alice, QStringLiteral("Alice A") } };
    TimelineEvent m = makeEvent(QStringLiteral("$r1"), QStringLiteral("hi"));
    m.sender = alice;
    m.senderDisplayName.clear();
    m_client->mirror.append(m);
    Q_EMIT m_client->eventAppended(kRoom, m);
    const QModelIndex idx = m_model->index(m_model->rowCount() - 1);
    QCOMPARE(m_model->data(idx, TimelineModel::SenderDisplayNameRole).toString(),
             QStringLiteral("Alice A"));
    for (const QString &asked : m_client->displayNameRoomsAsked)
        QCOMPARE(asked, kRoom);

    QSignalSpy changed(m_model, &TimelineModel::dataChanged);
    Q_EMIT m_client->membersChanged(kRoom);
    QVERIFY(!changed.isEmpty());
}

void TimelineModelDiffTest::markVisibleAsReadNeverSendsARoomReceiptForAThread()
{
    openThreadModel();
    m_model->markVisibleAsRead(0, m_model->rowCount() - 1);
    QCOMPARE(m_client->readReceipts.size(), 0);

    m_client->readReceipts.clear();
    m_client->mirrorAlias.clear();
    for (auto &event : m_client->mirror)
        event.roomId = kRoom;
    m_model->setRoomId(kRoom);
    m_model->markVisibleAsRead(0, m_model->rowCount() - 1);
    QCOMPARE(m_client->readReceipts.size(), 1);
    QCOMPARE(m_client->readReceipts.first().first, kRoom);
}

QTEST_GUILESS_MAIN(TimelineModelDiffTest)
#include "TimelineModelDiffTest.moc"
