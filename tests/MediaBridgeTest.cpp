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
#include "media/MediaImageProvider.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QLoggingCategory>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

// Log capture for cacheHitTraceIsSilentByDefault: a QtMessageHandler is a bare
// function pointer, so the sink is a file-scope static. qCDebug consults the
// category's enabled state BEFORE invoking the handler, so a message routed to
// a default-off category (lightning.media.trace) never reaches this at all —
// exactly the "hit is silent" property under test.
QStringList &capturedLines()
{
    static QStringList lines;
    return lines;
}
void captureHandler(QtMsgType, const QMessageLogContext &ctx, const QString &msg)
{
    capturedLines() << (QString::fromUtf8(ctx.category ? ctx.category : "")
                        + QLatin1Char(' ') + msg);
}

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
    QList<quint64> cancels;
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
    void cancelMediaFetch(quint64 opId) override { cancels.append(opId); }

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

    // Touchpad pass: the per-call cache=HIT trace must be silent by default so
    // scrolling never emits a GUI-thread log storm, while the real media
    // diagnostics (miss/dispatch/failure on `lightning.media`) stay available.
    // mediaSource()/avatarSource() run from QML bindings and re-fire on every
    // pooled-delegate rebind, so a hit logged at debug level flooded the
    // journal each frame during a scroll.
    void cacheHitTraceIsSilentByDefault()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        // Prime the cache with a completed avatar fetch.
        bridge.avatarSource(kMxc, 64);
        client.succeed(client.fetches.first().opId, QByteArray("pixels"));

        auto &captured = capturedLines();
        captured.clear();
        QtMessageHandler prev = qInstallMessageHandler(&captureHandler);
        // Cache HIT — the hot path that QML bindings re-run on every pooled
        // delegate rebind while scrolling. Must emit no log on the default
        // configuration.
        const QString hit = bridge.avatarSource(kMxc, 64);
        // Cache MISS on a fresh identity — a real, bounded diagnostic that
        // must stay visible on the default-on `lightning.media` category.
        bridge.avatarSource(QStringLiteral("mxc://mock.local/second"), 64);
        qInstallMessageHandler(prev);

        QVERIFY(hit.startsWith(QStringLiteral("image://lightning-media/")));
        bool sawMiss = false;
        for (const QString &line : captured) {
            QVERIFY2(!line.contains(QStringLiteral("cache=hit")),
                     qUtf8Printable("unexpected cache=hit storm line: " + line));
            if (line.contains(QStringLiteral("cache=miss")))
                sawMiss = true;
        }
        QVERIFY2(sawMiss,
                 "media cache=miss diagnostic must remain on lightning.media");
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

    // v0.6.6: "star a chat GIF" fetch trigger — mirrors saveAs()'s
    // dispatch/cache/failure shape but relays raw bytes via
    // mediaBytesForStar() instead of writing a user-chosen file.
    void fetchFullForStarDispatchesAtSaveTimeoutClassAndDeliversBytes()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy starred(&bridge, &MediaBridge::mediaBytesForStar);

        bridge.fetchFullForStar(QStringLiteral("$gif"));
        QCOMPARE(client.fetches.size(), 1);
        QCOMPARE(client.fetches.first().timeoutClass, 2); // save class
        QCOMPARE(starred.count(), 0);

        client.succeed(client.fetches.first().opId, QByteArray("GIF89a..."));
        QCOMPARE(starred.count(), 1);
        QVERIFY(starred.first().at(1).toBool());
        QCOMPARE(starred.first().at(2).toByteArray(), QByteArray("GIF89a..."));
        // Never inserted into the shared RAM cache — an "export" fetch,
        // exactly like saveAs.
        QCOMPARE(bridge.cacheBytesUsed(), 0);
    }

    void fetchFullForStarServesFromCacheWithoutDispatching()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        // Prime the cache the way an inline preview (animatedSource) would.
        bridge.mediaSource(QStringLiteral("$gif"), QStringLiteral("full"));
        client.succeed(client.fetches.first().opId, QByteArray("cached-bytes"));
        QCOMPARE(client.fetches.size(), 1);

        QSignalSpy starred(&bridge, &MediaBridge::mediaBytesForStar);
        bridge.fetchFullForStar(QStringLiteral("$gif"));
        QCOMPARE(client.fetches.size(), 1); // no new dispatch — cache hit
        QCOMPARE(starred.count(), 1);
        QVERIFY(starred.first().at(1).toBool());
        QCOMPARE(starred.first().at(2).toByteArray(), QByteArray("cached-bytes"));
    }

    // L10: a rapid double-activation of "Star GIF" for the SAME row must
    // not dispatch two identical fetches.
    void fetchFullForStarDedupsInFlightRequestsForSameMediaKey()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy starred(&bridge, &MediaBridge::mediaBytesForStar);

        bridge.fetchFullForStar(QStringLiteral("$gif"));
        bridge.fetchFullForStar(QStringLiteral("$gif"));
        QCOMPARE(client.fetches.size(), 1); // the second call was dropped

        // A DIFFERENT media key is never suppressed by the first's dedup.
        bridge.fetchFullForStar(QStringLiteral("$other"));
        QCOMPARE(client.fetches.size(), 2);

        client.succeed(client.fetches.at(0).opId, QByteArray("a"));
        client.succeed(client.fetches.at(1).opId, QByteArray("b"));
        QCOMPARE(starred.count(), 2); // both real requests still resolve
    }

    // L11: a save/star dispatch failure must be reported ONLY through its
    // own signal — never through mediaFetchFailed(cacheKey), which an
    // ordinary consumer of the SAME cache key (an inline preview already on
    // screen) would otherwise misread as ITS OWN fetch having failed.
    void starDispatchFailureNeverEmitsMediaFetchFailed()
    {
        FakeClient client;
        client.rejectFetches = true;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy starred(&bridge, &MediaBridge::mediaBytesForStar);
        QSignalSpy fetchFailed(&bridge, &MediaBridge::mediaFetchFailed);

        bridge.fetchFullForStar(QStringLiteral("$gif"));

        QCOMPARE(starred.count(), 1);
        QVERIFY(!starred.first().at(1).toBool());
        QCOMPARE(fetchFailed.count(), 0); // the ordinary-consumer signal
    }

    // An ordinary fetch for the SAME media key as an in-flight star request
    // must dispatch its own real fetch rather than being treated as
    // "already pending" — onMediaReady's star branch returns before ever
    // emitting mediaCached(), so an ordinary caller folded into that
    // dedup would wait forever.
    void ordinaryFetchNotDedupedAgainstInFlightStarRequest()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);

        bridge.fetchFullForStar(QStringLiteral("$gif")); // in flight, save class
        const QString source =
            bridge.mediaSource(QStringLiteral("$gif"), QStringLiteral("full"));
        QCOMPARE(source, QString()); // not cached yet — dispatched, not skipped
        QCOMPARE(client.fetches.size(), 2);

        client.succeed(client.fetches.at(1).opId, QByteArray("real-bytes"));
        QCOMPARE(cached.count(), 1); // the ordinary fetch resolved normally
    }

    // v0.6.6 fix: GifStarredStore's durable "is this GIF already starred?"
    // check (AppController::isChatGifStarred/unstarChatGif) reads the
    // content hash of whatever full payload is ALREADY cached for a
    // mediaKey — never dispatching a fetch of its own.
    void cachedFullContentHashMatchesSha256OfCachedFullPayloadWithoutDispatching()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        // Prime the cache the way an inline preview (animatedSource) would.
        bridge.mediaSource(QStringLiteral("$gif"), QStringLiteral("full"));
        const QByteArray bytes("GIF89a-cached-full-bytes");
        client.succeed(client.fetches.first().opId, bytes);
        QCOMPARE(client.fetches.size(), 1);

        const QString expected = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        QCOMPARE(bridge.cachedFullContentHash(QStringLiteral("$gif")), expected);
        QCOMPARE(client.fetches.size(), 1); // no new dispatch — read-only
    }

    // M2: the silent-always-false failure mode this guards against is
    // hashing mediaCacheKey(key, 1) ("thumb:") instead of mediaCacheKey(key,
    // 0) ("full:") — that bug would pass every other test in this file (an
    // empty/uncached/unknown key all correctly answer "") but silently never
    // answer true for a real GIF whose only cached entry is its thumbnail.
    void cachedFullContentHashIsEmptyUnlessTheFullPayloadItselfIsCached()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        // Empty key.
        QVERIFY(bridge.cachedFullContentHash(QString()).isEmpty());
        // Never requested at all.
        QVERIFY(bridge.cachedFullContentHash(QStringLiteral("$never-fetched")).isEmpty());

        // Only a THUMBNAIL is cached for this key — the full payload was
        // never fetched. Must still answer "", not the thumbnail's hash.
        bridge.mediaSource(QStringLiteral("$thumb-only"), QStringLiteral("thumb"));
        client.succeed(client.fetches.first().opId, QByteArray("thumbnail-bytes"));
        QVERIFY(bridge.cachedFullContentHash(QStringLiteral("$thumb-only")).isEmpty());
    }

    // H1b: pins the memoization itself with a deterministic, timing-
    // independent observable (a computation counter — see
    // MediaBridge::healthSnapshot's new "contentHashComputed" entry) rather
    // than a wall-clock assertion. Also proves the three documented
    // invalidation points: an actual byte re-insert (revision bump), LRU
    // eviction, and clear().
    void cachedFullContentHashIsMemoizedPerCacheKey()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.mediaSource(QStringLiteral("$gif"), QStringLiteral("full"));
        client.succeed(client.fetches.first().opId, QByteArray(4096, 'x'));

        // Every one of the (up to eight, per MessageDelegate.qml) triggers
        // this query is fired from must cost ONE real hash, not eight.
        // mediaSource() itself is a cache HIT for an already-cached key (it
        // never re-dispatches, so there is no public way to force a second
        // insert for the SAME key without first evicting it — see below),
        // which is exactly the "no NOTIFY, refresh from several signals"
        // shape MessageDelegate.qml's own repeated querying has.
        for (int i = 0; i < 8; ++i)
            bridge.cachedFullContentHash(QStringLiteral("$gif"));
        QCOMPARE(bridge.healthSnapshot()
                     .value(QStringLiteral("contentHashComputed")).toLongLong(),
                 qint64(1));
        QCOMPARE(bridge.cachedFullContentHash(QStringLiteral("$gif")),
                 QString::fromLatin1(QCryptographicHash::hash(
                     QByteArray(4096, 'x'), QCryptographicHash::Sha256).toHex()));

        // LRU eviction invalidates the memo: a tiny cache limit forces
        // "$gif" out the moment a second key is inserted, so a subsequent
        // query for "$gif" (now uncached) answers "" without touching the
        // stale memo entry, and does NOT count as a new computation.
        bridge.setCacheLimitBytes(1); // smaller than either payload
        bridge.mediaSource(QStringLiteral("$other"), QStringLiteral("full"));
        client.succeed(client.fetches.last().opId, QByteArray(4096, 'z'));
        QVERIFY(bridge.cachedFullContentHash(QStringLiteral("$gif")).isEmpty());
        QCOMPARE(bridge.healthSnapshot()
                     .value(QStringLiteral("contentHashComputed")).toLongLong(),
                 qint64(1)); // unchanged — a cache miss never hashes

        // A genuine RE-fetch of "$gif" (now actually uncached, so
        // mediaSource() dispatches for real) is a fresh byte insert — a
        // revision bump — and must be re-hashed exactly once, with the NEW
        // bytes' digest, never the stale pre-eviction one.
        bridge.setCacheLimitBytes(64 * 1024 * 1024);
        bridge.mediaSource(QStringLiteral("$gif"), QStringLiteral("full"));
        client.succeed(client.fetches.last().opId, QByteArray(4096, 'y'));
        for (int i = 0; i < 3; ++i)
            bridge.cachedFullContentHash(QStringLiteral("$gif"));
        QCOMPARE(bridge.healthSnapshot()
                     .value(QStringLiteral("contentHashComputed")).toLongLong(),
                 qint64(2)); // exactly one MORE computation, not three
        QCOMPARE(bridge.cachedFullContentHash(QStringLiteral("$gif")),
                 QString::fromLatin1(QCryptographicHash::hash(
                     QByteArray(4096, 'y'), QCryptographicHash::Sha256).toHex()));

        // clear() (sign-out/account switch) drops every memo entry along
        // with the cache itself — a post-clear re-fetch of the SAME key
        // must compute again, not reuse a stale pre-clear memo entry.
        bridge.clear();
        bridge.mediaSource(QStringLiteral("$gif"), QStringLiteral("full"));
        client.succeed(client.fetches.last().opId, QByteArray(4096, 'y'));
        bridge.cachedFullContentHash(QStringLiteral("$gif"));
        QCOMPARE(bridge.healthSnapshot()
                     .value(QStringLiteral("contentHashComputed")).toLongLong(),
                 qint64(3));

        // review L-c: the clear() assertion above is NOT mutation-detecting
        // on its own — "$gif" reached revision 2 before the clear and only
        // revision 1 after it, so the revision guard would have caught a
        // missing memo-clear anyway. This case isolates the memo clear
        // properly: a key inserted EXACTLY ONCE (revision 1), cleared, then
        // re-inserted with DIFFERENT bytes (revision 1 again, so the guard
        // cannot help). Drop m_contentHashCache.clear() from clear() and
        // this returns the stale 'p' digest for 'q' bytes.
        const QByteArray first(2048, 'p');
        const QByteArray second(2048, 'q');
        bridge.mediaSource(QStringLiteral("$once"), QStringLiteral("full"));
        client.succeed(client.fetches.last().opId, first);
        const QString firstHex =
            bridge.cachedFullContentHash(QStringLiteral("$once"));
        QCOMPARE(firstHex, QString::fromLatin1(QCryptographicHash::hash(
                     first, QCryptographicHash::Sha256).toHex()));

        bridge.clear();
        bridge.mediaSource(QStringLiteral("$once"), QStringLiteral("full"));
        client.succeed(client.fetches.last().opId, second);
        QCOMPARE(bridge.cachedFullContentHash(QStringLiteral("$once")),
                 QString::fromLatin1(QCryptographicHash::hash(
                     second, QCryptographicHash::Sha256).toHex()));
    }

    // v0.7 media round: request priority. The old single FIFO dispatched
    // strictly in arrival order, so a pressed-play track waited behind a
    // page of speculative full-GIF prefetches.
    void playbackClassJumpsQueueAheadOfSpeculative()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        for (int i = 0; i < 8; ++i)
            bridge.mediaSource(QStringLiteral("$m%1").arg(i),
                               QStringLiteral("thumb"));
        QCOMPARE(client.fetches.size(), 8);
        bridge.animatedSource(QStringLiteral("$gif1"));
        bridge.animatedSource(QStringLiteral("$gif2"));
        bridge.playableSource(QStringLiteral("$song"));
        QCOMPARE(bridge.queuedCountForTest(), 3);
        client.succeed(client.fetches.at(0).opId, QByteArray("img"));
        QCOMPARE(client.fetches.size(), 9);
        QCOMPARE(client.fetches.last().key, QStringLiteral("$song"));
        QCOMPARE(client.fetches.last().timeoutClass, 1);
    }

    // Heavy classes (full static media, speculative prefetch) never occupy
    // every slot: interactive chrome keeps reserved headroom, so eight
    // multi-megabyte GIF prefetches cannot make the room's avatars wait.
    void heavySlotsLeaveHeadroomForInteractive()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        for (int i = 0; i < 8; ++i)
            bridge.animatedSource(QStringLiteral("$gif%1").arg(i));
        QCOMPARE(client.fetches.size(), 6); // heavy cap
        QCOMPARE(bridge.queuedCountForTest(), 2);
        bridge.avatarSource(kMxc, 48);
        QCOMPARE(client.fetches.size(), 7); // reserved slot, no queueing
        QCOMPARE(client.fetches.last().key, kMxc);
        // A finished heavy fetch pumps the queued heavy work back in.
        client.succeed(client.fetches.at(0).opId,
                       QByteArray("GIF89a") + QByteArray(16, 'g'),
                       QStringLiteral("image/gif"));
        QCOMPARE(client.fetches.size(), 8);
    }

    // Bounded starvation: strict priority yields to the oldest entry once
    // it has waited past the guard, so speculative work is delayed, never
    // parked forever.
    void starvedSpeculativeEventuallyDispatches()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setStarvationMsForTest(0);
        for (int i = 0; i < 8; ++i)
            bridge.mediaSource(QStringLiteral("$m%1").arg(i),
                               QStringLiteral("thumb"));
        bridge.animatedSource(QStringLiteral("$gif"));
        bridge.mediaSource(QStringLiteral("$late"), QStringLiteral("thumb"));
        QCOMPARE(bridge.queuedCountForTest(), 2);
        client.succeed(client.fetches.at(0).opId, QByteArray("img"));
        // With the guard at 0 the OLDEST queued entry wins even though the
        // newer thumbnail has the higher priority.
        QCOMPARE(client.fetches.last().key, QStringLiteral("$gif"));
    }

    // A live player's materialized file is never deleted under it: pinned
    // entries are skipped by the LRU and become evictable again on unpin.
    void pinnedPlayableSurvivesEviction()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setPlayableCapsForTest(2, 64 * 1024 * 1024);
        // Above kLargeCacheSkipBytes so the payload lives ONLY as the
        // materialized file — a small payload would silently re-materialize
        // from the RAM cache and hide the eviction under test.
        const auto flac = [](char fill) {
            return QByteArray("fLaC") + QByteArray(9 * 1024 * 1024, fill);
        };
        bridge.playableSource(QStringLiteral("$a"));
        client.succeed(client.fetches.at(0).opId, flac('a'),
                       QStringLiteral("audio/flac"));
        const QString urlA = bridge.playableSource(QStringLiteral("$a"));
        QVERIFY(urlA.startsWith(QLatin1String("file://")));
        bridge.pinPlayable(QStringLiteral("$a"));

        bridge.playableSource(QStringLiteral("$b"));
        client.succeed(client.fetches.at(1).opId, flac('b'),
                       QStringLiteral("audio/flac"));
        bridge.playableSource(QStringLiteral("$c"));
        client.succeed(client.fetches.at(2).opId, flac('c'),
                       QStringLiteral("audio/flac"));

        // Cap 2 with three files: the unpinned $b was the victim, the
        // pinned $a survives on disk.
        QVERIFY(QFileInfo::exists(QUrl(urlA).toLocalFile()));
        QVERIFY(!bridge.playableSource(QStringLiteral("$a")).isEmpty());
        const int fetchesBefore = client.fetches.size();
        QVERIFY(bridge.playableSource(QStringLiteral("$b")).isEmpty());
        QCOMPARE(client.fetches.size(), fetchesBefore + 1); // re-dispatch

        // review L1: pins are REFCOUNTED — a second card pinning the same
        // event keeps the file protected after the first card resets.
        bridge.pinPlayable(QStringLiteral("$a"));   // second holder
        bridge.unpinPlayable(QStringLiteral("$a")); // first releases
        bridge.playableSource(QStringLiteral("$x1"));
        client.succeed(client.fetches.last().opId, flac('x'),
                       QStringLiteral("audio/flac"));
        bridge.playableSource(QStringLiteral("$x2"));
        client.succeed(client.fetches.last().opId, flac('y'),
                       QStringLiteral("audio/flac"));
        QVERIFY(QFileInfo::exists(QUrl(urlA).toLocalFile()));

        // Unpinned by the LAST holder, $a becomes an ordinary victim again
        // — further inserts walk the LRU past it.
        bridge.unpinPlayable(QStringLiteral("$a"));
        bridge.playableSource(QStringLiteral("$d"));
        client.succeed(client.fetches.last().opId, flac('d'),
                       QStringLiteral("audio/flac"));
        bridge.playableSource(QStringLiteral("$e"));
        client.succeed(client.fetches.last().opId, flac('e'),
                       QStringLiteral("audio/flac"));
        QVERIFY(!QFileInfo::exists(QUrl(urlA).toLocalFile()));
    }

    // The observed live failure: a "thumb" result can arrive labelled with
    // the parent video's MIME — and a homeserver that cannot thumbnail may
    // even answer with the original A/V payload. The bytes decide: A/V
    // containers never enter the image path; genuine image bytes pass
    // regardless of the mislabeled MIME; full-media fetches are untouched.
    void thumbPayloadSniffingRejectsAvContainers()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);

        bridge.mediaSource(QStringLiteral("$vid"), QStringLiteral("thumb"));
        QByteArray mp4 = QByteArray("\x00\x00\x00\x18", 4)
            + QByteArray("ftypisom") + QByteArray(16, '\0');
        client.succeed(client.fetches.at(0).opId, mp4,
                       QStringLiteral("video/mp4"));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("rejected"));
        QCOMPARE(cached.count(), 0);
        QVERIFY(bridge.cachedBytes(QStringLiteral("thumb:$vid")).isEmpty());

        bridge.retry(QStringLiteral("thumb:$vid"));
        bridge.mediaSource(QStringLiteral("$vid"), QStringLiteral("thumb"));
        QByteArray jpeg;
        jpeg.append('\xff');
        jpeg.append('\xd8');
        jpeg.append('\xff');
        jpeg.append(QByteArray(16, 'j'));
        client.succeed(client.fetches.at(1).opId, jpeg,
                       QStringLiteral("video/mp4"));
        QCOMPARE(cached.count(), 1);
        QVERIFY(!bridge.cachedBytes(QStringLiteral("thumb:$vid")).isEmpty());

        bridge.mediaSource(QStringLiteral("$file"), QStringLiteral("full"));
        client.succeed(client.fetches.at(2).opId, mp4,
                       QStringLiteral("video/mp4"));
        QCOMPARE(cached.count(), 2);
    }

    // Room switch drops QUEUED speculative prefetches (their delegates are
    // gone); queued interactive work and in-flight ops are untouched.
    void droppedSpeculativeQueueEntriesNeverDispatch()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        for (int i = 0; i < 8; ++i)
            bridge.mediaSource(QStringLiteral("$m%1").arg(i),
                               QStringLiteral("thumb"));
        bridge.animatedSource(QStringLiteral("$gif"));
        bridge.mediaSource(QStringLiteral("$keep"), QStringLiteral("thumb"));
        QCOMPARE(bridge.queuedCountForTest(), 2);
        bridge.dropQueuedSpeculative();
        QCOMPARE(bridge.queuedCountForTest(), 1);
        client.succeed(client.fetches.at(0).opId, QByteArray("img"));
        QCOMPARE(client.fetches.last().key, QStringLiteral("$keep"));
        client.succeed(client.fetches.at(1).opId, QByteArray("img"));
        QCOMPARE(client.fetches.size(), 9); // 8 + $keep; the GIF never ran
    }

    // ── v0.7 perf round: cancellation + bounded playable prefetch ──

    // Cancelling an in-flight playable fetch aborts the backend op, frees
    // the slot, and leaves NO failure mark — a fresh Play must re-dispatch
    // immediately.
    void cancelPlayableAbortsBackendAndFreesSlot()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.playableSource(QStringLiteral("$video"));
        QCOMPARE(bridge.inflightCountForTest(), 1);
        const quint64 opId = client.fetches.first().opId;
        bridge.cancelPlayable(QStringLiteral("$video"));
        QCOMPARE(bridge.inflightCountForTest(), 0);
        QCOMPARE(client.cancels, QList<quint64>{opId});
        QVERIFY(bridge.failureCategory(QStringLiteral("full:$video")).isEmpty());
        // A fresh Play dispatches again at the playable class.
        bridge.playableSource(QStringLiteral("$video"));
        QCOMPARE(client.fetches.size(), 2);
        QCOMPARE(client.fetches.last().timeoutClass, 1);
    }

    // A late completion for a cancelled op is stale — it must not populate
    // the cache or emit playableMediaReady.
    void lateCompletionAfterCancelIsDropped()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        bridge.playableSource(QStringLiteral("$video"));
        const quint64 opId = client.fetches.first().opId;
        bridge.cancelPlayable(QStringLiteral("$video"));
        client.succeed(opId, QByteArray("GIF89a-not-really"),
                       QStringLiteral("video/mp4"));
        QCOMPARE(ready.count(), 0);
        QVERIFY(bridge.cachedSource(QStringLiteral("full:$video")).isEmpty());
    }

    // Another interest class on the same bytes keeps the fetch alive: a
    // GIF row wants the payload too, so cancel must not abort the op.
    void cancelKeepsFetchAliveForOtherConsumers()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.animatedSource(QStringLiteral("$shared"));
        bridge.playableSource(QStringLiteral("$shared")); // coalesces
        QCOMPARE(client.fetches.size(), 1);
        bridge.cancelPlayable(QStringLiteral("$shared"));
        QCOMPARE(client.cancels.size(), 0);
        QCOMPARE(bridge.inflightCountForTest(), 1);
    }

    // Prefetch is bounded: declared in-cap sizes dispatch speculatively at
    // the playable timeout class; unknown or over-cap sizes never dispatch.
    void prefetchPlayableHonorsSizeCap()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.prefetchPlayable(QStringLiteral("$big"),
                                512.0 * 1024 * 1024);
        bridge.prefetchPlayable(QStringLiteral("$unknown"), 0);
        QCOMPARE(client.fetches.size(), 0);
        bridge.prefetchPlayable(QStringLiteral("$small"), 4 * 1024 * 1024);
        QCOMPARE(client.fetches.size(), 1);
        QCOMPARE(client.fetches.first().timeoutClass, 1);
        // A prefetched payload materializes and signals playableMediaReady
        // so a waiting card can start instantly.
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        QByteArray mp4(1024, 'x');
        mp4.replace(4, 4, "ftyp");
        client.succeed(client.fetches.first().opId, mp4,
                       QStringLiteral("video/mp4"));
        QCOMPARE(ready.count(), 1);
        // Play now serves the materialized file with no new fetch.
        QVERIFY(!bridge.playableSource(QStringLiteral("$small")).isEmpty());
        QCOMPARE(client.fetches.size(), 1);
    }

    // Queued prefetches are speculative: a room switch drops them exactly
    // like GIF autoplay prefetches.
    void queuedPrefetchDroppedOnRoomSwitch()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        for (int i = 0; i < 8; ++i)
            bridge.mediaSource(QStringLiteral("$m%1").arg(i),
                               QStringLiteral("thumb"));
        bridge.prefetchPlayable(QStringLiteral("$spec"), 1024 * 1024);
        QCOMPARE(bridge.queuedCountForTest(), 1);
        bridge.dropQueuedSpeculative();
        QCOMPARE(bridge.queuedCountForTest(), 0);
    }

    // review M1: playable interest is refcounted — two cards on the same
    // media (main timeline + thread panel) share one fetch, and the first
    // card's cancel must not strand the second.
    void cancelWithTwoPlayableConsumersKeepsFetch()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.playableSource(QStringLiteral("$dual")); // card A
        bridge.playableSource(QStringLiteral("$dual")); // card B coalesces
        QCOMPARE(client.fetches.size(), 1);
        bridge.cancelPlayable(QStringLiteral("$dual")); // card A leaves
        QCOMPARE(client.cancels.size(), 0);
        QCOMPARE(bridge.inflightCountForTest(), 1);
        bridge.cancelPlayable(QStringLiteral("$dual")); // card B leaves too
        QCOMPARE(client.cancels.size(), 1);
        QCOMPARE(bridge.inflightCountForTest(), 0);
    }

    // review H1: a poster hook left behind by an over-cap video (whose
    // prefetch declined) must not veto a later user cancel.
    void posterHookForOverCapVideoDoesNotBlockCancel()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        // No Matrix thumbnail, 500 MB declared: the poster path declines
        // the prefetch and must not leak its hook.
        bridge.videoPosterSource(QStringLiteral("$huge"),
                                 500.0 * 1024 * 1024);
        QCOMPARE(client.fetches.size(), 0);
        // User presses Play, then closes the card mid-download.
        bridge.playableSource(QStringLiteral("$huge"));
        QCOMPARE(bridge.inflightCountForTest(), 1);
        const quint64 opId = client.fetches.first().opId;
        bridge.cancelPlayable(QStringLiteral("$huge"));
        QCOMPARE(client.cancels, QList<quint64>{opId});
        QCOMPARE(bridge.inflightCountForTest(), 0);
    }

    // review M3: a terminal failure voids every interest class for the key
    // — a later cancel finds nothing to veto it, and the sets stay bounded.
    void terminalFailureClearsInterestSets()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.prefetchPlayable(QStringLiteral("$flaky"), 1024 * 1024);
        QCOMPARE(client.fetches.size(), 1);
        client.fail(client.fetches.first().opId, QStringLiteral("network"));
        // A pressed-play fetch for the same key after the transient mark
        // clears must dispatch and be cancellable.
        bridge.retry(QStringLiteral("full:$flaky"));
        bridge.playableSource(QStringLiteral("$flaky"));
        QCOMPARE(client.fetches.size(), 2);
        bridge.cancelPlayable(QStringLiteral("$flaky"));
        QCOMPARE(client.cancels.size(), 1);
        QCOMPARE(bridge.inflightCountForTest(), 0);
    }

    // review-recheck LOW: a playableSource call served synchronously from
    // the RAM cache holds no fetch, so it must not leave an interest count
    // behind — a phantom +1 would silently veto a later cancel of a REAL
    // fetch for the same key.
    void ramCacheHitLeavesNoPhantomInterest()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setPlayableCapsForTest(1, 1024 * 1024); // 1 materialized file
        QByteArray mp4(256, 'x');
        mp4.replace(4, 4, "ftyp");
        // Fetch $track once (RAM-cached + materialized).
        bridge.playableSource(QStringLiteral("$track"));
        client.succeed(client.fetches.at(0).opId, mp4,
                       QStringLiteral("video/mp4"));
        // Evict $track's FILE with $other (file cap is 1)...
        bridge.playableSource(QStringLiteral("$other"));
        client.succeed(client.fetches.at(1).opId, mp4,
                       QStringLiteral("video/mp4"));
        // ...then serve $track from RAM bytes — the synchronous cache-hit
        // path this regression is about. It must not retain interest.
        QVERIFY(!bridge.playableSource(QStringLiteral("$track")).isEmpty());
        // Drop the RAM copies (limit forces eviction on the next insert)…
        bridge.setCacheLimitBytes(1);
        bridge.mediaSource(QStringLiteral("$bump"), QStringLiteral("full"));
        client.succeed(client.fetches.at(2).opId, QByteArray("img"));
        // …and $track's file again (via $other, itself now a real fetch).
        bridge.playableSource(QStringLiteral("$other"));
        client.succeed(client.fetches.at(3).opId, mp4,
                       QStringLiteral("video/mp4"));
        // A REAL fetch for $track now carries exactly one press-play
        // interest; a phantom +1 from the earlier cache hit would make
        // this cancel a silent no-op.
        bridge.playableSource(QStringLiteral("$track"));
        QCOMPARE(bridge.inflightCountForTest(), 1);
        bridge.cancelPlayable(QStringLiteral("$track"));
        QCOMPARE(client.cancels.size(), 1);
        QCOMPARE(bridge.inflightCountForTest(), 0);
    }

    // cancelPlayable with no playable interest registered is a no-op (an
    // unrelated ordinary fetch for the same key must survive).
    void cancelWithoutPlayableInterestIsNoop()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.mediaSource(QStringLiteral("$img"), QStringLiteral("full"));
        QCOMPARE(bridge.inflightCountForTest(), 1);
        bridge.cancelPlayable(QStringLiteral("$img"));
        QCOMPARE(bridge.inflightCountForTest(), 1);
        QCOMPARE(client.cancels.size(), 0);
    }

    // v0.7.2 PERF — this is why scrolling up through a media-heavy room
    // spiked. QML's documented idiom for "scale to this width and keep the
    // aspect" is to set sourceSize.width and leave the height 0, which is
    // exactly what every timeline image asks for. But QSize::isEmpty() is
    // true when EITHER axis is below 1, so the provider's
    // `isValid() && !isEmpty()` guard rejected that request as "no size
    // asked for" and decoded at the source's FULL resolution — a 2400x1600
    // screenshot became 15 MB of pixels for a 348px-wide box, dozens of
    // times per gesture.
    void aWidthOnlySourceSizeBoundsTheDecode()
    {
        FakeClient client;
        // NOT MediaBridge(&client): that ctor parameter is the QObject
        // PARENT, so it compiles and silently leaves the client unset.
        MediaBridge bridge;
        bridge.setClient(&client);
        MediaImageProvider provider(&bridge);

        QImage big(2400, 1600, QImage::Format_RGB32);
        big.fill(Qt::blue);
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(big.save(&buffer, "PNG"));
        buffer.close();

        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);
        bridge.mediaSource(QStringLiteral("$ev"), QStringLiteral("full"));
        QVERIFY(!client.fetches.isEmpty());
        client.succeed(client.fetches.first().opId, png);
        QCOMPARE(cached.count(), 1);
        const QString id = cached.first().at(0).toString();

        QSize reported;
        const QImage bounded = provider.requestImage(id, &reported,
                                                     QSize(640, 0));
        QVERIFY2(!bounded.isNull(), "the bounded decode produced nothing");
        QCOMPARE(bounded.width(), 640);
        // 1600 * 640 / 2400, aspect preserved.
        QCOMPARE(bounded.height(), 427);
    }

    // Asking for nothing still decodes naturally — the save path and the
    // full-size viewer depend on that.
    void noSourceSizeDecodesNaturally()
    {
        FakeClient client;
        // NOT MediaBridge(&client): that ctor parameter is the QObject
        // PARENT, so it compiles and silently leaves the client unset.
        MediaBridge bridge;
        bridge.setClient(&client);
        MediaImageProvider provider(&bridge);

        QImage big(1200, 900, QImage::Format_RGB32);
        big.fill(Qt::red);
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(big.save(&buffer, "PNG"));
        buffer.close();

        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);
        bridge.mediaSource(QStringLiteral("$ev2"), QStringLiteral("full"));
        QVERIFY(!client.fetches.isEmpty());
        client.succeed(client.fetches.first().opId, png);
        QCOMPARE(cached.count(), 1);
        const QString id = cached.first().at(0).toString();

        QSize reported;
        QCOMPARE(provider.requestImage(id, &reported, QSize()).size(),
                 QSize(1200, 900));
        // And a request LARGER than the source must not inflate it: a small
        // image in a big box is scaled by the scene graph, not in memory.
        QCOMPARE(provider.requestImage(id, &reported, QSize(4000, 0)).size(),
                 QSize(1200, 900));
        // A height-only request is the same idiom on the other axis.
        QCOMPARE(provider.requestImage(id, &reported, QSize(0, 300)).size(),
                 QSize(400, 300));
    }

    // The never-upscale rule must NOT apply when a shape is baked into the
    // bitmap. An avatar mask is rasterized once, at whatever size it is baked
    // at, so refusing the upscale would bake a circle at the source's size
    // and its edge would visibly alias when shown larger. Plain images are
    // the opposite case (above) — this asymmetry is deliberate.
    void aBakedShapeStillHonoursAnUpscale()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        MediaImageProvider provider(&bridge);

        QImage small(100, 100, QImage::Format_RGB32);
        small.fill(Qt::green);
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(small.save(&buffer, "PNG"));
        buffer.close();

        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);
        bridge.mediaSource(QStringLiteral("$av"), QStringLiteral("full"));
        QVERIFY(!client.fetches.isEmpty());
        client.succeed(client.fetches.first().opId, png);
        QCOMPARE(cached.count(), 1);
        const QString id = cached.first().at(0).toString();

        QSize reported;
        const QImage masked = provider.requestImage(
            id + QStringLiteral("|shape:circle"), &reported, QSize(224, 224));
        QCOMPARE(masked.size(), QSize(224, 224));
        // Without the shape the same request decodes at the source size.
        QCOMPARE(provider.requestImage(id, &reported, QSize(224, 224)).size(),
                 QSize(100, 100));
    }
};

QTEST_GUILESS_MAIN(MediaBridgeTest)
#include "MediaBridgeTest.moc"
