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
    void clientSwitchDoesNotLeakSenderProfile();
    void messageActionsUseStableIdentityAndSafeMetadata();
    void staleAndInapplicableMessageActionsAreRejected();
    // v0.7: MSC3381 poll roles and the conservative end-poll rule.
    void pollRolesExposeOutcomeAndEndPermission();
    // v0.6.1 loaded-timeline search.
    void searchFindsMatchesAndNavigatesWithWrap();
    void searchUpdatesOnPaginationInsert();
    void searchClearsOnRoomSwitchAndEnd();
    void searchSurvivesEditAndExcludesRedacted();

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
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::SenderInitialsRole).toString(),
             QStringLiteral("AU"));
    QCOMPARE(m_model->data(m_model->index(0), TimelineModel::SenderAvatarMxcRole).toString(),
             QStringLiteral("mxc://example.org/new-avatar"));
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

QTEST_GUILESS_MAIN(TimelineModelDiffTest)
#include "TimelineModelDiffTest.moc"
