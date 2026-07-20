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

    void succeed(quint64 opId, const QByteArray &bytes,
                 const QString &mime = QStringLiteral("image/png"))
    {
        Q_EMIT mediaReady(opId, QString(), 0, bytes,
                          mime, QString());
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

        for (int i = 0; i < 10; ++i)
            bridge.mediaSource(QStringLiteral("$ev%1").arg(i),
                               QStringLiteral("thumb"));
        QCOMPARE(client.fetches.size(), 8); // kMaxConcurrent

        client.succeed(client.fetches.first().opId, QByteArray("x"));
        QCOMPARE(client.fetches.size(), 9); // one queued request pumped
        client.fail(client.fetches.at(1).opId, QStringLiteral("network"));
        QCOMPARE(client.fetches.size(), 10);
    }

    void transientFailureExpiresAndRedispatches()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setFailureRetryMsForTest(0); // expire immediately

        bridge.avatarSource(kMxc, 64);
        client.fail(client.fetches.first().opId, QStringLiteral("network"));
        QCOMPARE(client.fetches.size(), 1);

        // The expired transient mark allows exactly one new dispatch.
        bridge.avatarSource(kMxc, 64);
        QCOMPARE(client.fetches.size(), 2);

        // A validation failure stays blocked regardless of the interval.
        client.fail(client.fetches.at(1).opId, QStringLiteral("rejected"));
        bridge.avatarSource(kMxc, 64);
        QCOMPARE(client.fetches.size(), 2);
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

    // v0.7.1 regression: an op the backend never completes must not pin its
    // concurrency slot forever. Before the watchdog, kMaxConcurrent such
    // orphans stalled the whole pipeline ("avatars/images stop loading after
    // a few minutes"). This is the test that would have caught that leak.
    void stuckRequestTimesOutAndReclaimsSlot()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);

        // Saturate every slot with ops that will never be answered.
        for (int i = 0; i < 8; ++i)
            bridge.mediaSource(QStringLiteral("$stuck%1").arg(i),
                               QStringLiteral("thumb"));
        QCOMPARE(client.fetches.size(), 8);          // kMaxConcurrent dispatched
        QCOMPARE(bridge.inflightCountForTest(), 8);

        // A fresh request cannot dispatch while the slots are (leaked) held.
        bridge.mediaSource(QStringLiteral("$fresh"), QStringLiteral("thumb"));
        QCOMPARE(client.fetches.size(), 8);
        QCOMPARE(bridge.queuedCountForTest(), 1);

        // The watchdog reclaims every stuck slot and pumps the queue.
        bridge.setInflightTimeoutMsForTest(0);
        bridge.checkInflightTimeouts();

        QCOMPARE(failed.count(), 8);
        QCOMPARE(failed.first().at(1).toString(), QStringLiteral("timeout"));
        QCOMPARE(bridge.inflightCountForTest(), 1); // the queued $fresh pumped in
        QCOMPARE(bridge.queuedCountForTest(), 0);
        QCOMPARE(client.fetches.size(), 9);
        QCOMPARE(bridge.healthSnapshot().value(QStringLiteral("timedOut"))
                     .toLongLong(),
                 8);
    }

    void timedOutRequestRedispatchesAfterTransientExpiry()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setInflightTimeoutMsForTest(0);
        bridge.setFailureRetryMsForTest(0); // transient mark expires at once

        bridge.avatarSource(kMxc, 64);
        QCOMPARE(client.fetches.size(), 1);
        bridge.checkInflightTimeouts();
        QCOMPARE(bridge.inflightCountForTest(), 0);

        // A "timeout" mark is transient, so the next poll re-dispatches once
        // — never a permanent Loading state.
        bridge.avatarSource(kMxc, 64);
        QCOMPARE(client.fetches.size(), 2);
    }

    // Soak: under sustained saturation with a mixed success/failure outcome,
    // every terminal path must release its slot and the queue must fully
    // drain — in-flight and queued both return to zero, and memory stays
    // bounded.
    void saturationDrainsToZeroWithBoundedMemory()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setCacheLimitBytes(64); // force LRU eviction under load

        constexpr int kN = 200;
        for (int i = 0; i < kN; ++i)
            bridge.mediaSource(QStringLiteral("$soak%1").arg(i),
                               QStringLiteral("thumb"));
        QCOMPARE(bridge.inflightCountForTest(), 8);
        QCOMPARE(bridge.queuedCountForTest(), kN - 8);

        // Resolve every dispatched op in order with a deterministic mix; each
        // completion pumps one queued request into the freed slot, so the
        // fetch list grows to exactly kN (one op per distinct key).
        int resolved = 0;
        int guard = 0;
        while (resolved < client.fetches.size() && guard++ < kN * 8) {
            const quint64 op = client.fetches.at(resolved).opId;
            if (resolved % 5 == 0)
                client.fail(op, QStringLiteral("network"));
            else
                client.succeed(op, QByteArray("x"));
            QVERIFY(bridge.inflightCountForTest() <= 8); // never over-committed
            QVERIFY(bridge.cacheBytesUsed() <= 64);      // bounded throughout
            ++resolved;
        }

        QCOMPARE(client.fetches.size(), kN);      // deduped: one op per key
        QCOMPARE(bridge.inflightCountForTest(), 0);
        QCOMPARE(bridge.queuedCountForTest(), 0);
        const QVariantMap snap = bridge.healthSnapshot();
        QCOMPARE(snap.value(QStringLiteral("completed")).toLongLong()
                     + snap.value(QStringLiteral("failed")).toLongLong(),
                 static_cast<qint64>(kN));
        QVERIFY(bridge.cacheBytesUsed() <= 64);

        // A brand-new request after the storm still dispatches immediately —
        // no leaked slot, no manual intervention, no click required.
        bridge.mediaSource(QStringLiteral("$after"), QStringLiteral("thumb"));
        QCOMPARE(bridge.inflightCountForTest(), 1);
    }

    void animatedGifUsesValidatedLocalFileAndCleansOnLogout()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        QCOMPARE(bridge.animatedSource(QStringLiteral("$gif")), QString());
        QByteArray gif("GIF89a");
        gif.append(QByteArray(64, '\0'));
        client.succeed(client.fetches.first().opId, gif, QStringLiteral("image/gif"));
        QCOMPARE(ready.count(), 1);
        const QString source = bridge.animatedSource(QStringLiteral("$gif"));
        QVERIFY(source.startsWith(QStringLiteral("file://")));
        const QString path = QUrl(source).toLocalFile();
        QVERIFY(QFileInfo::exists(path));
        client.logout();
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(bridge.animatedSource(QStringLiteral("send-queue.localhost/$txn")).isEmpty());
    }

    void fakeAndOversizedGifsNeverReachAnimatedImage()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        bridge.animatedSource(QStringLiteral("$fake"));
        client.succeed(client.fetches.last().opId, QByteArray("<html>not gif</html>"),
                       QStringLiteral("image/gif"));
        QCOMPARE(ready.count(), 0);
        QCOMPARE(failed.count(), 1);

        bridge.animatedSource(QStringLiteral("$large"));
        QByteArray large("GIF89a");
        large.resize(20 * 1024 * 1024 + 1, '\0');
        client.succeed(client.fetches.last().opId, large, QStringLiteral("image/gif"));
        QCOMPARE(ready.count(), 0);
        QCOMPARE(failed.count(), 2);
    }

    void clientPreviewGifUsesTheSameControlledFilePath()
    {
        MediaBridge bridge;
        QByteArray gif("GIF89a"); gif.append(QByteArray(32, '\0'));
        const QString data = QStringLiteral("data:image/gif;base64,")
            + QString::fromLatin1(gif.toBase64());
        const QString source = bridge.previewAnimatedSource(data, QStringLiteral("image/gif"));
        QVERIFY(source.startsWith(QStringLiteral("file://")));
        QVERIFY(QFileInfo::exists(QUrl(source).toLocalFile()));
        QVERIFY(bridge.previewAnimatedSource(QStringLiteral("data:text/html;base64,QQ=="),
                                             QStringLiteral("image/gif")).isEmpty());
    }

    void clientPreviewStaticImageUsesControlledProviderSource()
    {
        MediaBridge bridge;
        QByteArray png(24, '\0');
        png.replace(0, 8, QByteArray("\x89PNG\r\n\x1a\n", 8));
        const QString data = QStringLiteral("data:image/png;base64,")
            + QString::fromLatin1(png.toBase64());
        const QString source = bridge.previewImageSource(data, QStringLiteral("image/png"));
        QVERIFY(source.startsWith(QStringLiteral("image://lightning-media/")));
        QVERIFY(!source.startsWith(QStringLiteral("http")));
        QVERIFY(!source.startsWith(QStringLiteral("file:")));
        QVERIFY(bridge.previewImageSource(
                    QStringLiteral("data:image/png;base64,PGh0bWw+"),
                    QStringLiteral("image/png")).isEmpty());
        QVERIFY(bridge.previewImageSource(
                    QStringLiteral("data:image/svg+xml;base64,PHN2Zz4="),
                    QStringLiteral("image/svg+xml")).isEmpty());
        QByteArray oversized("GIF89a");
        oversized.resize(5 * 1024 * 1024 + 1, '\0');
        QVERIFY(bridge.previewImageSource(
                    QStringLiteral("data:image/gif;base64,")
                        + QString::fromLatin1(oversized.toBase64()),
                    QStringLiteral("image/gif")).isEmpty());
    }
};

QTEST_GUILESS_MAIN(MediaBridgeTest)
#include "MediaBridgeTest.moc"
