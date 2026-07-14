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
    void memberProfileUpdateEmitsIdentityRoles();
    void clientSwitchDoesNotLeakSenderProfile();

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

QTEST_GUILESS_MAIN(TimelineModelDiffTest)
#include "TimelineModelDiffTest.moc"
