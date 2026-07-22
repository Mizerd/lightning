// v0.5.11: tests for the shared avatar/media request layer — identical
// request deduplication, bounded LRU eviction, stale-result rejection after
// sign-out (account separation), avatar-URI change handling, failure marking
// with explicit retry, missing-avatar no-op, and concurrency-bounded queue
// pumping.
//
// v0.7.1 avatar reliability: avatars use ONE canonical fetch edge per
// identity (size-independent cache keys), a reserved avatar-class cache
// budget, active transient-failure expiry via mediaRetryable, a transient
// "unavailable" category for opId==0 dispatch failures, and per-key
// revision-suffixed provider URLs bumped only on actual byte inserts.

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
        int timeoutClass = 0; // v0.7: 0 standard / 1 playable / 2 save
    };
    QList<Fetch> fetches;
    bool rejectFetches = false;

    bool supportsMediaBridge() const override { return true; }
    quint64 fetchMedia(const QString &mediaKey, int kind,
                       int timeoutClass) override
    {
        if (rejectFetches)
            return 0;
        const quint64 op = nextOp++;
        fetches.append({ op, mediaKey, kind, 0, 0, timeoutClass });
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

    // v0.7.1: every requested render size maps onto ONE canonical fetch
    // edge, so one identity shown at ~10 different sizes across the app is
    // exactly one network request, one cache entry, one failure mark — and
    // every surface becomes consistent by construction.
    void allAvatarSizesShareOneCanonicalFetch()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        for (int size : { 30, 34, 48, 56 })
            QCOMPARE(bridge.avatarSource(kMxc, size), QString());
        QCOMPARE(client.fetches.size(), 1);
        QCOMPARE(client.fetches.first().width, 224);
        QCOMPARE(client.fetches.first().height, 224);

        client.succeed(client.fetches.first().opId, QByteArray("pixels"));
        QString shared;
        for (int size : { 30, 34, 48, 56 }) {
            const QString source = bridge.avatarSource(kMxc, size);
            QVERIFY(source.startsWith(QStringLiteral("image://lightning-media/")));
            if (shared.isEmpty())
                shared = source;
            else
                QCOMPARE(source, shared); // identical string at every size
        }
        QCOMPARE(client.fetches.size(), 1); // still exactly one fetch
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

        bridge.mediaSource(QStringLiteral("$lru-a"), QStringLiteral("thumb"));
        client.succeed(client.fetches.at(0).opId, QByteArray(8, 'a'));
        bridge.mediaSource(QStringLiteral("$lru-b"), QStringLiteral("thumb"));
        client.succeed(client.fetches.at(1).opId, QByteArray(8, 'b'));

        const QString firstKey = cached.at(0).at(0).toString();
        const QString secondKey = cached.at(1).at(0).toString();
        QVERIFY(bridge.cachedBytes(firstKey).isEmpty());   // evicted
        QCOMPARE(bridge.cachedBytes(secondKey), QByteArray(8, 'b'));
        QVERIFY(bridge.cacheBytesUsed() <= 10);
    }

    // v0.7.1: avatar-class entries ("mxc:" keys) evict only against their
    // own reserved budget.
    void avatarClassEvictionIsBoundedLru()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setAvatarCacheLimitBytes(10);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);

        bridge.avatarSource(kMxc, 64);
        client.succeed(client.fetches.at(0).opId, QByteArray(8, 'a'));
        bridge.avatarSource(QStringLiteral("mxc://example.org/avatar2"), 64);
        client.succeed(client.fetches.at(1).opId, QByteArray(8, 'b'));

        const QString firstKey = cached.at(0).at(0).toString();
        const QString secondKey = cached.at(1).at(0).toString();
        QVERIFY(bridge.cachedBytes(firstKey).isEmpty());   // evicted
        QCOMPARE(bridge.cachedBytes(secondKey), QByteArray(8, 'b'));
        QVERIFY(bridge.cacheBytesUsed() <= 10);
    }

    // v0.7.1: the long-session degradation fix — churning timeline media
    // through the main budget can never evict a cached avatar, so avatars
    // stay reliable no matter how much media scrolls past.
    void timelineChurnNeverEvictsAvatars()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setCacheLimitBytes(24); // tiny main budget, heavy churn
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);

        bridge.avatarSource(kMxc, 64);
        client.succeed(client.fetches.at(0).opId, QByteArray(12, 'a'));
        const QString avatarKey = cached.at(0).at(0).toString();

        for (int i = 0; i < 20; ++i) {
            bridge.mediaSource(QStringLiteral("$churn%1").arg(i),
                               QStringLiteral("thumb"));
            client.succeed(client.fetches.last().opId, QByteArray(16, 'x'));
        }

        QCOMPARE(bridge.cachedBytes(avatarKey), QByteArray(12, 'a'));
        QVERIFY(!bridge.avatarSource(kMxc, 64).isEmpty()); // still a hit
        int avatarFetches = 0;
        for (const auto &f : client.fetches) {
            if (f.key == kMxc)
                ++avatarFetches;
        }
        QCOMPARE(avatarFetches, 1); // never re-fetched
        QVERIFY(bridge.cacheBytesUsed() <= 24 + 12); // both bounds hold
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
        // The mxc-keyed synchronous lookup QML uses for honest initials.
        QCOMPARE(bridge.avatarFailureCategory(kMxc), QStringLiteral("network"));
        QVERIFY(bridge.avatarFailureCategory(
                    QStringLiteral("mxc://example.org/other")).isEmpty());

        // Repolling the failed source must not hammer the backend, and the
        // suppressed "" still reports its category synchronously.
        QCOMPARE(bridge.avatarSource(kMxc, 64), QString());
        QCOMPARE(client.fetches.size(), 1);
        QCOMPARE(bridge.avatarFailureCategory(kMxc), QStringLiteral("network"));

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

    // v0.7.1 regression: the backend returns opId==0 while the session is
    // restoring/switching (or the media item is not known yet). Marking
    // that PERMANENT poisoned the key for the whole account session — the
    // "room-header avatar skeleton forever" failure. It is now the
    // transient "unavailable" category with the normal retry window.
    void unavailableDispatchIsTransientAndRecovers()
    {
        FakeClient client;
        client.rejectFetches = true;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        QSignalSpy retryable(&bridge, &MediaBridge::mediaRetryable);

        bridge.avatarSource(kMxc, 64);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().at(1).toString(),
                 QStringLiteral("unavailable"));
        QCOMPARE(bridge.avatarFailureCategory(kMxc),
                 QStringLiteral("unavailable"));

        // While the window is armed, repolling cannot hammer the backend.
        QCOMPARE(bridge.avatarSource(kMxc, 64), QString());
        QCOMPARE(failed.count(), 1);
        QCOMPARE(client.fetches.size(), 0);

        // The backend becomes usable; the sweep announces the expiry and
        // the next request dispatches for real.
        client.rejectFetches = false;
        bridge.setFailureRetryMsForTest(0);
        bridge.checkInflightTimeouts();
        QCOMPARE(retryable.count(), 1);
        QVERIFY(retryable.first().at(0).toString()
                    .endsWith(QLatin1Char(':') + kMxc));
        bridge.avatarSource(kMxc, 64);
        QCOMPARE(client.fetches.size(), 1);
        client.succeed(client.fetches.first().opId, QByteArray("pixels"));
        QVERIFY(!bridge.avatarSource(kMxc, 64).isEmpty());
    }

    // v0.7.1: active expiry. The watchdog tick sweeps expired TRANSIENT
    // marks and emits mediaRetryable so a quiesced UI recovers without any
    // interaction; permanent validation categories are never swept.
    void sweepEmitsMediaRetryableForExpiredTransientMarks()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy retryable(&bridge, &MediaBridge::mediaRetryable);

        bridge.avatarSource(kMxc, 64);
        client.fail(client.fetches.first().opId, QStringLiteral("network"));

        // Inside the window nothing expires.
        bridge.checkInflightTimeouts();
        QCOMPARE(retryable.count(), 0);
        QCOMPARE(bridge.avatarFailureCategory(kMxc), QStringLiteral("network"));

        bridge.setFailureRetryMsForTest(1);
        QTest::qWait(5);
        bridge.checkInflightTimeouts();
        QCOMPARE(retryable.count(), 1);
        QVERIFY(retryable.first().at(0).toString()
                    .endsWith(QLatin1Char(':') + kMxc));
        QVERIFY(bridge.avatarFailureCategory(kMxc).isEmpty()); // mark swept
        bridge.avatarSource(kMxc, 64);
        QCOMPARE(client.fetches.size(), 2); // re-dispatch allowed

        // A permanent validation failure is never swept or re-announced.
        client.fail(client.fetches.at(1).opId, QStringLiteral("rejected"));
        QTest::qWait(5);
        bridge.checkInflightTimeouts();
        QCOMPARE(retryable.count(), 1);
        QCOMPARE(bridge.avatarFailureCategory(kMxc), QStringLiteral("rejected"));
        bridge.avatarSource(kMxc, 64);
        QCOMPARE(client.fetches.size(), 2); // still blocked
    }

    // v0.7.1: provider URLs carry a per-key revision bumped ONLY on an
    // actual byte insert. Cache hits keep the identical string (QML
    // pixmap-cache dedup survives); a re-fetch after eviction produces a
    // new string, so an Image stuck in Error reloads (the cache-hit-then-
    // evicted race between avatarSource() and the provider's later read).
    void revisionChangesExactlyOnByteInsert()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setAvatarCacheLimitBytes(10);

        bridge.avatarSource(kMxc, 64);
        client.succeed(client.fetches.at(0).opId, QByteArray(6, 'a'));
        const QString url1 = bridge.avatarSource(kMxc, 64);
        QVERIFY(url1.contains(QStringLiteral("?r=")));
        QCOMPARE(bridge.avatarSource(kMxc, 64), url1); // hit: identical

        // Eviction race analog: the key was served (cache hit above), then
        // LRU eviction empties it before the provider read.
        bridge.avatarSource(QStringLiteral("mxc://example.org/avatar2"), 64);
        client.succeed(client.fetches.at(1).opId, QByteArray(8, 'b'));
        QCOMPARE(bridge.avatarSource(kMxc, 64), QString()); // miss → dispatch
        QCOMPARE(client.fetches.size(), 3);
        client.succeed(client.fetches.at(2).opId, QByteArray(6, 'a'));
        const QString url2 = bridge.avatarSource(kMxc, 64);
        QVERIFY(!url2.isEmpty());
        QVERIFY2(url2 != url1, "re-cached bytes must yield a NEW source string");
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

    // ── v0.7: playable (video/audio) materialization ────────────────────

    // Container sniffing is fail-closed: only known audio/video magic
    // passes, mislabeled bytes never materialize, and raw ADTS needs the
    // metadata to actually claim AAC.
    void playableSniffingIsFailClosed()
    {
        auto ext = [](const QByteArray &bytes, const char *mime) {
            return MediaBridge::playableExtensionFor(
                bytes, QString::fromLatin1(mime));
        };
        QByteArray mp4 = QByteArrayLiteral("\x00\x00\x00\x18""ftypisom");
        mp4.resize(64, '\0');
        QCOMPARE(ext(mp4, "video/mp4"), QStringLiteral("mp4"));
        QCOMPARE(ext(mp4, "audio/mp4"), QStringLiteral("m4a"));
        QByteArray webm = QByteArrayLiteral("\x1A\x45\xDF\xA3");
        webm.resize(64, '\0');
        QCOMPARE(ext(webm, "video/webm"), QStringLiteral("webm"));
        QCOMPARE(ext(webm, "video/x-matroska"), QStringLiteral("mkv"));
        QByteArray ogg = QByteArrayLiteral("OggS");
        ogg.resize(64, '\0');
        QCOMPARE(ext(ogg, "audio/ogg"), QStringLiteral("ogg"));
        QByteArray wav = QByteArrayLiteral("RIFF\x24\x00\x00\x00WAVE");
        wav.resize(64, '\0');
        QCOMPARE(ext(wav, "audio/wav"), QStringLiteral("wav"));
        QByteArray mp3 = QByteArrayLiteral("ID3\x04");
        mp3.resize(64, '\0');
        QCOMPARE(ext(mp3, "audio/mpeg"), QStringLiteral("mp3"));
        QByteArray flac = QByteArrayLiteral("fLaC");
        flac.resize(64, '\0');
        QCOMPARE(ext(flac, "audio/flac"), QStringLiteral("flac"));
        // Frame-sync MP3 needs the mimetype; ADTS needs an AAC claim.
        QByteArray sync = QByteArrayLiteral("\xFF\xFB\x90\x00");
        sync.resize(64, '\0');
        QCOMPARE(ext(sync, "audio/mpeg"), QStringLiteral("mp3"));
        QVERIFY(ext(sync, "").isEmpty());
        QByteArray adts = QByteArrayLiteral("\xFF\xF1\x50\x80");
        adts.resize(64, '\0');
        QCOMPARE(ext(adts, "audio/aac"), QStringLiteral("aac"));
        QVERIFY(ext(adts, "audio/mpeg").isEmpty());
        // Junk and non-media formats are rejected outright.
        QByteArray junk(64, 'x');
        QVERIFY(ext(junk, "video/mp4").isEmpty());
        QByteArray gif = QByteArrayLiteral("GIF89a");
        gif.resize(64, '\0');
        QVERIFY(ext(gif, "image/gif").isEmpty());
        QVERIFY(ext(QByteArrayLiteral("sh"), "video/mp4").isEmpty());
    }

    // The playable flow materializes a validated payload as a session temp
    // file (correct suffix, restrictive permissions, unguessable name) and
    // reports it via playableMediaReady; invalid payloads fail closed with
    // the permanent "rejected" category.
    void playableSourceMaterializesValidatedFile()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);

        // First call dispatches (nothing cached yet).
        QCOMPARE(bridge.playableSource(QStringLiteral("$vid")), QString());
        QCOMPARE(client.fetches.size(), 1);
        QByteArray ogg = QByteArrayLiteral("OggS");
        ogg.resize(2048, '\1');
        client.succeed(client.fetches.at(0).opId, ogg,
                       QStringLiteral("audio/ogg"));
        QCOMPARE(ready.count(), 1);
        const QString url = bridge.playableSource(QStringLiteral("$vid"));
        QVERIFY(url.startsWith(QStringLiteral("file:")));
        QVERIFY(url.endsWith(QStringLiteral(".ogg")));
        const QString path = QUrl(url).toLocalFile();
        QVERIFY(QFileInfo::exists(path));
        // Unguessable name: never derived from the raw key alone.
        QVERIFY(!QFileInfo(path).fileName().contains(QStringLiteral("$vid")));
        const auto perms = QFileInfo(path).permissions();
        QVERIFY(!(perms & QFileDevice::ReadGroup));
        QVERIFY(!(perms & QFileDevice::ReadOther));

        // Mislabeled/junk bytes fail closed with the permanent category.
        QCOMPARE(bridge.playableSource(QStringLiteral("$bad")), QString());
        QCOMPARE(client.fetches.size(), 2);
        client.succeed(client.fetches.at(1).opId, QByteArray(2048, 'x'),
                       QStringLiteral("video/mp4"));
        QCOMPARE(ready.count(), 1); // no new materialization
        bool sawRejected = false;
        for (const auto &args : failed) {
            if (args.at(0).toString() == QStringLiteral("full:$bad")
                && args.at(1).toString() == QStringLiteral("rejected"))
                sawRejected = true;
        }
        QVERIFY(sawRejected);

        // clear() (sign-out / account switch) removes the decrypted file.
        bridge.clear();
        QVERIFY(!QFileInfo::exists(path));
        QCOMPARE(bridge.playableSource(QStringLiteral("$vid")), QString());
        QCOMPARE(client.fetches.size(), 3); // re-dispatch, no stale reuse
    }

    // Large playable payloads bypass the RAM LRU — one video must not
    // evict the entire image cache — while small ones still cache.
    void largePlayablePayloadSkipsRamCache()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        QCOMPARE(bridge.playableSource(QStringLiteral("$big")), QString());
        QByteArray big = QByteArrayLiteral("OggS");
        big.resize(9 * 1024 * 1024, '\2'); // > 8 MiB skip threshold
        client.succeed(client.fetches.at(0).opId, big,
                       QStringLiteral("audio/ogg"));
        QVERIFY(bridge.cacheBytesUsed() < 1024 * 1024);
        QVERIFY(!bridge.playableSource(QStringLiteral("$big")).isEmpty());

        QCOMPARE(bridge.playableSource(QStringLiteral("$small")), QString());
        QByteArray small = QByteArrayLiteral("OggS");
        small.resize(4096, '\3');
        client.succeed(client.fetches.at(1).opId, small,
                       QStringLiteral("audio/ogg"));
        QVERIFY(bridge.cacheBytesUsed() >= small.size());
    }

    // ── v0.7 defense-in-depth: timeout classes + terminal coordination ──

    // Each dispatch advertises its backend timeout class so the Rust bound
    // stays strictly below the covering C++ watchdog deadline.
    void dispatchCarriesTimeoutClasses()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        bridge.mediaSource(QStringLiteral("$img"), QStringLiteral("full"));
        QCOMPARE(client.fetches.at(0).timeoutClass, 0);

        bridge.playableSource(QStringLiteral("$vid"));
        QCOMPARE(client.fetches.at(1).timeoutClass, 1);

        bridge.saveAs(QStringLiteral("$file"),
                      QUrl::fromLocalFile(QStringLiteral("/tmp/x")));
        QCOMPARE(client.fetches.at(2).timeoutClass, 2);
    }

    // A duplicated terminal for the same op releases exactly one slot and
    // emits exactly one mediaCached; the duplicate counts as stale.
    void duplicateTerminalIsCountedStale()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);

        bridge.avatarSource(kMxc, 64);
        const quint64 op = client.fetches.first().opId;
        client.succeed(op, QByteArray("pixels"));
        client.succeed(op, QByteArray("pixels-again"));

        QCOMPARE(cached.count(), 1);
        QCOMPARE(bridge.inflightCountForTest(), 0);
        QCOMPARE(bridge.healthSnapshot()
                     .value(QStringLiteral("droppedStale")).toLongLong(),
                 1);
    }

    // Watchdog fires first, the real completion lands later: the late
    // success is silently discarded (no duplicate signal, no cache entry
    // resurrection) and the pipeline stays fully drained.
    void lateSuccessAfterWatchdogIsDiscarded()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);

        bridge.avatarSource(kMxc, 64);
        const quint64 op = client.fetches.first().opId;
        bridge.setInflightTimeoutMsForTest(0);
        bridge.checkInflightTimeouts(); // reclaim → transient timeout mark
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().at(1).toString(), QStringLiteral("timeout"));
        QCOMPARE(bridge.inflightCountForTest(), 0);

        client.succeed(op, QByteArray("late"));
        QCOMPARE(cached.count(), 0); // never delivered
        QCOMPARE(bridge.cacheBytesUsed(), 0);
        QCOMPARE(bridge.healthSnapshot()
                     .value(QStringLiteral("droppedStale")).toLongLong(),
                 1);
    }
};

QTEST_GUILESS_MAIN(MediaBridgeTest)
#include "MediaBridgeTest.moc"
