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
    // Counts member lookups so a test can prove a role was computed ONCE.
    // Every identity resolution in the model funnels through here, so a
    // second sanitize walk (or a second name resolution) is visible.
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
        return roomId == kRoom ? mirror : QList<TimelineEvent>{};
    }
    QString displayNameFor(const QString &, const QString &userId) const override
    {
        ++displayNameLookups;
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
    // The live "receipts disappear / swap between users" report: two remote
    // readers advancing independently through the exact adjacent Set pairs
    // the SDK emits, across pagination inserts and member hydration —
    // each reader's latest position must stay represented throughout.
    void twoReadersAdvanceIndependentlyWithoutLoss();
    // v0.6.1 loaded-timeline search.
    void searchFindsMatchesAndNavigatesWithWrap();
    void searchUpdatesOnPaginationInsert();
    void searchClearsOnRoomSwitchAndEnd();
    void searchSurvivesEditAndExcludesRedacted();
    // v0.7.4 (C1): fenced code blocks reach QML as ordered segments, and
    // only for the rows that have one.
    void messageSegmentsSplitCodeBlocksAndAreComputedOnce();
    void messageSegmentsStayEmptyAndFreeForOrdinaryBodies();
    // v0.7.4 (C2): who reacted, resolved like every other identity.
    void reactionRolesNameTheReactorsAndKeepTheUncappedTotal();

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

    // Sender is @alice (makeEvent). Own receipt first (the ONLY
    // exclusion), then the SENDER's implicit receipt — SHOWN, exactly
    // like Element: a user's marker rides their own latest message until
    // they read something newer (the 2026-08-11 two-device report was the
    // old sender-exclusion making receipts vanish the moment the other
    // side sent). Then two genuine other readers with Carol's receipt
    // OLDER than the unknown member's.
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
    // Only the local user is hidden — the sender's own marker renders.
    QCOMPARE(receipts.size(), 3);
    // Newest reader first: the sender's implicit receipt is newest here.
    QCOMPARE(receipts.at(0).toMap().value(QStringLiteral("userId")).toString(),
             QStringLiteral("@alice:example.org"));
    const QVariantMap unknown = receipts.at(1).toMap();
    QCOMPARE(unknown.value(QStringLiteral("userId")).toString(),
             QStringLiteral("@unknown:example.org"));
    QCOMPARE(unknown.value(QStringLiteral("tsMs")).toLongLong(),
             Q_INT64_C(1700000003000));
    // Unresolved member: LOCALPART fallback, never the bare MXID.
    QCOMPARE(unknown.value(QStringLiteral("displayName")).toString(),
             QStringLiteral("unknown"));
    QVERIFY(unknown.value(QStringLiteral("avatarMxc")).toString().isEmpty());
    const QVariantMap carol = receipts.at(2).toMap();
    QCOMPARE(carol.value(QStringLiteral("displayName")).toString(),
             QStringLiteral("Carol"));
    QCOMPARE(carol.value(QStringLiteral("avatarMxc")).toString(),
             QStringLiteral("mxc://example.org/carol"));

    // Companion total: no explicit readByTotal → the delivered list (4)
    // minus the one exclusion (self) found in it.
    QCOMPARE(m_model->data(idx, TimelineModel::ReadReceiptsTotalRole).toInt(),
             3);

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

void TimelineModelDiffTest::twoReadersAdvanceIndependentlyWithoutLoss()
{
    // Helper: the OTHER-reader user ids a row currently represents.
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

    // Rows: index 0 = $e0, 1 = $e1 (from init). Both readers start with a
    // known position on $e1.
    TimelineEvent m1 = makeEvent(QStringLiteral("$e1"), QStringLiteral("m1"));
    m1.readBy = { { anna, Q_INT64_C(1700000001000) },
                  { ben, Q_INT64_C(1700000002000) } };
    m_client->mirror[1] = m1;
    Q_EMIT m_client->eventChangedAt(kRoom, 1, m1);
    QCOMPARE(readersOf(1), QStringList({ ben, anna })); // newest first

    // A new message arrives, then ANNA advances to it. matrix-sdk-ui emits
    // the adjacent pair Set(old row without anna) then Set(new row with
    // anna) — apply exactly that. Ben must remain represented at his
    // previous latest-read position.
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

    // BEN advances too: the pair empties $e1 and joins him to $e2. Anna
    // must remain represented at her latest position.
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

    // Pagination inserts an older page at the top: indexes shift, receipts
    // stay attached to their events.
    TimelineEvent older = makeEvent(QStringLiteral("$older"),
                                    QStringLiteral("history"));
    m_client->mirror.insert(0, older);
    Q_EMIT m_client->eventInsertedAt(kRoom, 0, older);
    QCOMPARE(m_model->data(m_model->index(3),
                           TimelineModel::EventIdRole).toString(),
             QStringLiteral("$e2"));
    QCOMPARE(readersOf(3), QStringList({ ben, anna }));
    QVERIFY(readersOf(0).isEmpty());

    // Member hydration re-announces receipt roles without dropping anyone,
    // and resolves display names in place.
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
    // eventsPrepended inside ONE poll-drain turn (drain cap 64 >= 20).
    //
    // The guarantee is that a page costs O(boundary), never O(loaded rows).
    // It used to be met by coalescing ONE whole-model grouping dataChanged
    // onto the next turn; at 600-900 rows that still rebound and remeasured
    // all of loaded history once per page, and the lag grew the further back
    // the reader went. Grouping is now refreshed synchronously but only
    // AROUND THE MUTATION BOUNDARY, so a refresh may fire per diff as long as
    // each one stays bounded and none spans the model.
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

    // Grouping reads correctly from data() (computed live, uncached).
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::EventIdRole)
                 .toString(),
             QStringLiteral("$old0"));

    // The load-bearing property: no grouping refresh may span the model, and
    // each stays within a small neighbourhood of its own boundary. A refresh
    // covering every row is the regression this test exists to catch, whether
    // it fires once or N times.
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

    // A later mutation still refreshes grouping, equally bounded.
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
    // init() already pointed the model at kRoom, so setRoomId(kRoom) is a
    // NO-OP (it early-returns on an unchanged id) and the model would still
    // be holding init's two plain rows — every assertion below would then be
    // measuring the wrong event. Load the fixture the way every other case
    // in this file does, and prove it landed.
    Q_EMIT m_client->timelineReset(kRoom);
    QCOMPARE(m_model->rowCount(), 1);

    const QModelIndex idx = m_model->index(0);
    m_client->displayNameLookups = 0;
    const QVariantList segments =
        m_model->data(idx, TimelineModel::MessageSegmentsRole).toList();
    const int firstReadLookups = m_client->displayNameLookups;
    QVERIFY(firstReadLookups > 0);   // the mention was really resolved

    QCOMPARE(segments.size(), 3);
    QCOMPARE(segments.at(0).toMap().value(QStringLiteral("kind")).toInt(), 0);
    QCOMPARE(segments.at(1).toMap().value(QStringLiteral("kind")).toInt(), 1);
    QCOMPARE(segments.at(2).toMap().value(QStringLiteral("kind")).toInt(), 0);

    const QVariantMap code = segments.at(1).toMap();
    QCOMPARE(code.value(QStringLiteral("language")).toString(),
             QStringLiteral("rust"));
    // PLAIN text: entities are decoded, so the delegate can render it with
    // Text.PlainText and "&lt;hi&gt;" can never become markup again.
    QCOMPARE(code.value(QStringLiteral("text")).toString(),
             QStringLiteral("fn main() {\n    println!(\"<hi>\");\n}"));

    // The rich runs kept their sanitized html and the resolved mention name.
    QVERIFY(segments.at(0).toMap().value(QStringLiteral("text")).toString()
                .contains(QStringLiteral("Bob B")));
    QVERIFY(segments.at(2).toMap().value(QStringLiteral("text")).toString()
                .contains(QStringLiteral("after")));

    // Memoized: a second read of the role costs no second sanitize walk and
    // no second identity resolution. Every row's delegate binds this role,
    // and the walk is the expensive half.
    m_client->displayNameLookups = 0;
    const QVariantList again =
        m_model->data(idx, TimelineModel::MessageSegmentsRole).toList();
    QCOMPARE(m_client->displayNameLookups, 0);
    QCOMPARE(again, segments);

    // An edit invalidates it — the cache is keyed on the event, not frozen
    // for the session.
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
    // A <pre> that lives inside dropped content is not a code block: nothing
    // in there is rendered, so this row must keep the single-TextEdit path.
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
    // setRoomId(kRoom) would early-return here (init() already set it) and
    // leave init's rows in place. Reset is the load path.
    Q_EMIT m_client->timelineReset(kRoom);
    // Non-vacuous: data() on a row past the end answers an invalid variant
    // whose toList() is empty, so the loop below would "pass" against a
    // fixture that never loaded.
    QCOMPARE(m_model->rowCount(), 4);

    for (int row = 0; row < 4; ++row) {
        QVERIFY2(m_model->data(m_model->index(row),
                               TimelineModel::MessageSegmentsRole)
                     .toList().isEmpty(),
                 qPrintable(QStringLiteral("row %1 must keep the ordinary "
                                           "single-TextEdit path").arg(row)));
    }

    // And the ordinary row is answered WITHOUT a sanitize walk at all — the
    // role's whole cost for a normal message is one substring test, so
    // adding it to every delegate does not double the timeline's parsing.
    // A walk would have resolved the mention.
    m_client->displayNameLookups = 0;
    m_model->data(m_model->index(1), TimelineModel::MessageSegmentsRole);
    QCOMPARE(m_client->displayNameLookups, 0);
    // The rich body itself is still rendered the usual way — and the
    // sanitizer resolves the mention to the ROOM display name. It never
    // echoes the sender's own anchor text (attacker-chosen), and an
    // unresolved mention renders the localpart ("@bob"), so asserting the
    // resolved "Bob B" is what proves the walk really ran.
    QVERIFY(m_model->data(m_model->index(1), TimelineModel::FormattedBodyRole)
                .toString().contains(QStringLiteral("Bob B")));
}

void TimelineModelDiffTest::reactionRolesNameTheReactorsAndKeepTheUncappedTotal()
{
    TimelineEvent e = makeEvent(QStringLiteral("$m0"), QStringLiteral("hi"));
    Reaction thumbs;
    thumbs.key = QString::fromUtf8("\U0001F44D");
    thumbs.count = 7;          // uncapped total, larger than the id window
    thumbs.byMe = true;
    thumbs.myEventId = QStringLiteral("$react0");
    thumbs.senders = { QStringLiteral("@me:example.org"),
                       QStringLiteral("@bob:example.org"),
                       QStringLiteral("@carol:example.org") };
    e.reactions = { thumbs };
    m_client->mirror = { e };
    m_client->displayNames.insert(QStringLiteral("@bob:example.org"),
                                  QStringLiteral("Bob B"));
    // setRoomId(kRoom) is a no-op after init() already set the same id.
    Q_EMIT m_client->timelineReset(kRoom);
    QCOMPARE(m_model->rowCount(), 1);

    // Read buckets through a helper that answers an empty map instead of
    // indexing an empty list: a role that answers nothing must be a legible
    // failure here, not a QList::at abort that kills every case after it.
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
    // The count is the UNCAPPED total; the names are the bounded window the
    // bridge delivered. QML must never have to infer the overflow from a
    // list length that was capped.
    QCOMPARE(bucket.value(QStringLiteral("reactorTotal")).toInt(), 7);

    const QStringList names =
        bucket.value(QStringLiteral("reactorNames")).toStringList();
    QCOMPARE(names, QStringList({ QStringLiteral("me"),
                                  QStringLiteral("Bob B"),
                                  QStringLiteral("carol") }));
    // Order is the bridge's (local user first) and NEVER a bare MXID: an
    // unresolved reactor is a localpart, exactly like every other identity
    // this model shows.
    for (const QString &name : names) {
        QVERIFY(!name.startsWith(QLatin1Char('@')));
        QVERIFY(!name.contains(QLatin1Char(':')));
    }

    // Member hydration must refresh them off that localpart fallback, the
    // same way it refreshes senders and receipt chips.
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

    // Backends that report no reactor identities (mock/HTTP) simply carry an
    // empty list — never a fabricated name, never a guessed count.
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

QTEST_GUILESS_MAIN(TimelineModelDiffTest)
#include "TimelineModelDiffTest.moc"
