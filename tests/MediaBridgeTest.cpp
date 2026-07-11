// v0.5.11: tests for the shared avatar/media request layer — identical
// request deduplication, per-size cache keys, bounded LRU eviction,
// stale-result rejection after sign-out (account separation), avatar-URI
// change handling, failure marking with explicit retry, missing-avatar
// no-op, and concurrency-bounded queue pumping.

#include "matrix/MatrixClient.h"
#include "media/MediaBridge.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    quint64 nextOp = 1;
    struct Fetch {
        quint64 opId;
        QString key; // media key or mxc uri
        int kind;    // 0 full, 1 thumb, 2 mxc thumb
        int width = 0;
        int height = 0;
    };
    QList<Fetch> fetches;
    bool rejectFetches = false;

    bool supportsMediaBridge() const override { return true; }
    quint64 fetchMedia(const QString &mediaKey, int kind) override
    {
        if (rejectFetches)
            return 0;
        const quint64 op = nextOp++;
        fetches.append({ op, mediaKey, kind, 0, 0 });
        return op;
    }
    quint64 fetchMxcThumbnail(const QString &mxc, int width, int height) override
    {
        if (rejectFetches)
            return 0;
        const quint64 op = nextOp++;
        fetches.append({ op, mxc, 2, width, height });
        return op;
    }

    void succeed(quint64 opId, const QByteArray &bytes)
    {
        Q_EMIT mediaReady(opId, QString(), 0, bytes,
                          QStringLiteral("image/png"), QString());
    }
    void fail(quint64 opId, const QString &category)
    {
        Q_EMIT mediaFailed(opId, QString(), 0, category);
    }

    // Pure virtuals (inert).
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override { return {}; }
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
};

const QString kMxc = QStringLiteral("mxc://example.org/avatar1");

} // namespace

class MediaBridgeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void identicalRequestsAreDeduplicated()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        QCOMPARE(bridge.avatarSource(kMxc, 64), QString());
        QCOMPARE(bridge.avatarSource(kMxc, 64), QString());
        QCOMPARE(bridge.mediaSource(QStringLiteral("$ev1"), QStringLiteral("thumb")),
                 QString());
        QCOMPARE(bridge.mediaSource(QStringLiteral("$ev1"), QStringLiteral("thumb")),
                 QString());
        QCOMPARE(client.fetches.size(), 2); // one per distinct key
    }

    void differentSizesAreDistinctRequests()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        bridge.avatarSource(kMxc, 32);
        bridge.avatarSource(kMxc, 128);
        QCOMPARE(client.fetches.size(), 2);
        QCOMPARE(client.fetches.at(0).width, 32);
        QCOMPARE(client.fetches.at(1).width, 128);
    }

    void completedFetchIsServedFromCache()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);

        bridge.avatarSource(kMxc, 64);
        client.succeed(client.fetches.first().opId, QByteArray("pixels"));
        QCOMPARE(cached.count(), 1);

        const QString source = bridge.avatarSource(kMxc, 64);
        QVERIFY(source.startsWith(QStringLiteral("image://lightning-media/")));
        QCOMPARE(client.fetches.size(), 1); // no second network fetch
        const QString cacheKey = cached.first().at(0).toString();
        QCOMPARE(bridge.cachedBytes(cacheKey), QByteArray("pixels"));
    }

    void evictionIsBoundedLru()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setCacheLimitBytes(10);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);

        bridge.avatarSource(kMxc, 32);
        client.succeed(client.fetches.at(0).opId, QByteArray(8, 'a'));
        bridge.avatarSource(kMxc, 64);
        client.succeed(client.fetches.at(1).opId, QByteArray(8, 'b'));

        const QString firstKey = cached.at(0).at(0).toString();
        const QString secondKey = cached.at(1).at(0).toString();
        QVERIFY(bridge.cachedBytes(firstKey).isEmpty());   // evicted
        QCOMPARE(bridge.cachedBytes(secondKey), QByteArray(8, 'b'));
        QVERIFY(bridge.cacheBytesUsed() <= 10);
    }

    void staleResultAfterSignOutIsRejected()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);

        bridge.avatarSource(kMxc, 64);
        const quint64 preLogoutOp = client.fetches.first().opId;
        client.logout();

        // The previous account's bytes complete after sign-out: they must
        // neither enter the cache nor emit into the new session.
        client.succeed(preLogoutOp, QByteArray("secret"));
        QCOMPARE(cached.count(), 0);
        QCOMPARE(bridge.cacheBytesUsed(), 0);
    }

    void avatarUriChangeIsANewRequest()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        bridge.avatarSource(kMxc, 64);
        client.succeed(client.fetches.first().opId, QByteArray("old"));
        bridge.avatarSource(QStringLiteral("mxc://example.org/avatar2"), 64);
        QCOMPARE(client.fetches.size(), 2);
        QCOMPARE(client.fetches.at(1).key,
                 QStringLiteral("mxc://example.org/avatar2"));
    }

    void failureIsMarkedAndRetryClearsIt()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);

        bridge.avatarSource(kMxc, 64);
        client.fail(client.fetches.first().opId, QStringLiteral("network"));
        QCOMPARE(failed.count(), 1);
        const QString cacheKey = failed.first().at(0).toString();
        QCOMPARE(bridge.failureCategory(cacheKey), QStringLiteral("network"));

        // Repolling the failed source must not hammer the backend.
        bridge.avatarSource(kMxc, 64);
        QCOMPARE(client.fetches.size(), 1);

        // An explicit retry re-dispatches once.
        bridge.retry(cacheKey);
        QVERIFY(bridge.failureCategory(cacheKey).isEmpty());
        bridge.avatarSource(kMxc, 64);
        QCOMPARE(client.fetches.size(), 2);
    }

    void missingAvatarDispatchesNothing()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        QCOMPARE(bridge.avatarSource(QString(), 64), QString());
        QCOMPARE(bridge.avatarSource(QStringLiteral("https://not-mxc"), 64),
                 QString());
        QCOMPARE(client.fetches.size(), 0);
    }

    void concurrencyIsBoundedAndQueuePumps()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        for (int i = 0; i < 6; ++i)
            bridge.mediaSource(QStringLiteral("$ev%1").arg(i),
                               QStringLiteral("thumb"));
        QCOMPARE(client.fetches.size(), 4); // kMaxConcurrent

        client.succeed(client.fetches.first().opId, QByteArray("x"));
        QCOMPARE(client.fetches.size(), 5); // one queued request pumped
        client.fail(client.fetches.at(1).opId, QStringLiteral("network"));
        QCOMPARE(client.fetches.size(), 6);
    }

    void rejectedDispatchReportsFailure()
    {
        FakeClient client;
        client.rejectFetches = true;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);

        bridge.avatarSource(kMxc, 64);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().at(1).toString(), QStringLiteral("rejected"));
    }
};

QTEST_GUILESS_MAIN(MediaBridgeTest)
#include "MediaBridgeTest.moc"
