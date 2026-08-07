// v0.5.7: TimelineModel diff application.
//
// Drives the model through a scripted fake MatrixClient: append, prepend,
// insert, in-place replace, remove, truncate, reset, local-echo
// reconciliation, undecryptable → decrypted replacement, and the
// self-heal path for invalid indices. No network, no Rust.

#include "matrix/MatrixClient.h"
#include "models/TimelineModel.h"

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

// Minimal scripted backend: the timeline() list is the mirror the model
// reloads from; tests mutate it and emit the interface signals exactly
// like RustSdkMatrixClient does after applying a Rust diff.
class FakeClient : public MatrixClient
{
    Q_OBJECT
public:
    explicit FakeClient(QObject *parent = nullptr) : MatrixClient(parent) {}

    QList<TimelineEvent> mirror;
    QStringList retriedTransactions;
    QStringList typingUsers;
    QHash<QString, QString> displayNames;
    QHash<QString, QString> avatarMxc;

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
        return roomId == kRoom ? mirror : QList<TimelineEvent>{};
    }
    QString displayNameFor(const QString &, const QString &userId) const override
    {
        return displayNames.value(userId, userId);
    }
    QString avatarMxcFor(const QString &, const QString &userId) const override
    {
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
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }
    void retryFailedSend(const QString &, const QString &txn) override
    {
        retriedTransactions.append(txn);
    }
};

} // namespace

class TimelineModelDiffTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void cleanup();

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
    void localEchoReconciliation();
    void retrySendRoutesTransaction();
    void retrySendIgnoresNonFailedRows();
    void loggedOutClearsModel();
    void stableRoleData();
    void typingTextFormatsByCount();
    void senderIdentityRolesUseSdkProfileAndSafeAvatarMxc();
    void senderGroupingBoundariesAreDeterministic();
    void readMarkerDoesNotBreakSenderGroupButVisibleRowsDo();
    void mediaAndPaginationPreserveSenderGrouping();
    void groupingRefreshCoalescesAcrossPrependBurst();
    void memberProfileUpdateEmitsIdentityRoles();
    void replyToSenderResolvesDisplayNameWithLocalpartFallback();
    void threadRoleIndexTracksEveryStructuralMutation();
    void clientSwitchDoesNotLeakSenderProfile();
    void messageActionsUseStableIdentityAndSafeMetadata();
    void staleAndInapplicableMessageActionsAreRejected();
    // v0.7: MSC3381 poll roles and the conservative end-poll rule.
    void pollRolesExposeOutcomeAndEndPermission();
    // Read-receipt chips: identity resolution, own-receipt exclusion,
    // newest-first order, live Set-diff/member-hydration updates, and
    // ReadMarker-row neutrality.
    void readReceiptsRoleResolvesExcludesSelfAndSortsNewestFirst();
    void readReceiptsUpdateViaSetDiffAndMemberHydration();
    void receiptOnlySetKeepsThreadIndexWithoutRebuild();
    void readMarkerRowsStayReceiptFreeWithoutIndexDrift();
    // v0.6.1 loaded-timeline search.
    void searchFindsMatchesAndNavigatesWithWrap();
    void searchUpdatesOnPaginationInsert();
    void searchClearsOnRoomSwitchAndEnd();
    void searchSurvivesEditAndExcludesRedacted();

    // v0.7.x: near-top backfill "virtual scrolling" staging window
    // (TimelineModel::backfillStagingActive / setBackfillStagingActive).
    void stagingHoldsPrependedRowsOutOfExposedSpaceUntilFlush();
    void stagingFlushCoalescesMultipleBatchesIntoOneInsert();
    void stagingNeverHoldsBackAnAppendAtTheBottom();
    void stagingAppliesInPlaceEditsToHiddenRowsSilently();
    void stagingHeldPrefixIsDiscardedByARoomReset();
    void stagingCapBoundsTheHeldPrefixAndAutoFlushes();
    // Independent review H1: the exposed row space must be self-consistent
    // on its own while staged — grouping must never resolve into the
    // hidden prefix.
    void stagingKeepsSenderGroupingSelfConsistentOnExposedBoundary();
    void stagingKeepsStateGroupLeaderSelfConsistentOnExposedBoundary();
    // M4: boundary coverage the reviewer specified.
    void stagingRemovalInsideThePrefixShrinksItSilently();
    void stagingTruncationCuttingIntoThePrefixFlushesFirst();
    void stagingTruncationExactlyAtTheBoundaryStaysOrdinary();
    void stagingInsertExactlyAtTheBoundaryIsExposedNotStaged();
    void stagingMediaEntryRowRoundTripsThroughStableIdAt();
    // L8: search must not report a match the reader cannot navigate to yet.
    void stagingExcludesHeldRowsFromSearchUntilFlush();

private:
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

void TimelineModelDiffTest::cleanup()
{
    delete m_model;
    delete m_client;
    m_model = nullptr;
    m_client = nullptr;
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
    QCOMPARE(m_model->rowCount(), 2); // in place, no growth
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
    QCOMPARE(m_model->rowCount(), 2); // untouched
}

void TimelineModelDiffTest::invalidIndexSelfHeals()
{
    // A corrupt index must never crash or corrupt — the model falls back
    // to reloading the full backend list.
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

    // Key import happened → the SDK replaces the row in place.
    TimelineEvent decrypted = makeEvent(QStringLiteral("$enc"),
                                        QStringLiteral("secret hello"));
    decrypted.itemId = utd.itemId;
    decrypted.isEncrypted = true;
    decrypted.isDecrypted = true;
    m_client->mirror[2] = decrypted;
    Q_EMIT m_client->eventChangedAt(kRoom, 2, decrypted);

    QCOMPARE(m_model->rowCount(), 3); // no duplicate row
    QVERIFY(!m_model->data(m_model->index(2), TimelineModel::UndecryptableRole)
                 .toBool());
    QVERIFY(m_model->data(m_model->index(2), TimelineModel::IsDecryptedRole)
                .toBool());
    QCOMPARE(m_model->data(m_model->index(2), TimelineModel::BodyRole)
                 .toString(),
             QStringLiteral("secret hello"));
    // Stable identity survives the replacement.
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

    // Remote echo arrives: same row updates in place, no duplicate.
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

    // No duplicate event id anywhere in the model.
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
    m_model->retrySend(0);   // Sent row
    m_model->retrySend(99);  // out of range
    m_model->retrySend(-1);  // out of range
    QVERIFY(m_client->retriedTransactions.isEmpty());
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
    // Existing role names are preserved for QML compatibility.
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

    // Alice's poll is not ours: no End poll offer.
    QVERIFY(!m_model->data(idx, TimelineModel::CanEndPollRole).toBool());

    // Our own running poll can be ended…
    TimelineEvent own = poll;
    own.eventId = QStringLiteral("$own-poll");
    own.itemId = QStringLiteral("uid-own-poll");
    own.sender = QStringLiteral("@me:example.org");
    m_client->mirror.append(own);
    Q_EMIT m_client->eventAppended(kRoom, own);
    const auto ownIdx = m_model->index(3);
    QVERIFY(m_model->data(ownIdx, TimelineModel::CanEndPollRole).toBool());

    // …but not once it has ended (in-place Set replacement).
    TimelineEvent ended = own;
    ended.pollEnded = true;
    m_client->mirror[3] = ended;
    Q_EMIT m_client->eventChangedAt(kRoom, 3, ended);
    QVERIFY(m_model->data(ownIdx, TimelineModel::PollEndedRole).toBool());
    QVERIFY(!m_model->data(ownIdx, TimelineModel::CanEndPollRole).toBool());
    // Role-name contract for QML.
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

    // Sender is @alice (makeEvent). Own receipt first (excluded), then the
    // SENDER's implicit receipt (excluded — the SDK synthesizes one for
    // every event's author, which must never render a permanent "read by
    // its author" chip), then two genuine other readers with Carol's
    // receipt OLDER than the unknown member's.
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
    // Neither the local user nor the sender is ever shown.
    QCOMPARE(receipts.size(), 2);
    // Newest reader first.
    const QVariantMap newest = receipts.at(0).toMap();
    QCOMPARE(newest.value(QStringLiteral("userId")).toString(),
             QStringLiteral("@unknown:example.org"));
    QCOMPARE(newest.value(QStringLiteral("tsMs")).toLongLong(),
             Q_INT64_C(1700000003000));
    // Unresolved member: LOCALPART fallback, never the bare MXID.
    QCOMPARE(newest.value(QStringLiteral("displayName")).toString(),
             QStringLiteral("unknown"));
    QVERIFY(newest.value(QStringLiteral("avatarMxc")).toString().isEmpty());
    const QVariantMap carol = receipts.at(1).toMap();
    QCOMPARE(carol.value(QStringLiteral("displayName")).toString(),
             QStringLiteral("Carol"));
    QCOMPARE(carol.value(QStringLiteral("avatarMxc")).toString(),
             QStringLiteral("mxc://example.org/carol"));

    // Companion total: no explicit readByTotal → the delivered list (4)
    // minus the two exclusions found in it.
    QCOMPARE(m_model->data(idx, TimelineModel::ReadReceiptsTotalRole).toInt(),
             2);

    // A DIFFERENT user's receipt on the sender's own message still shows:
    // exclusion keys on the receipt's user, never on the row having an
    // author.
    TimelineEvent bySender = makeEvent(QStringLiteral("$by-sender"),
                                       QStringLiteral("alice's message"));
    bySender.readBy = {
        { QStringLiteral("@carol:example.org"), Q_INT64_C(1700000006000) },
    };
    // Capped-window shape: the server knows 30 readers, the FFI window
    // delivered 1 → "+N" math uses the uncapped total minus in-window
    // exclusions (none here).
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

    // Rows without receipts answer an empty list and a zero total (thread
    // timelines, local echoes, unread messages).
    QVERIFY(m_model->data(m_model->index(0), TimelineModel::ReadReceiptsRole)
                .toList()
                .isEmpty());
    QCOMPARE(m_model->data(m_model->index(0),
                           TimelineModel::ReadReceiptsTotalRole).toInt(),
             0);
    // Role-name contract for QML.
    QCOMPARE(m_model->roleNames().value(TimelineModel::ReadReceiptsRole),
             QByteArrayLiteral("readReceipts"));
    QCOMPARE(m_model->roleNames().value(TimelineModel::ReadReceiptsTotalRole),
             QByteArrayLiteral("readReceiptsTotal"));
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

    // The Set diff replaced the row in place and re-announced every role
    // (empty role vector), so the receipt strip re-reads too.
    QCOMPARE(m_model->rowCount(), 2);
    QCOMPARE(spy.count(), 1);
    QVERIFY(spy.at(0).at(2).value<QVector<int>>().isEmpty());
    QVariantList receipts =
        m_model->data(m_model->index(1), TimelineModel::ReadReceiptsRole)
            .toList();
    QCOMPARE(receipts.size(), 1);
    // Unhydrated member: localpart fallback for now.
    QCOMPARE(receipts.first().toMap()
                 .value(QStringLiteral("displayName")).toString(),
             QStringLiteral("bob"));

    // Member hydration announces ReadReceiptsRole so chips leave the
    // localpart fallback exactly like every other member-derived label.
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

    // A later Set diff that clears the receipts (Bob read a newer message)
    // empties the strip in place.
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
    // onEventChangedAt rebuilds the O(n) thread-reply index ONLY when the
    // row's threadRootId changed. Receipts multiply Set frequency (every
    // receipt move is a Set), so a receipts-only Set must leave the index
    // untouched-but-correct, while a Set that genuinely rewires the thread
    // relation still rebuilds it. There is no rebuild counter; this pins
    // the observable contract on both sides of the guard.
    TimelineEvent reply = makeEvent(QStringLiteral("$reply"),
                                    QStringLiteral("in thread"));
    reply.threadRootId = QStringLiteral("$e0");
    m_client->mirror.append(reply);
    Q_EMIT m_client->eventAppended(kRoom, reply);
    const auto rootIdx = m_model->index(0);
    QVERIFY(m_model->data(rootIdx, TimelineModel::IsThreadRootRole).toBool());
    QCOMPARE(m_model->data(rootIdx,
                           TimelineModel::ThreadReplyCountRole).toInt(), 1);

    // Receipts-only Set on the reply: threadRootId unchanged → no rebuild
    // needed, and the thread roles keep answering identically.
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

    // A Set that clears the thread relation must still rebuild: the root
    // stops being a root.
    TimelineEvent detached = withReceipt;
    detached.threadRootId.clear();
    m_client->mirror[2] = detached;
    Q_EMIT m_client->eventChangedAt(kRoom, 2, detached);
    QVERIFY(!m_model->data(rootIdx, TimelineModel::IsThreadRootRole).toBool());
    QCOMPARE(m_model->data(rootIdx,
                           TimelineModel::ThreadReplyCountRole).toInt(), 0);
}

void TimelineModelDiffTest::readMarkerRowsStayReceiptFreeWithoutIndexDrift()
{
    // Enabling SDK receipt tracking also produces ReadMarker virtual rows.
    // They stay ordinary virtual rows: no receipts of their own, no index
    // drift for the event rows around them, grouping stays transparent.
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
    // The rows around the marker kept their identity and data.
    QCOMPARE(m_model->data(m_model->index(2), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$e1"));
    QCOMPARE(m_model->data(m_model->index(3), TimelineModel::ReadReceiptsRole)
                 .toList()
                 .size(),
             1);
    // Grouping remains transparent through the marker: the $e1 row still
    // continues Alice's group started at $e0.
    QVERIFY(m_model->data(m_model->index(2),
                          TimelineModel::ContinuesSenderGroupRole).toBool());
}

void TimelineModelDiffTest::typingTextFormatsByCount()
{
    // Phase 7 exact phrasing: the current user is always excluded, and the
    // wording depends on how many *other* users are typing.
    m_client->displayNames.insert(QStringLiteral("@a:example.org"),
                                  QStringLiteral("Alice"));
    m_client->displayNames.insert(QStringLiteral("@b:example.org"),
                                  QStringLiteral("Bob"));

    // Zero typers.
    m_client->typingUsers = {};
    Q_EMIT m_client->typingChanged(kRoom);
    QCOMPARE(m_model->typingText(), QString{});

    // The current user typing is filtered out → still empty.
    m_client->typingUsers = { QStringLiteral("@me:example.org") };
    Q_EMIT m_client->typingChanged(kRoom);
    QCOMPARE(m_model->typingText(), QString{});

    // One other user.
    m_client->typingUsers = { QStringLiteral("@a:example.org") };
    Q_EMIT m_client->typingChanged(kRoom);
    QCOMPARE(m_model->typingText(), QStringLiteral("Alice is typing…"));

    // Two other users (self filtered even when present in the set).
    m_client->typingUsers = { QStringLiteral("@a:example.org"),
                              QStringLiteral("@b:example.org") };
    Q_EMIT m_client->typingChanged(kRoom);
    QCOMPARE(m_model->typingText(),
             QStringLiteral("Alice and Bob are typing…"));

    // Three or more collapses to a count.
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
    // A backward-pagination page is delivered by the SDK as many single-item
    // push_front diffs (PAGINATION_BATCH=20), each arriving as its own
    // eventsPrepended inside ONE poll-drain turn (drain cap 64 >= 20). The
    // whole-model grouping dataChanged that keeps sender/state grouping
    // correct must fire ONCE for the whole burst, not once per event —
    // otherwise every page triggers N full-model relayouts and scrolling
    // jitters while older history loads.
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

    // 12 separate single-item prepends, back to back (one synchronous drain),
    // exactly like RustSdkMatrixClient::handleTimelineDiff emitting per diff.
    const int kPage = 12;
    for (int i = kPage - 1; i >= 0; --i) {
        auto older = makeEvent(QStringLiteral("$old%1").arg(i),
                               QStringLiteral("o%1").arg(i));
        older.timestamp = base.addSecs(-60 * (kPage - i));
        m_client->mirror.prepend(older);
        Q_EMIT m_client->eventsPrepended(kRoom, QList<TimelineEvent>{ older });
    }
    QCOMPARE(m_model->rowCount(), 2 + kPage);

    // Synchronously the rows are all in, but NOT ONE whole-model grouping
    // dataChanged has fired — it is coalesced onto the next event-loop turn.
    QCOMPARE(countGrouping(), 0);
    // Grouping still reads correctly from data() (computed live, uncached).
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$old0"));

    // One turn later: exactly ONE coalesced whole-model grouping refresh
    // spanning every row, however many prepend diffs arrived.
    QTRY_COMPARE(countGrouping(), 1);
    QModelIndex tl, br;
    for (const auto &sig : changed) {
        if (isGroupingChange(sig)) {
            tl = sig.at(0).toModelIndex();
            br = sig.at(1).toModelIndex();
        }
    }
    QCOMPARE(tl.row(), 0);
    QCOMPARE(br.row(), m_model->rowCount() - 1);

    // The coalescer re-arms: a later mutation still refreshes grouping once.
    changed.clear();
    auto live = makeEvent(QStringLiteral("$live"), QStringLiteral("live"));
    live.timestamp = base.addSecs(120);
    m_client->mirror.append(live);
    Q_EMIT m_client->eventAppended(kRoom, live);
    QCOMPARE(countGrouping(), 0);
    QTRY_COMPARE(countGrouping(), 1);
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
    // Mention chips (FormattedBodyRole) and reply headers
    // (ReplyToSenderRole) resolve display names through the same member
    // lookup — a hydration burst must refresh them too, or a row rendered
    // pre-hydration keeps its localpart fallback forever.
    QVERIFY(roles.contains(TimelineModel::FormattedBodyRole));
    QVERIFY(roles.contains(TimelineModel::ReplyToSenderRole));
    QVERIFY(roles.contains(TimelineModel::ThreadLatestSenderDisplayNameRole));
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::SenderInitialsRole).toString(),
             QStringLiteral("AU"));
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::SenderAvatarMxcRole).toString(),
             QStringLiteral("mxc://example.org/new-avatar"));
}

void TimelineModelDiffTest::replyToSenderResolvesDisplayNameWithLocalpartFallback()
{
    auto reply = makeEvent(QStringLiteral("$reply"), QStringLiteral("hi"));
    reply.replyToEventId = QStringLiteral("$orig");
    reply.replyToSender = QStringLiteral("@maya:example.org");
    m_client->mirror = { reply };
    Q_EMIT m_client->timelineReset(kRoom);

    // Unknown member: the visible label is the LOCALPART (a readable
    // fallback), never the full MXID.
    QCOMPARE(m_model->data(m_model->index(0),
                           TimelineModel::ReplyToSenderRole).toString(),
             QStringLiteral("maya"));

    // Member hydrates: the reply header resolves to the display name, and
    // membersChanged is what refreshes the already-rendered row.
    m_client->displayNames.insert(QStringLiteral("@maya:example.org"),
                                  QStringLiteral("Maya Chen"));
    Q_EMIT m_client->membersChanged(kRoom);
    QCOMPARE(m_model->data(m_model->index(0),
                           TimelineModel::ReplyToSenderRole).toString(),
             QStringLiteral("Maya Chen"));

    // Thread summary cards resolve through the same three tiers: with no
    // embedded SDK name, the member lookup wins over the localpart.
    auto root = makeEvent(QStringLiteral("$root"), QStringLiteral("topic"));
    root.isThreadRoot = true;
    root.threadLatestSender = QStringLiteral("@maya:example.org");
    m_client->mirror = { root };
    Q_EMIT m_client->timelineReset(kRoom);
    QCOMPARE(m_model->data(m_model->index(0),
                           TimelineModel::ThreadLatestSenderDisplayNameRole)
                 .toString(),
             QStringLiteral("Maya Chen"));
    // And the embedded SDK name still has first claim when present.
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

// beginSearch matches loaded, visible message text (case-insensitive),
// starts at the newest match, and next/prev walk with wrap-around.
void TimelineModelDiffTest::searchFindsMatchesAndNavigatesWithWrap()
{
    // init() gives "m0","m1"; add two more so "m" matches four and "hello"
    // matches one.
    for (const auto &pair : { std::pair<QString, QString>{ "$e2", "hello world" },
                              std::pair<QString, QString>{ "$e3", "m3" } }) {
        auto e = makeEvent(pair.first, pair.second);
        m_client->mirror.append(e);
        Q_EMIT m_client->eventAppended(kRoom, e);
    }
    QSignalSpy spy(m_model, &TimelineModel::searchChanged);

    m_model->beginSearch(QStringLiteral("M"));   // case-insensitive
    QVERIFY(m_model->searchActive());
    QCOMPARE(m_model->searchResultCount(), 3);    // m0, m1, m3 (not "hello")
    // Starts at the newest match ("m3").
    QCOMPARE(m_model->searchCurrentEventId(), QStringLiteral("$e3"));
    QCOMPARE(m_model->searchCurrentPosition(), 3);

    m_model->searchPrev();
    QCOMPARE(m_model->searchCurrentEventId(), QStringLiteral("$e1"));
    QCOMPARE(m_model->searchCurrentPosition(), 2);

    m_model->searchNext();
    QCOMPARE(m_model->searchCurrentEventId(), QStringLiteral("$e3"));
    m_model->searchNext();                        // wraps to the first
    QCOMPARE(m_model->searchCurrentEventId(), QStringLiteral("$e0"));
    QCOMPARE(m_model->searchCurrentPosition(), 1);

    // A single-match query positions "1 of 1".
    m_model->updateSearch(QStringLiteral("hello"));
    QCOMPARE(m_model->searchResultCount(), 1);
    QCOMPARE(m_model->searchCurrentEventId(), QStringLiteral("$e2"));
    QVERIFY(spy.count() >= 1);
}

// A pagination prepend of an older matching event grows the result set and
// keeps the current match selected.
void TimelineModelDiffTest::searchUpdatesOnPaginationInsert()
{
    m_model->beginSearch(QStringLiteral("m1"));
    QCOMPARE(m_model->searchResultCount(), 1);
    const QString selected = m_model->searchCurrentEventId();
    QCOMPARE(selected, QStringLiteral("$e1"));

    // Prepend an older event that also matches "m1".
    auto older = makeEvent(QStringLiteral("$old"), QStringLiteral("m1 older"));
    m_client->mirror.prepend(older);
    Q_EMIT m_client->eventsPrepended(kRoom, { older });

    QCOMPARE(m_model->searchResultCount(), 2);
    // The originally-selected match is preserved (not reset to newest).
    QCOMPARE(m_model->searchCurrentEventId(), selected);
}

// Search state is memory-only: a room switch and endSearch both clear it.
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

    // A room switch also clears an active search.
    m_model->beginSearch(QStringLiteral("m0"));
    QVERIFY(m_model->searchActive());
    const QString other = QStringLiteral("!other:example.org");
    m_client->mirror.clear();
    m_model->setRoomId(other);
    QVERIFY(!m_model->searchActive());
    QCOMPARE(m_model->searchResultCount(), 0);
}

// An edit that changes a body updates matches; redacted events never match.
void TimelineModelDiffTest::searchSurvivesEditAndExcludesRedacted()
{
    m_model->beginSearch(QStringLiteral("needle"));
    QCOMPARE(m_model->searchResultCount(), 0);

    // Edit $e1 to contain the needle → it now matches, live.
    auto edited = makeEvent(QStringLiteral("$e1"),
                            QStringLiteral("has needle now"));
    edited.edited = true;
    m_client->mirror[1] = edited;
    Q_EMIT m_client->eventChangedAt(kRoom, 1, edited);
    QCOMPARE(m_model->searchResultCount(), 1);
    QCOMPARE(m_model->searchCurrentEventId(), QStringLiteral("$e1"));

    // Redact it → visible text becomes empty, so it stops matching.
    auto redacted = makeEvent(QStringLiteral("$e1"), QString());
    redacted.redacted = true;
    m_client->mirror[1] = redacted;
    Q_EMIT m_client->eventChangedAt(kRoom, 1, redacted);
    QCOMPARE(m_model->searchResultCount(), 0);
}


// The thread roles answer from an incrementally maintained index instead of
// scanning the whole event list per query (that cost every delegate two
// full-timeline scans and scaled with loaded history — a real scroll-jitter
// source while backfilling). An index can go stale where a scan could not,
// so this drives each structural mutation and re-checks the answers.
void TimelineModelDiffTest::threadRoleIndexTracksEveryStructuralMutation()
{
    const QString rootId = QStringLiteral("$root");
    auto root = makeEvent(rootId, QStringLiteral("topic"));
    auto reply = makeEvent(QStringLiteral("$r1"), QStringLiteral("reply one"));
    reply.threadRootId = rootId;
    auto plain = makeEvent(QStringLiteral("$p1"), QStringLiteral("unrelated"));

    // Reset path.
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
    QVERIFY(!isRoot(2));           // an ordinary row is never a root
    QCOMPARE(replies(2), 0);

    // Append path: a second reply raises the root's count.
    auto reply2 = makeEvent(QStringLiteral("$r2"), QStringLiteral("reply two"));
    reply2.threadRootId = rootId;
    m_client->mirror.append(reply2);
    Q_EMIT m_client->eventAppended(kRoom, reply2);
    QCOMPARE(replies(0), 2);

    // Prepend path: older history carrying another reply to the same root.
    auto older = makeEvent(QStringLiteral("$r0"), QStringLiteral("older reply"));
    older.threadRootId = rootId;
    m_client->mirror.prepend(older);
    Q_EMIT m_client->eventsPrepended(kRoom, { older });
    QCOMPARE(replies(1), 3);       // the root shifted down by the prepend
    QVERIFY(isRoot(1));

    // Removal path: dropping a reply lowers the count again.
    m_client->mirror.removeAt(0);
    Q_EMIT m_client->eventRemovedAt(kRoom, 0);
    QCOMPARE(replies(0), 2);

    // In-place replacement path: a late decryption revealing a thread
    // relation must make the previously-plain row count toward its root.
    // Rows here are [root, reply, plain, reply2]; plain is row 2.
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

// v0.7.x: near-top backfill "virtual scrolling". While
// backfillStagingActive is true, a landed backward-pagination batch is
// folded into the model's internal mirror but held OUT of the exposed
// QAbstractListModel row space entirely — no rowsInserted, no rowCount()
// change — so a ListView bound to this model sees no structural change at
// all while a reader holds a gesture through active loading. Deactivating
// flushes the held prefix in one clean insert. See
// TimelineModel::setBackfillStagingActive in the .cpp for the full
// mechanism and TimelinePane.qml's backfillStagingActive for the gesture/
// run gate that drives it live.
void TimelineModelDiffTest::stagingHoldsPrependedRowsOutOfExposedSpaceUntilFlush()
{
    QSignalSpy inserted(m_model, &QAbstractItemModel::rowsInserted);
    m_model->setBackfillStagingActive(true);
    QVERIFY(m_model->backfillStagingActive());

    const QList<TimelineEvent> older = {
        makeEvent(QStringLiteral("$old0"), QStringLiteral("o0")),
        makeEvent(QStringLiteral("$old1"), QStringLiteral("o1")),
    };
    for (int i = older.size() - 1; i >= 0; --i)
        m_client->mirror.prepend(older.at(i));
    Q_EMIT m_client->eventsPrepended(kRoom, older);

    // Held: the view sees nothing at all.
    QCOMPARE(m_model->rowCount(), 2);
    QCOMPARE(inserted.count(), 0);
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$e0"));
    // A stable-id lookup for a still-hidden row reads "not found" — the
    // same contract TimelinePane.qml's anchor code already treats as
    // genuinely unresolvable (see maintainViewAnchor()'s captureViewAnchor
    // fallback), never a wrong or stale row.
    QCOMPARE(m_model->rowForStableId(QStringLiteral("uid-$old0")), -1);
    QCOMPARE(m_model->stableIdAt(0), QStringLiteral("uid-$e0"));
    // The internal mirror IS complete, though — policy code (
    // PaginationController) must see real growth even while staged.
    QCOMPARE(m_model->internalEventCount(), 4);

    // Flush (deactivating IS the trigger): one clean insert exposes both
    // staged rows at once.
    m_model->setBackfillStagingActive(false);
    QVERIFY(!m_model->backfillStagingActive());
    QCOMPARE(m_model->rowCount(), 4);
    QCOMPARE(inserted.count(), 1);
    QCOMPARE(inserted.constFirst().at(1).toInt(), 0); // first
    QCOMPARE(inserted.constFirst().at(2).toInt(), 1); // last
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$old0"));
    QCOMPARE(m_model->rowForStableId(QStringLiteral("uid-$old0")), 0);
}

void TimelineModelDiffTest::stagingFlushCoalescesMultipleBatchesIntoOneInsert()
{
    m_model->setBackfillStagingActive(true);
    QSignalSpy inserted(m_model, &QAbstractItemModel::rowsInserted);

    for (int batch = 0; batch < 3; ++batch) {
        const auto e = makeEvent(
            QStringLiteral("$b%1").arg(batch),
            QStringLiteral("batch %1").arg(batch));
        m_client->mirror.prepend(e);
        // Alternate the two shapes production code actually staged-applies:
        // a batch (onEventsPrepended) and the bridge's single push_front
        // shape (onEventInsertedAt at index 0).
        if (batch % 2 == 0)
            Q_EMIT m_client->eventsPrepended(kRoom, { e });
        else
            Q_EMIT m_client->eventInsertedAt(kRoom, 0, e);
        QCOMPARE(m_model->rowCount(), 2); // still nothing exposed
        QCOMPARE(inserted.count(), 0);
    }

    m_model->setBackfillStagingActive(false);
    QCOMPARE(inserted.count(), 1); // ALL three batches in one insert
    QCOMPARE(m_model->rowCount(), 5);
    // Oldest-first: the last-arrived batch is the oldest, so it sits at the
    // very front.
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$b2"));
    QCOMPARE(m_model->data(m_model->index(1), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$b1"));
    QCOMPARE(m_model->data(m_model->index(2), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$b0"));
}

void TimelineModelDiffTest::stagingNeverHoldsBackAnAppendAtTheBottom()
{
    m_model->setBackfillStagingActive(true);
    // Stage a prepend first so the exposed/raw offset is nonzero.
    const auto older = makeEvent(QStringLiteral("$old"), QStringLiteral("o"));
    m_client->mirror.prepend(older);
    Q_EMIT m_client->eventsPrepended(kRoom, { older });
    QCOMPARE(m_model->rowCount(), 2);

    // A live/local-echo append at the bottom must be exposed immediately —
    // staging is for a near-top backfill prefix only.
    QSignalSpy inserted(m_model, &QAbstractItemModel::rowsInserted);
    const auto live = makeEvent(QStringLiteral("$live"), QStringLiteral("l"));
    m_client->mirror.append(live);
    Q_EMIT m_client->eventAppended(kRoom, live);
    QCOMPARE(inserted.count(), 1);
    QCOMPARE(m_model->rowCount(), 3); // the two originals + this one
    QCOMPARE(m_model->data(m_model->index(2), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$live"));

    m_model->setBackfillStagingActive(false);
    QCOMPARE(m_model->rowCount(), 4);
}

void TimelineModelDiffTest::stagingAppliesInPlaceEditsToHiddenRowsSilently()
{
    m_model->setBackfillStagingActive(true);
    auto staged = makeEvent(QStringLiteral("$old"), QStringLiteral("original"));
    m_client->mirror.prepend(staged);
    Q_EMIT m_client->eventsPrepended(kRoom, { staged });
    QCOMPARE(m_model->rowCount(), 2); // held

    // Late decryption / an edit resolving for the still-hidden row must
    // update the mirror WITHOUT trying to notify a view row that does not
    // exist yet.
    QSignalSpy dataSpy(m_model, &QAbstractItemModel::dataChanged);
    auto edited = staged;
    edited.body = QStringLiteral("decrypted");
    edited.isDecrypted = true;
    m_client->mirror[0] = edited;
    Q_EMIT m_client->eventChangedAt(kRoom, 0, edited);
    QCOMPARE(dataSpy.count(), 0);
    QCOMPARE(m_model->rowCount(), 2);

    m_model->setBackfillStagingActive(false);
    QCOMPARE(m_model->rowCount(), 3);
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::BodyRole)
                 .toString(),
             QStringLiteral("decrypted"));
    QVERIFY(m_model->data(m_model->index(0), TimelineModel::IsDecryptedRole)
                .toBool());
}

void TimelineModelDiffTest::stagingHeldPrefixIsDiscardedByARoomReset()
{
    m_model->setBackfillStagingActive(true);
    const auto older = makeEvent(QStringLiteral("$old"), QStringLiteral("o"));
    m_client->mirror.prepend(older);
    Q_EMIT m_client->eventsPrepended(kRoom, { older });
    QCOMPARE(m_model->rowCount(), 2);
    QCOMPARE(m_model->internalEventCount(), 3);

    // A room switch (or any full reset) must discard the held prefix
    // outright, not silently expose or leak it into the fresh snapshot —
    // and must not leave staging latched across the switch.
    m_client->mirror = { makeEvent(QStringLiteral("$fresh"),
                                   QStringLiteral("fresh")) };
    Q_EMIT m_client->timelineReset(kRoom);
    QVERIFY(!m_model->backfillStagingActive());
    QCOMPARE(m_model->rowCount(), 1);
    QCOMPARE(m_model->internalEventCount(), 1);
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$fresh"));
}

void TimelineModelDiffTest::stagingCapBoundsTheHeldPrefixAndAutoFlushes()
{
    m_model->setBackfillStagingActive(true);
    QSignalSpy inserted(m_model, &QAbstractItemModel::rowsInserted);

    // Exceed the safety cap (400) with single-event inserts so an unbounded
    // held gesture cannot grow memory without limit. The 401st insert must
    // force exactly ONE automatic flush spanning the WHOLE accumulated
    // range in one clean beginInsertRows/endInsertRows — not fragmented
    // into several smaller inserts, which would defeat the point of a
    // single reconcile — and staging itself must stay ACTIVE afterward: the
    // cap forces one extra reconciliation mid-run, not an early end to it.
    for (int i = 0; i < 401; ++i) {
        const auto e = makeEvent(QStringLiteral("$c%1").arg(i),
                                 QStringLiteral("c"));
        m_client->mirror.prepend(e);
        Q_EMIT m_client->eventInsertedAt(kRoom, 0, e);
    }
    QCOMPARE(inserted.count(), 1);
    QCOMPARE(inserted.constFirst().at(1).toInt(), 0);   // first row
    QCOMPARE(inserted.constFirst().at(2).toInt(), 400); // last row: 401 rows
    QCOMPARE(m_model->rowCount(), 401 + 2);
    QVERIFY2(m_model->backfillStagingActive(),
             "the cap must not end the run, only force one reconciliation");

    // Staging must have re-engaged for anything landing AFTER the forced
    // flush: a few more inserts accumulate into a fresh hidden prefix
    // rather than exposing immediately.
    for (int i = 0; i < 3; ++i) {
        const auto e = makeEvent(QStringLiteral("$d%1").arg(i),
                                 QStringLiteral("d"));
        m_client->mirror.prepend(e);
        Q_EMIT m_client->eventInsertedAt(kRoom, 0, e);
    }
    QCOMPARE(inserted.count(), 1); // still just the cap flush — these 3 held
    QCOMPARE(m_model->rowCount(), 401 + 2);
    QCOMPARE(m_model->internalEventCount(), 401 + 3 + 2);

    m_model->setBackfillStagingActive(false);
    QCOMPARE(inserted.count(), 2); // the settle flush exposes exactly these 3
    QCOMPARE(inserted.constLast().at(1).toInt(), 0);
    QCOMPARE(inserted.constLast().at(2).toInt(), 2);
    QCOMPARE(m_model->rowCount(), 401 + 3 + 2);
}

// Independent review H1: previousMessageRowForGrouping()/stateGroupLeaderRow()
// used to walk backward past the exposure boundary into the hidden prefix.
// The topmost EXPOSED row could then silently lose its sender identity
// header (its "previous" lookup resolved to a same-sender hidden neighbour),
// or a whole exposed state-change run could resolve its leader to a hidden
// row and collapse its presentation to nothing — content loss and geometry
// churn from INSIDE the mechanism this round exists to prevent. Both walks
// are now clamped at m_hiddenPrefixCount.
void TimelineModelDiffTest::stagingKeepsSenderGroupingSelfConsistentOnExposedBoundary()
{
    // A single existing row. makeEvent()'s fixed default sender/timestamp
    // means a second makeEvent() call is trivially a same-sender,
    // zero-time-gap continuation candidate — exactly the shape that exposes
    // H1 if the backward walk is unbounded.
    m_client->mirror = { makeEvent(QStringLiteral("$existing"), QStringLiteral("e")) };
    Q_EMIT m_client->timelineReset(kRoom);
    QCOMPARE(m_model->rowCount(), 1);

    m_model->setBackfillStagingActive(true);
    const auto staged = makeEvent(QStringLiteral("$staged"), QStringLiteral("s"));
    m_client->mirror.prepend(staged);
    Q_EMIT m_client->eventsPrepended(kRoom, QList<TimelineEvent>{ staged });
    QCOMPARE(m_model->rowCount(), 1); // held

    // The exposed row space must be self-consistent on its own: public row
    // 0 is, from the view's perspective, the very first row. It must not
    // lose its identity header to a same-sender neighbour sitting in the
    // still-hidden prefix.
    QVERIFY2(m_model->data(m_model->index(0),
                           TimelineModel::ShowSenderIdentityRole).toBool(),
             "public row 0 lost its identity header to a hidden neighbour");
    QVERIFY(m_model->data(m_model->index(0),
                          TimelineModel::BeginsSenderGroupRole).toBool());
    QVERIFY(!m_model->data(m_model->index(0),
                           TimelineModel::ContinuesSenderGroupRole).toBool());

    m_model->setBackfillStagingActive(false);
    QCOMPARE(m_model->rowCount(), 2);
    // After flush the true merge is visible: row 1 (the formerly-only row)
    // now correctly continues row 0's group and loses its own header.
    QVERIFY(m_model->data(m_model->index(0),
                          TimelineModel::BeginsSenderGroupRole).toBool());
    QVERIFY(m_model->data(m_model->index(1),
                          TimelineModel::ContinuesSenderGroupRole).toBool());
    QVERIFY(!m_model->data(m_model->index(1),
                           TimelineModel::ShowSenderIdentityRole).toBool());
}

void TimelineModelDiffTest::stagingKeepsStateGroupLeaderSelfConsistentOnExposedBoundary()
{
    TimelineEvent existingActivity;
    existingActivity.eventId = QStringLiteral("$activity-existing");
    existingActivity.itemId = QStringLiteral("uid-activity-existing");
    existingActivity.roomId = kRoom;
    existingActivity.sender = QStringLiteral("@alice:example.org");
    existingActivity.type = TimelineEvent::StateChange;
    existingActivity.timestamp = QDateTime::fromMSecsSinceEpoch(1700000000000);
    m_client->mirror = { existingActivity };
    Q_EMIT m_client->timelineReset(kRoom);
    QCOMPARE(m_model->rowCount(), 1);
    QVERIFY(m_model->data(m_model->index(0),
                          TimelineModel::StateGroupLeaderRole).toBool());

    m_model->setBackfillStagingActive(true);
    TimelineEvent stagedActivity = existingActivity;
    stagedActivity.eventId = QStringLiteral("$activity-staged");
    stagedActivity.itemId = QStringLiteral("uid-activity-staged");
    m_client->mirror.prepend(stagedActivity);
    Q_EMIT m_client->eventsPrepended(kRoom, QList<TimelineEvent>{ stagedActivity });
    QCOMPARE(m_model->rowCount(), 1); // held

    // A contiguous state-change run continuing into the still-hidden prefix
    // must not resolve the exposed row's leader into it — that would make
    // StateGroupLeaderRole false for every exposed row of the run and
    // collapse its whole presentation (rendered only at the leader) to
    // nothing, mid-gesture.
    QVERIFY2(m_model->data(m_model->index(0),
                           TimelineModel::StateGroupLeaderRole).toBool(),
             "the exposed run's leader resolved into the hidden prefix");
    const QVariantList entries = m_model->data(
        m_model->index(0), TimelineModel::StateGroupEntriesRole).toList();
    QCOMPARE(entries.size(), 1); // only the exposed row, not the hidden one

    m_model->setBackfillStagingActive(false);
    QCOMPARE(m_model->rowCount(), 2);
    // After flush the two merge into one run led by row 0.
    QVERIFY(m_model->data(m_model->index(0),
                          TimelineModel::StateGroupLeaderRole).toBool());
    QVERIFY(!m_model->data(m_model->index(1),
                           TimelineModel::StateGroupLeaderRole).toBool());
    const QVariantList mergedEntries = m_model->data(
        m_model->index(0), TimelineModel::StateGroupEntriesRole).toList();
    QCOMPARE(mergedEntries.size(), 2);
}

// M4: onEventRemovedAt landing strictly inside the still-hidden prefix.
void TimelineModelDiffTest::stagingRemovalInsideThePrefixShrinksItSilently()
{
    m_model->setBackfillStagingActive(true);
    const QList<TimelineEvent> older = {
        makeEvent(QStringLiteral("$old0"), QStringLiteral("o0")),
        makeEvent(QStringLiteral("$old1"), QStringLiteral("o1")),
    };
    for (int i = older.size() - 1; i >= 0; --i)
        m_client->mirror.prepend(older.at(i));
    Q_EMIT m_client->eventsPrepended(kRoom, older);
    QCOMPARE(m_model->internalEventCount(), 4);
    QCOMPARE(m_model->rowCount(), 2); // held

    // Remove the older of the two staged rows (raw index 0), strictly
    // inside the hidden prefix.
    m_client->mirror.removeAt(0);
    QSignalSpy removed(m_model, &QAbstractItemModel::rowsRemoved);
    Q_EMIT m_client->eventRemovedAt(kRoom, 0);
    QCOMPARE(removed.count(), 0); // no view row existed to remove
    QCOMPARE(m_model->rowCount(), 2); // still held, unaffected
    QCOMPARE(m_model->internalEventCount(), 3);

    m_model->setBackfillStagingActive(false);
    QCOMPARE(m_model->rowCount(), 3); // one staged row survived, not two
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$old1"));
}

// M4: onEventsTruncatedTo eating into the hidden prefix itself (very rare —
// the backend trimmed its cache further than this run has staged).
void TimelineModelDiffTest::stagingTruncationCuttingIntoThePrefixFlushesFirst()
{
    m_model->setBackfillStagingActive(true);
    const QList<TimelineEvent> older = {
        makeEvent(QStringLiteral("$old0"), QStringLiteral("o0")),
        makeEvent(QStringLiteral("$old1"), QStringLiteral("o1")),
        makeEvent(QStringLiteral("$old2"), QStringLiteral("o2")),
    };
    for (int i = older.size() - 1; i >= 0; --i)
        m_client->mirror.prepend(older.at(i));
    Q_EMIT m_client->eventsPrepended(kRoom, older);
    QCOMPARE(m_model->internalEventCount(), 5);
    QCOMPARE(m_model->rowCount(), 2); // held

    // Truncate to fewer events than the hidden prefix alone (3 held + 2
    // exposed = 5; truncate to 2, i.e. INTO the hidden prefix).
    while (m_client->mirror.size() > 2)
        m_client->mirror.removeLast();
    QSignalSpy inserted(m_model, &QAbstractItemModel::rowsInserted);
    Q_EMIT m_client->eventsTruncatedTo(kRoom, 2);
    // Must self-heal to a consistent state, never corrupt or crash. The
    // implementation flushes first (exposing the whole held prefix in one
    // insert) so the ordinary truncation logic then operates on a plain,
    // single index space.
    QCOMPARE(inserted.count(), 1);
    // The forced flush does not end the staging MODE itself — only this
    // particular held content — same as the safety-cap flush.
    QVERIFY(m_model->backfillStagingActive());
    QCOMPARE(m_model->rowCount(), 2);
    QCOMPARE(m_model->internalEventCount(), 2);
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$old0"));
}

// M4: onEventsTruncatedTo landing exactly AT the exposure boundary — the
// ordinary path, no flush needed, since nothing hidden is touched.
void TimelineModelDiffTest::stagingTruncationExactlyAtTheBoundaryStaysOrdinary()
{
    m_model->setBackfillStagingActive(true);
    const auto staged = makeEvent(QStringLiteral("$old"), QStringLiteral("o"));
    m_client->mirror.prepend(staged);
    Q_EMIT m_client->eventsPrepended(kRoom, QList<TimelineEvent>{ staged });
    QCOMPARE(m_model->internalEventCount(), 3);
    QCOMPARE(m_model->rowCount(), 2); // held: 1 hidden + 2 exposed

    QSignalSpy inserted(m_model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removedSpy(m_model, &QAbstractItemModel::rowsRemoved);
    // Truncate to exactly the hidden-prefix size (1): removes both exposed
    // rows, none of the hidden one.
    while (m_client->mirror.size() > 1)
        m_client->mirror.removeLast();
    Q_EMIT m_client->eventsTruncatedTo(kRoom, 1);
    QCOMPARE(inserted.count(), 0); // no flush was needed
    QCOMPARE(removedSpy.count(), 1);
    QCOMPARE(m_model->rowCount(), 0); // both exposed rows gone
    QCOMPARE(m_model->internalEventCount(), 1); // the hidden one survives
    QVERIFY(m_model->backfillStagingActive());

    m_model->setBackfillStagingActive(false);
    QCOMPARE(m_model->rowCount(), 1);
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$old"));
}

// M4: onEventInsertedAt at index == m_hiddenPrefixCount — the boundary
// itself. This is NOT a new-older-content prepend (that is always raw index
// 0); it targets the position that is CURRENTLY the first EXPOSED row, so it
// must be treated as an ordinary visible insert, never folded into the
// hidden prefix.
void TimelineModelDiffTest::stagingInsertExactlyAtTheBoundaryIsExposedNotStaged()
{
    m_model->setBackfillStagingActive(true);
    const auto staged = makeEvent(QStringLiteral("$old"), QStringLiteral("o"));
    m_client->mirror.prepend(staged);
    Q_EMIT m_client->eventsPrepended(kRoom, QList<TimelineEvent>{ staged });
    QCOMPARE(m_model->internalEventCount(), 3); // 1 hidden + 2 exposed
    QCOMPARE(m_model->rowCount(), 2);

    QSignalSpy inserted(m_model, &QAbstractItemModel::rowsInserted);
    const auto boundary = makeEvent(QStringLiteral("$boundary"),
                                    QStringLiteral("b"));
    m_client->mirror.insert(1, boundary); // raw index 1 == m_hiddenPrefixCount
    Q_EMIT m_client->eventInsertedAt(kRoom, 1, boundary);
    QCOMPARE(inserted.count(), 1);
    QCOMPARE(inserted.constFirst().at(1).toInt(), 0); // exposed public row 0
    QCOMPARE(m_model->rowCount(), 3);
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$boundary"));

    m_model->setBackfillStagingActive(false);
    QCOMPARE(m_model->rowCount(), 4);
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$old"));
}

// M4: mediaEntries() reports a PUBLIC row for a currently-exposed item; that
// row must round-trip through stableIdAt() to the same stable id
// rowForStableId() resolves — the same contract every other view-facing
// lookup in this class honours.
void TimelineModelDiffTest::stagingMediaEntryRowRoundTripsThroughStableIdAt()
{
    auto image = makeEvent(QStringLiteral("$image"), QStringLiteral("cat"));
    image.type = TimelineEvent::Image;
    image.mediaSourceAvailable = true;
    m_client->mirror = { image };
    Q_EMIT m_client->timelineReset(kRoom);
    QCOMPARE(m_model->rowCount(), 1);

    m_model->setBackfillStagingActive(true);
    auto staged = makeEvent(QStringLiteral("$old"), QStringLiteral("o"));
    m_client->mirror.prepend(staged);
    Q_EMIT m_client->eventsPrepended(kRoom, QList<TimelineEvent>{ staged });
    QCOMPARE(m_model->rowCount(), 1); // held

    const QVariantList entries = m_model->mediaEntries();
    QCOMPARE(entries.size(), 1);
    const QVariantMap entry = entries.first().toMap();
    const int publicRow = entry.value(QStringLiteral("row")).toInt();
    QCOMPARE(publicRow, 0); // the only EXPOSED row, not its raw position
    QCOMPARE(m_model->stableIdAt(publicRow), QStringLiteral("uid-$image"));
    QCOMPARE(m_model->rowForStableId(QStringLiteral("uid-$image")), publicRow);

    m_model->setBackfillStagingActive(false);
}

// L8: a match still staged inside the hidden prefix must not be reported —
// the reader could not navigate to it. It is picked up for free via the
// existing rowsInserted->resync wiring the moment it flushes.
void TimelineModelDiffTest::stagingExcludesHeldRowsFromSearchUntilFlush()
{
    m_client->mirror = { makeEvent(QStringLiteral("$e0"), QStringLiteral("nothing here")),
                         makeEvent(QStringLiteral("$e1"), QStringLiteral("nothing here")) };
    Q_EMIT m_client->timelineReset(kRoom);

    m_model->setBackfillStagingActive(true);
    const auto staged = makeEvent(QStringLiteral("$old"), QStringLiteral("needle in here"));
    m_client->mirror.prepend(staged);
    Q_EMIT m_client->eventsPrepended(kRoom, QList<TimelineEvent>{ staged });
    QCOMPARE(m_model->rowCount(), 2); // held

    // beginSearch() recomputes UNCONDITIONALLY — the strongest check that a
    // staged match is excluded even from a search that starts fresh while
    // staging is active, not merely one that happened to run earlier.
    m_model->beginSearch(QStringLiteral("needle"));
    QCOMPARE(m_model->searchResultCount(), 0);

    m_model->setBackfillStagingActive(false);
    QTRY_COMPARE(m_model->searchResultCount(), 1);
    QCOMPARE(m_model->searchCurrentEventId(), QStringLiteral("$old"));
}

QTEST_GUILESS_MAIN(TimelineModelDiffTest)
#include "TimelineModelDiffTest.moc"
