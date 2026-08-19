// 2026-08-19 Element-parity round: SpaceManager::setSpaceChildSuggested
// plumbing — pending-op tracking against MatrixClient's
// spaceChildSuggestedFinished, foreign-op rejection, the refused-send
// (opId 0) immediate failure, and account isolation on logout. The wire
// behavior (state-event read-modify-send preserving via/order) is Rust
// and is not what this suite proves.
#include <QSignalSpy>
#include <QtTest>

#include "matrix/MatrixClient.h"
#include "spaces/SpaceManager.h"

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    // MatrixClient pure virtuals (inert).
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override
    { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &) const override
    { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override
    { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override
    { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override
    { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &,
                   const QString &) override {}
    void editMessage(const QString &, const QString &,
                     const QString &) override {}
    void redactEvent(const QString &, const QString &,
                     const QString &) override {}
    void toggleReaction(const QString &, const QString &,
                        const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }

    quint64 setSpaceChildSuggested(const QString &spaceId,
                                   const QString &roomId,
                                   bool suggested) override
    {
        lastSpaceId = spaceId;
        lastRoomId = roomId;
        lastSuggested = suggested;
        if (refuse)
            return 0;
        return ++opCounter;
    }
    void finishSuggested(quint64 opId, const QString &spaceId,
                         const QString &roomId, bool suggested, bool ok)
    {
        Q_EMIT spaceChildSuggestedFinished(opId, spaceId, roomId, suggested,
                                           ok);
    }

    QString lastSpaceId;
    QString lastRoomId;
    bool lastSuggested = false;
    bool refuse = false;
    quint64 opCounter = 0;
};

const QString kSpace = QStringLiteral("!space:example.org");
const QString kRoom = QStringLiteral("!room:example.org");

} // namespace

class SpaceChildSuggestTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void forwardsAndReportsOnlyOwnOps()
    {
        FakeClient client;
        SpaceManager mgr;
        mgr.setClient(&client);
        QSignalSpy done(&mgr, &SpaceManager::childSuggestedFinished);

        mgr.setSpaceChildSuggested(kSpace, kRoom, true);
        QCOMPARE(client.lastSpaceId, kSpace);
        QCOMPARE(client.lastRoomId, kRoom);
        QCOMPARE(client.lastSuggested, true);
        QCOMPARE(done.count(), 0); // pending, not finished

        // A foreign op (another manager, another surface) must not be
        // reported as this call's outcome.
        client.finishSuggested(9999, kSpace, kRoom, true, true);
        QCOMPARE(done.count(), 0);

        client.finishSuggested(client.opCounter, kSpace, kRoom, true, true);
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.at(0).at(0).toString(), kSpace);
        QCOMPARE(done.at(0).at(1).toString(), kRoom);
        QCOMPARE(done.at(0).at(2).toBool(), true);
        QCOMPARE(done.at(0).at(3).toBool(), true);

        // The op is consumed — a duplicate result is dropped.
        client.finishSuggested(client.opCounter, kSpace, kRoom, true, true);
        QCOMPARE(done.count(), 1);
    }

    void refusedSendFailsImmediately()
    {
        FakeClient client;
        client.refuse = true;
        SpaceManager mgr;
        mgr.setClient(&client);
        QSignalSpy done(&mgr, &SpaceManager::childSuggestedFinished);

        mgr.setSpaceChildSuggested(kSpace, kRoom, false);
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.at(0).at(2).toBool(), false); // suggested arg echoed
        QCOMPARE(done.at(0).at(3).toBool(), false); // ok=false
    }

    void emptyArgumentsAreIgnored()
    {
        FakeClient client;
        SpaceManager mgr;
        mgr.setClient(&client);
        QSignalSpy done(&mgr, &SpaceManager::childSuggestedFinished);
        mgr.setSpaceChildSuggested(QString(), kRoom, true);
        mgr.setSpaceChildSuggested(kSpace, QString(), true);
        QCOMPARE(client.opCounter, quint64(0));
        QCOMPARE(done.count(), 0);
    }

    void logoutClearsPendingOps()
    {
        FakeClient client;
        SpaceManager mgr;
        mgr.setClient(&client);
        QSignalSpy done(&mgr, &SpaceManager::childSuggestedFinished);

        mgr.setSpaceChildSuggested(kSpace, kRoom, true);
        const quint64 op = client.opCounter;
        Q_EMIT client.loggedOut();
        // A late result from the signed-out account's op must not report
        // into the next session (account isolation).
        client.finishSuggested(op, kSpace, kRoom, true, true);
        QCOMPARE(done.count(), 0);
    }
};

QTEST_GUILESS_MAIN(SpaceChildSuggestTest)
#include "SpaceChildSuggestTest.moc"
