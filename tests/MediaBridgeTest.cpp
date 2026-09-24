// MediaBridge, the shared avatar/media request layer: request dedup, bounded
// LRU eviction, stale-result rejection after sign-out, failure marking and
// retry, bounded concurrency and queue pumping, one canonical fetch edge per
// avatar identity with its own cache budget, transient-failure expiry via
// mediaRetryable, and revision-suffixed provider URLs.

#include "matrix/MatrixClient.h"
#include "media/MediaBridge.h"
#include "media/MediaImageProvider.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QDir>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

// Log capture for cacheHitTraceIsSilentByDefault (a QtMessageHandler is a
// bare function pointer, so the sink is file-scope). qCDebug checks the
// category before calling the handler, so a default-off category never
// reaches it.
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
        int timeoutClass = 0; // 0 standard / 1 playable / 2 save
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

    // Playable payloads are written on PlayableFileWriter's worker thread,
    // so wait for playableMediaReady. Polling playableSource() instead would
    // register phantom playable interests as a side effect.
    static bool waitForPlayableCount(QSignalSpy &spy, int expected)
    {
        constexpr int timeoutMs = 5000;
        QElapsedTimer clock;
        clock.start();
        while (spy.count() < expected && clock.elapsed() < timeoutMs)
            spy.wait(25);
        return spy.count() == expected;
    }

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

    // Every requested avatar size maps onto one canonical fetch edge: one
    // request, one cache entry, one failure mark per identity.
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

    // Per-call cache hits are not logged by default: mediaSource() and
    // avatarSource() run from QML bindings on every delegate rebind, so a
    // hit log floods the journal while scrolling.
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
        // Cache hit: the hot path bindings re-run while scrolling.
        const QString hit = bridge.avatarSource(kMxc, 64);
        // Cache miss on a fresh identity.
        bridge.avatarSource(QStringLiteral("mxc://mock.local/second"), 64);
        qInstallMessageHandler(prev);

        QVERIFY(hit.startsWith(QStringLiteral("image://lightning-media/")));
        // No per-request line reaches the default category (they are on
        // lightning.media.trace); only a counts-only burst summary and
        // failures land there.
        for (const QString &line : captured) {
            QVERIFY2(!line.contains(QStringLiteral("cache=hit")),
                     qUtf8Printable("unexpected cache=hit storm line: " + line));
            QVERIFY2(!line.contains(QStringLiteral("cache=miss")),
                     qUtf8Printable("a per-request line is back on the default "
                                    "category: " + line));
            QVERIFY2(!line.contains(QStringLiteral("already-pending")),
                     qUtf8Printable("the per-CALLER line is back, and it is "
                                    "unbounded in a list: " + line));
        }
    }

    // A burst of media work is summarised in one line once it goes quiet:
    // counts and bytes only, no keys, mxc URIs or paths, so it is safe to
    // paste into a bug report.
    void aBurstOfMediaWorkIsSummarisedOnceItGoesQuiet()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        bridge.avatarSource(kMxc, 64);
        bridge.avatarSource(QStringLiteral("mxc://mock.local/second"), 64);

        auto &captured = capturedLines();
        captured.clear();
        QtMessageHandler prev = qInstallMessageHandler(&captureHandler);
        for (const auto &fetch : client.fetches)
            client.succeed(fetch.opId, QByteArray("pixels"));
        // The summary is deferred until activity stops, so it covers the
        // whole burst.
        bool sawSummary = false;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            for (const QString &line : captured) {
                if (line.contains(QStringLiteral("media burst:")))
                    sawSummary = true;
            }
            return sawSummary;
        }()), 5000);
        qInstallMessageHandler(prev);

        for (const QString &line : captured) {
            if (!line.contains(QStringLiteral("media burst:")))
                continue;
            QVERIFY2(!line.contains(QStringLiteral("mxc://")),
                     qUtf8Printable("the summary names a media URI: " + line));
            QVERIFY2(line.contains(QStringLiteral("fetched")),
                     qUtf8Printable("the summary carries no count: " + line));
        }
        QVERIFY(sawSummary);
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

    // Avatar-class entries ("mxc:" keys) evict only against their own
    // reserved budget.
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

    // Churning timeline media through the main budget never evicts a cached
    // avatar.
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

        // The previous account's bytes complete after sign-out: they neither
        // enter the cache nor reach the new session.
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

        // Repolling a failed source does not re-dispatch, and the empty
        // answer still reports its category synchronously.
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

    // A dispatch rejected with opId==0 (session restoring or switching) is the
    // transient "unavailable" category with the normal retry window, not a
    // permanent mark.
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

        // The backend becomes usable; the sweep announces the expiry and the
        // next request dispatches.
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

    // The watchdog sweeps expired transient marks and emits mediaRetryable,
    // so an idle UI recovers; permanent validation marks are never swept.
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

    // Provider URLs carry a per-key revision bumped only on an actual byte
    // insert: cache hits keep the identical string, and a re-fetch after
    // eviction yields a new one so an Image stuck in Error reloads.
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

        // The key was served, then LRU-evicted before the provider read.
        bridge.avatarSource(QStringLiteral("mxc://example.org/avatar2"), 64);
        client.succeed(client.fetches.at(1).opId, QByteArray(8, 'b'));
        QCOMPARE(bridge.avatarSource(kMxc, 64), QString()); // miss → dispatch
        QCOMPARE(client.fetches.size(), 3);
        client.succeed(client.fetches.at(2).opId, QByteArray(6, 'a'));
        const QString url2 = bridge.avatarSource(kMxc, 64);
        QVERIFY(!url2.isEmpty());
        QVERIFY2(url2 != url1, "re-cached bytes must yield a NEW source string");
    }

    // An op the backend never completes does not hold its concurrency slot
    // forever; the watchdog reclaims it.
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

        // A fresh request cannot dispatch while the slots are held.
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

        // A timeout mark is transient: the next poll re-dispatches once.
        bridge.avatarSource(kMxc, 64);
        QCOMPARE(client.fetches.size(), 2);
    }

    // Under sustained saturation with mixed outcomes, every terminal path
    // releases its slot, the queue drains to zero, and memory stays bounded.
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

        // Resolve ops in order with a deterministic mix; each completion pumps
        // one queued request, so there is exactly one op per distinct key.
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

        // A new request after the storm still dispatches immediately.
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

    // Animation is decided by the bytes, not the declared mimetype: a sticker
    // may omit `mimetype` (MSC2545), and an unlabelled animated GIF must still
    // materialize.
    void animationIsDecidedByTheBytesNotTheDeclaredMimetype()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        QCOMPARE(bridge.animatedSource(QStringLiteral("$sticker")), QString());
        QByteArray gif("GIF89a");
        gif.append(QByteArray(64, '\0'));
        // Delivered with no mimetype, as an unlabelled sticker arrives.
        client.succeed(client.fetches.first().opId, gif, QString());
        QCOMPARE(ready.count(), 1);
        QCOMPARE(failed.count(), 0);
        QVERIFY(bridge.animatedSource(QStringLiteral("$sticker"))
                    .startsWith(QStringLiteral("file://")));
    }

    // The magic bytes are authoritative: a payload labelled image/gif whose
    // bytes are something else is still refused.
    void aLabelSayingGifCannotMakeNonGifBytesAnimatable()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        bridge.animatedSource(QStringLiteral("$png"));
        QByteArray png("\x89PNG\r\n\x1a\n", 8);
        png.append(QByteArray(64, '\0'));
        client.succeed(client.fetches.last().opId, png,
                       QStringLiteral("image/gif"));
        QCOMPARE(ready.count(), 0);
    }

    // A speculative animation ask (a sticker cannot know whether it is
    // animated) is answered with silence, not mediaFetchFailed, which would
    // replace a good still image with a retry card. A caller that does know
    // still gets its terminal answer.
    void aSpeculativeAnimationAskIsAnsweredWithSilenceNotAFailure()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);
        bridge.animatedSource(QStringLiteral("$still"), /*speculative=*/true);
        QByteArray png("\x89PNG\r\n\x1a\n", 8);
        png.append(QByteArray(64, '\0'));
        client.succeed(client.fetches.last().opId, png, QString());
        QCOMPARE(ready.count(), 0);
        QCOMPARE(failed.count(), 0);
        // The bytes still reached the ordinary image path.
        QCOMPARE(cached.count(), 1);
        QCOMPARE(bridge.failureCategory(QStringLiteral("full:$still")), QString());
    }

    // One demanding asker among speculative ones still gets its answer: the
    // obligation is the OR of the claimants', as for playable writes.
    void oneDemandingAskerRestoresTheFailureObligation()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        bridge.animatedSource(QStringLiteral("$mixed"), /*speculative=*/true);
        bridge.animatedSource(QStringLiteral("$mixed"), /*speculative=*/false);
        QByteArray png("\x89PNG\r\n\x1a\n", 8);
        png.append(QByteArray(64, '\0'));
        client.succeed(client.fetches.last().opId, png, QString());
        QCOMPARE(failed.count(), 1);
    }

    // Animated formats by magic bytes alone: GIF and animated WebP (Qt's webp
    // handler animates). Still WebP is excluded, and APNG too, since Qt's PNG
    // handler does not animate.
    void animatedExtensionIsMagicOnlyAndNarrow()
    {
        const auto ext = [](const QByteArray &b) {
            return MediaBridge::animatedExtensionFor(b);
        };
        QByteArray gif("GIF89a");
        gif.append(QByteArray(64, '\0'));
        QCOMPARE(ext(gif), QStringLiteral("gif"));
        QByteArray gif87("GIF87a");
        gif87.append(QByteArray(64, '\0'));
        QCOMPARE(ext(gif87), QStringLiteral("gif"));

        // RIFF....WEBPVP8X<size>, then the flags byte at offset 20.
        const auto webp = [](char flags, const char *fourcc) {
            QByteArray b("RIFF", 4);
            b.append(QByteArray(4, '\0'));
            b.append("WEBP", 4);
            b.append(fourcc, 4);
            b.append(QByteArray(4, '\0'));
            b.append(flags);
            b.append(QByteArray(32, '\0'));
            return b;
        };
        QCOMPARE(ext(webp('\x02', "VP8X")), QStringLiteral("webp"));
        // Alpha, no animation bit.
        QVERIFY(ext(webp('\x10', "VP8X")).isEmpty());
        // A plain lossy still has no VP8X chunk at all.
        QVERIFY(ext(webp('\x00', "VP8 ")).isEmpty());

        QByteArray png("\x89PNG\r\n\x1a\n", 8);
        png.append(QByteArray(64, '\0'));
        QVERIFY(ext(png).isEmpty());
        // SVG must never reach a media rendering path.
        QVERIFY(ext(QByteArrayLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\"/>"))
                    .isEmpty());
        QVERIFY(ext(QByteArrayLiteral("GIF")).isEmpty());
        QVERIFY(ext(QByteArray()).isEmpty());
    }

    // Sticker picker tiles ask for the original bytes (both edges 0), not a
    // server thumbnail, which is one frame by construction:
    // `media_fetch_mxc` selects MediaFormat::File only when an edge is 0.

    void stickerPickerAsksForTheOriginalBytesNotAServerThumbnail()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        QCOMPARE(bridge.mxcAnimatedSource(kMxc), QString());
        QCOMPARE(client.fetches.size(), 1);
        const FakeClient::Fetch fetch = client.fetches.first();
        QCOMPARE(fetch.key, kMxc);
        // Zero on both edges: a single non-zero edge is still a thumbnail
        // request.
        QCOMPARE(fetch.width, 0);
        QCOMPARE(fetch.height, 0);

        QByteArray gif("GIF89a");
        gif.append(QByteArray(64, '\0'));
        client.succeed(fetch.opId, gif, QString()); // unlabelled, as MSC2545 allows
        QCOMPARE(ready.count(), 1);
        QCOMPARE(ready.first().first().toString(),
                 QStringLiteral("mxcanim:") + kMxc);
        const QString source = bridge.mxcAnimatedSource(kMxc);
        QVERIFY(source.startsWith(QStringLiteral("file://")));
        const QString path = QUrl(source).toLocalFile();
        QVERIFY(QFileInfo::exists(path));
        // Materialised bytes are 0600, like every payload from an encrypted
        // room.
        QCOMPARE(QFileInfo(path).permissions()
                     & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
                        | QFileDevice::ReadOther | QFileDevice::WriteOther),
                 QFileDevice::Permissions());
        client.logout();
        QVERIFY(!QFileInfo::exists(path));
        // Not an mxc URI: refused without a fetch (Rust also sanitises pack
        // entries).
        QVERIFY(bridge.mxcAnimatedSource(QStringLiteral("https://evil.example/x.gif"))
                    .isEmpty());
    }

    // The still tile (a scaled thumbnail) and the animation (the original)
    // are different bytes for one mxc and use separate cache keys.
    void theStillTileAndTheAnimatedPickerTileAreSeparateFetches()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        bridge.mxcImageSource(kMxc, 160);
        bridge.mxcAnimatedSource(kMxc);
        QCOMPARE(client.fetches.size(), 2);
        QCOMPARE(client.fetches.at(0).width, 160); // still: a thumbnail
        QCOMPARE(client.fetches.at(1).width, 0);   // animation: the original

        // The still PNG thumbnail answers first; it must neither satisfy nor
        // fail the animated ask.
        QByteArray png("\x89PNG\r\n\x1a\n", 8);
        png.append(QByteArray(64, '\0'));
        client.succeed(client.fetches.at(0).opId, png, QStringLiteral("image/png"));
        QCOMPARE(ready.count(), 0);
        QCOMPARE(bridge.mxcAnimatedSource(kMxc), QString());

        QByteArray gif("GIF89a");
        gif.append(QByteArray(64, '\0'));
        client.succeed(client.fetches.at(1).opId, gif, QString());
        QCOMPARE(ready.count(), 1);
        QVERIFY(bridge.mxcAnimatedSource(kMxc).startsWith(QStringLiteral("file://")));
    }

    // A non-animated picker tile is answered with silence, not a failure: a
    // pack's mimetype is optional and attacker-writable, and the still tile is
    // already drawing these bytes.
    void aNonAnimatedPickerTileIsAnsweredWithSilenceNotAFailure()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        bridge.mxcAnimatedSource(kMxc);
        QByteArray png("\x89PNG\r\n\x1a\n", 8);
        png.append(QByteArray(64, '\0'));
        client.succeed(client.fetches.last().opId, png, QString());
        QCOMPARE(ready.count(), 0);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(bridge.failureCategory(QStringLiteral("mxcanim:") + kMxc),
                 QString());
    }

    // A picker tile refuses SVG and gzip (SVGZ) by bytes before any decode;
    // the declared type is attacker-controlled room state and may be absent.
    void aPickerTileRefusesMarkupAndGzipBeforeAnyDecode()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);

        bridge.mxcAnimatedSource(kMxc);
        client.succeed(client.fetches.last().opId,
                       QByteArrayLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\"/>"),
                       QStringLiteral("image/gif")); // lying label
        QCOMPARE(ready.count(), 0);
        QCOMPARE(cached.count(), 0); // never entered the image cache either
        QVERIFY(bridge.mxcAnimatedSource(kMxc).isEmpty());

        const QString svgz = QStringLiteral("mxc://example.org/svgz");
        bridge.mxcAnimatedSource(svgz);
        QByteArray gz("\x1F\x8B\x08", 3);
        gz.append(QByteArray(64, '\0'));
        client.succeed(client.fetches.last().opId, gz, QString());
        QCOMPARE(ready.count(), 0);
        QCOMPARE(cached.count(), 0);
    }

    // A picker tile is an image request (kind 2), so the A/V refusal applies
    // and an MP4 answer never enters the image cache or registers as timeline
    // A/V (kind 0 would emit playableSizeLearned with an mxc URI).
    void anAvContainerNeverEntersThePickerImagePath()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);
        QSignalSpy learned(&bridge, &MediaBridge::playableSizeLearned);
        bridge.mxcAnimatedSource(kMxc);
        QByteArray mp4(4, '\0');
        mp4.append("ftypisom", 8);
        mp4.append(QByteArray(32, '\0'));
        client.succeed(client.fetches.last().opId, mp4, QStringLiteral("image/gif"));
        QCOMPARE(ready.count(), 0);
        QCOMPARE(cached.count(), 0);
        QCOMPARE(learned.count(), 0);
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

    // Playable (video/audio) materialization.

    // Container sniffing is fail-closed: only known audio/video magic passes,
    // mislabelled bytes never materialize, and raw ADTS needs an AAC claim.
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

    // A validated playable payload is materialized as a session temp file
    // (correct suffix, restrictive permissions, unguessable name) and
    // reported via playableMediaReady; invalid payloads fail closed as
    // "rejected".
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
        QVERIFY(waitForPlayableCount(ready, 1));
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
        // The refusal is synchronous: an unknown container is rejected before
        // any file or worker job exists.
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 0);
        QTest::qWait(50);
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

    // Large playable payloads bypass the RAM LRU (one video must not evict the
    // image cache); small ones still cache.
    void largePlayablePayloadSkipsRamCache()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);

        QCOMPARE(bridge.playableSource(QStringLiteral("$big")), QString());
        QByteArray big = QByteArrayLiteral("OggS");
        big.resize(9 * 1024 * 1024, '\2'); // > 8 MiB skip threshold
        client.succeed(client.fetches.at(0).opId, big,
                       QStringLiteral("audio/ogg"));
        QVERIFY(waitForPlayableCount(ready, 1));
        QVERIFY(bridge.cacheBytesUsed() < 1024 * 1024);
        QVERIFY(!bridge.playableSource(QStringLiteral("$big")).isEmpty());

        QCOMPARE(bridge.playableSource(QStringLiteral("$small")), QString());
        QByteArray small = QByteArrayLiteral("OggS");
        small.resize(4096, '\3');
        client.succeed(client.fetches.at(1).opId, small,
                       QStringLiteral("audio/ogg"));
        QVERIFY(waitForPlayableCount(ready, 2));
        QVERIFY(bridge.cacheBytesUsed() >= small.size());
    }

    // A fetched playable payload is written on the worker thread, not by the
    // call that receives it.
    void aFetchedPayloadIsNotWrittenByTheThreadThatReceivesIt()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        bridge.playableSource(QStringLiteral("$vid"));
        QByteArray ogg = QByteArrayLiteral("OggS");
        ogg.resize(2 * 1024 * 1024, '\1');
        client.succeed(client.fetches.at(0).opId, ogg,
                       QStringLiteral("audio/ogg"));
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 1);
        QCOMPARE(ready.count(), 0);
        // A card asking again meanwhile is coalesced, not re-fetched.
        QVERIFY(bridge.playableSource(QStringLiteral("$vid")).isEmpty());
        QCOMPARE(client.fetches.size(), 1);
        QVERIFY(waitForPlayableCount(ready, 1));
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 0);
        QVERIFY(!bridge.playableSource(QStringLiteral("$vid")).isEmpty());
    }

    // A speculative prefetch and a pressed-play card on the same key share
    // one write, and its single broadcast services both.
    void twoClaimantsShareOneWriteAndBothAreServiced()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        bridge.prefetchPlayable(QStringLiteral("$dual"), 2 * 1024 * 1024);
        QCOMPARE(client.fetches.size(), 1);
        QByteArray mp4(2 * 1024 * 1024, 'x');
        mp4.replace(4, 4, "ftyp");
        client.succeed(client.fetches.at(0).opId, mp4,
                       QStringLiteral("video/mp4"));
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 1);
        // Play is pressed while the prefetch's write is still running.
        QVERIFY(bridge.playableSource(QStringLiteral("$dual")).isEmpty());
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 1); // still ONE
        QCOMPARE(client.fetches.size(), 1);                 // and ONE fetch
        QVERIFY(waitForPlayableCount(ready, 1));
        QTest::qWait(50);
        QCOMPARE(ready.count(), 1); // one broadcast, not one per claimant
        QVERIFY(!bridge.playableSource(QStringLiteral("$dual")).isEmpty());
    }

    // A write started before sign-out publishes nothing: clear() empties the
    // tracking hash (the authority, since the completion is a queued call),
    // bumps the generation and cancels the job.
    void aWriteStartedBeforeSignOutPublishesNothing()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        bridge.playableSource(QStringLiteral("$vid"));
        QByteArray ogg = QByteArrayLiteral("OggS");
        ogg.resize(8 * 1024 * 1024, '\1');
        client.succeed(client.fetches.at(0).opId, ogg,
                       QStringLiteral("audio/ogg"));
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 1);
        bridge.clear(); // sign-out / account switch
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 0);
        QTest::qWait(300); // a completion in flight would land in here
        QCOMPARE(ready.count(), 0);
        // The next session inherits nothing and re-fetches.
        QVERIFY(bridge.playableSource(QStringLiteral("$vid")).isEmpty());
        QCOMPARE(client.fetches.size(), 2);
    }

    // A card closed mid-materialization abandons the write: nothing is
    // published or left under the final name, and a fresh Play re-fetches.
    void cancelDuringAWriteAbandonsIt()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        bridge.playableSource(QStringLiteral("$vid"));
        // Above kLargeCacheSkipBytes, so no RAM copy remains and a re-ask
        // needs a real fetch.
        QByteArray ogg = QByteArrayLiteral("OggS");
        ogg.resize(9 * 1024 * 1024, '\1');
        client.succeed(client.fetches.at(0).opId, ogg,
                       QStringLiteral("audio/ogg"));
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 1);
        bridge.cancelPlayable(QStringLiteral("$vid"));
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 0);
        QTest::qWait(300);
        QCOMPARE(ready.count(), 0);
        QVERIFY(bridge.playableSource(QStringLiteral("$vid")).isEmpty());
        QCOMPARE(client.fetches.size(), 2);
    }

    // The size bound and container sniff fail closed before any byte reaches
    // the worker thread.
    void anOverBoundPayloadIsRefusedBeforeAnyWriteStarts()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setPlayableCapsForTest(4, 1024); // 1 KiB per-file bound
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        bridge.playableSource(QStringLiteral("$vid"));
        QByteArray ogg = QByteArrayLiteral("OggS");
        ogg.resize(4096, '\1');
        client.succeed(client.fetches.at(0).opId, ogg,
                       QStringLiteral("audio/ogg"));
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 0);
        QTest::qWait(50);
        QCOMPARE(ready.count(), 0);
        bool sawRejected = false;
        for (const auto &args : failed) {
            if (args.at(0).toString() == QStringLiteral("full:$vid")
                && args.at(1).toString() == QStringLiteral("rejected"))
                sawRejected = true;
        }
        QVERIFY(sawRejected);
    }

    // Each dispatch carries its backend timeout class, so the Rust bound stays
    // below the covering C++ watchdog deadline.
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

    // A duplicated terminal for one op releases one slot and emits one
    // mediaCached; the duplicate counts as stale.
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

    // A completion landing after the watchdog fired is discarded: no duplicate
    // signal, no cache entry, and the pipeline stays drained.
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

    // fetchFullForStar dispatches like saveAs() (save timeout class) and
    // relays raw bytes via mediaBytesForStar().
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
        // Never inserted into the shared RAM cache, like saveAs.
        QCOMPARE(bridge.cacheBytesUsed(), 0);
    }

    void fetchFullForStarServesFromCacheWithoutDispatching()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        // Prime the cache as an inline preview (animatedSource) would.
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

    // A rapid double "Star GIF" on the same row dispatches one fetch.
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

    // A save/star dispatch failure is reported only through its own signal,
    // never mediaFetchFailed(cacheKey), which an inline preview on the same
    // key would take as its own failure.
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

    // An ordinary fetch for a key with an in-flight star request dispatches
    // its own fetch: the star branch never emits mediaCached, so folding the
    // ordinary caller into it would strand it.
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

    // cachedFullContentHash() hashes the full payload already cached for a
    // mediaKey without dispatching a fetch (used by the starred-GIF check).
    void cachedFullContentHashMatchesSha256OfCachedFullPayloadWithoutDispatching()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        // Prime the cache as an inline preview (animatedSource) would.
        bridge.mediaSource(QStringLiteral("$gif"), QStringLiteral("full"));
        const QByteArray bytes("GIF89a-cached-full-bytes");
        client.succeed(client.fetches.first().opId, bytes);
        QCOMPARE(client.fetches.size(), 1);

        const QString expected = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        QCOMPARE(bridge.cachedFullContentHash(QStringLiteral("$gif")), expected);
        QCOMPARE(client.fetches.size(), 1); // no new dispatch — read-only
    }

    // The hash is empty unless the full payload itself is cached: hashing
    // the "thumb:" entry would silently never match a real GIF.
    void cachedFullContentHashIsEmptyUnlessTheFullPayloadItselfIsCached()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        // Empty key.
        QVERIFY(bridge.cachedFullContentHash(QString()).isEmpty());
        // Never requested at all.
        QVERIFY(bridge.cachedFullContentHash(QStringLiteral("$never-fetched")).isEmpty());

        // Only a thumbnail is cached for this key: still "".
        bridge.mediaSource(QStringLiteral("$thumb-only"), QStringLiteral("thumb"));
        client.succeed(client.fetches.first().opId, QByteArray("thumbnail-bytes"));
        QVERIFY(bridge.cachedFullContentHash(QStringLiteral("$thumb-only")).isEmpty());
    }

    // The hash is memoized per cache key (observed through the
    // "contentHashComputed" counter in healthSnapshot) and invalidated by a
    // byte re-insert, LRU eviction and clear().
    void cachedFullContentHashIsMemoizedPerCacheKey()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.mediaSource(QStringLiteral("$gif"), QStringLiteral("full"));
        client.succeed(client.fetches.first().opId, QByteArray(4096, 'x'));

        // Repeated queries (MessageDelegate asks from several signals) cost
        // one real hash.
        for (int i = 0; i < 8; ++i)
            bridge.cachedFullContentHash(QStringLiteral("$gif"));
        QCOMPARE(bridge.healthSnapshot()
                     .value(QStringLiteral("contentHashComputed")).toLongLong(),
                 qint64(1));
        QCOMPARE(bridge.cachedFullContentHash(QStringLiteral("$gif")),
                 QString::fromLatin1(QCryptographicHash::hash(
                     QByteArray(4096, 'x'), QCryptographicHash::Sha256).toHex()));

        // Eviction invalidates the memo: querying the now-uncached key
        // answers "" without computing.
        bridge.setCacheLimitBytes(1); // smaller than either payload
        bridge.mediaSource(QStringLiteral("$other"), QStringLiteral("full"));
        client.succeed(client.fetches.last().opId, QByteArray(4096, 'z'));
        QVERIFY(bridge.cachedFullContentHash(QStringLiteral("$gif")).isEmpty());
        QCOMPARE(bridge.healthSnapshot()
                     .value(QStringLiteral("contentHashComputed")).toLongLong(),
                 qint64(1)); // unchanged — a cache miss never hashes

        // A real re-fetch is a new byte insert and is hashed once, with the
        // new digest.
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

        // clear() drops the memo: a post-clear re-fetch computes again.
        bridge.clear();
        bridge.mediaSource(QStringLiteral("$gif"), QStringLiteral("full"));
        client.succeed(client.fetches.last().opId, QByteArray(4096, 'y'));
        bridge.cachedFullContentHash(QStringLiteral("$gif"));
        QCOMPARE(bridge.healthSnapshot()
                     .value(QStringLiteral("contentHashComputed")).toLongLong(),
                 qint64(3));

        // Isolate the memo clear: a key inserted once (revision 1), cleared,
        // then re-inserted with different bytes (revision 1 again), so the
        // revision guard cannot mask a missing memo clear.
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

    // Request priority: a pressed-play track jumps ahead of queued
    // speculative prefetches.
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

    // Heavy classes (full media, speculative prefetch) never take every slot,
    // leaving headroom for interactive chrome such as avatars.
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

    // Bounded starvation: once the oldest entry has waited past the guard it
    // wins over higher priorities, so speculative work is never parked
    // forever.
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
        // With the guard at 0 the oldest queued entry wins over the newer,
        // higher-priority thumbnail.
        QCOMPARE(client.fetches.last().key, QStringLiteral("$gif"));
    }

    // A live player's materialized file is never deleted under it: pinned
    // entries are skipped by the LRU until unpinned.
    void pinnedPlayableSurvivesEviction()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setPlayableCapsForTest(2, 64 * 1024 * 1024);
        // Above kLargeCacheSkipBytes, so the payload exists only as the file;
        // a small one would re-materialize from RAM and hide the eviction.
        const auto flac = [](char fill) {
            return QByteArray("fLaC") + QByteArray(9 * 1024 * 1024, fill);
        };
        // Writes are off-thread, so await each materialization.
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        int materialized = 0;

        bridge.playableSource(QStringLiteral("$a"));
        client.succeed(client.fetches.at(0).opId, flac('a'),
                       QStringLiteral("audio/flac"));
        QVERIFY(waitForPlayableCount(ready, ++materialized));
        const QString urlA = bridge.playableSource(QStringLiteral("$a"));
        QVERIFY(urlA.startsWith(QLatin1String("file://")));
        bridge.pinPlayable(QStringLiteral("$a"));

        bridge.playableSource(QStringLiteral("$b"));
        client.succeed(client.fetches.at(1).opId, flac('b'),
                       QStringLiteral("audio/flac"));
        QVERIFY(waitForPlayableCount(ready, ++materialized));
        bridge.playableSource(QStringLiteral("$c"));
        client.succeed(client.fetches.at(2).opId, flac('c'),
                       QStringLiteral("audio/flac"));
        QVERIFY(waitForPlayableCount(ready, ++materialized));

        // Cap 2 with three files: unpinned $b was evicted, pinned $a survives.
        QVERIFY(QFileInfo::exists(QUrl(urlA).toLocalFile()));
        QVERIFY(!bridge.playableSource(QStringLiteral("$a")).isEmpty());
        const int fetchesBefore = client.fetches.size();
        QVERIFY(bridge.playableSource(QStringLiteral("$b")).isEmpty());
        QCOMPARE(client.fetches.size(), fetchesBefore + 1); // re-dispatch

        // Pins are refcounted: a second card's pin keeps the file after the
        // first card releases.
        bridge.pinPlayable(QStringLiteral("$a"));   // second holder
        bridge.unpinPlayable(QStringLiteral("$a")); // first releases
        bridge.playableSource(QStringLiteral("$x1"));
        client.succeed(client.fetches.last().opId, flac('x'),
                       QStringLiteral("audio/flac"));
        QVERIFY(waitForPlayableCount(ready, ++materialized));
        bridge.playableSource(QStringLiteral("$x2"));
        client.succeed(client.fetches.last().opId, flac('y'),
                       QStringLiteral("audio/flac"));
        QVERIFY(waitForPlayableCount(ready, ++materialized));
        QVERIFY(QFileInfo::exists(QUrl(urlA).toLocalFile()));

        // Unpinned by its last holder, $a is an ordinary LRU victim again.
        bridge.unpinPlayable(QStringLiteral("$a"));
        bridge.playableSource(QStringLiteral("$d"));
        client.succeed(client.fetches.last().opId, flac('d'),
                       QStringLiteral("audio/flac"));
        QVERIFY(waitForPlayableCount(ready, ++materialized));
        bridge.playableSource(QStringLiteral("$e"));
        client.succeed(client.fetches.last().opId, flac('e'),
                       QStringLiteral("audio/flac"));
        QVERIFY(waitForPlayableCount(ready, ++materialized));
        QVERIFY(!QFileInfo::exists(QUrl(urlA).toLocalFile()));
    }

    // A "thumb" result may carry the video's MIME or even be the original A/V
    // payload; the bytes decide. A/V containers never enter the image path,
    // real image bytes pass despite a wrong MIME, and full fetches are
    // untouched.
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

    // The full-payload class is sniffed for SVG exactly as thumbnails are:
    // image rows without a sender thumbnail, the full-screen viewer and
    // profile/space banners all fetch "full".
    void fullPayloadSniffingRejectsSvgTheSameWayAThumbnailDoes()
    {
        const QByteArray plainSvg = "<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
        const QByteArray xmlFirst =
            "<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\"/>";
        const QByteArray svgz = QByteArray("\x1f\x8b", 2) + QByteArray(30, '\x08');

        int n = 0;
        for (const QByteArray &payload : { plainSvg, xmlFirst, svgz }) {
            FakeClient client;
            MediaBridge bridge;
            bridge.setClient(&client);
            QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
            QSignalSpy cached(&bridge, &MediaBridge::mediaCached);
            const QString ev = QStringLiteral("$fullsvg%1").arg(n++);

            bridge.mediaSource(ev, QStringLiteral("full"));
            QCOMPARE(client.fetches.size(), 1);
            // The image/png label is a lie, as a hostile sender would send it.
            client.succeed(client.fetches.at(0).opId, payload,
                           QStringLiteral("image/png"));
            QCOMPARE(cached.count(), 0);
            QCOMPARE(failed.count(), 1);
            QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("rejected"));
            QVERIFY(bridge.cachedBytes(QStringLiteral("full:") + ev).isEmpty());
        }
    }

    // A sender-chosen attachment name cannot act as a path when suggested
    // for saving.
    void aHostileAttachmentNameCannotEscapeTheChosenDirectory()
    {
        MediaBridge bridge;
        // Separators, traversal, control characters and Windows device names
        // are neutralised. A single leading dot is kept: the helper also
        // sanitises what the user typed, where `.hidden.png` is deliberate.
        QCOMPARE(bridge.suggestedSaveName(QStringLiteral("../../.bashrc")),
                 QStringLiteral(".bashrc"));
        QCOMPARE(bridge.suggestedSaveName(QStringLiteral("..\\..\\evil.exe")),
                 QStringLiteral("evil.exe"));
        QCOMPARE(bridge.suggestedSaveName(QStringLiteral("a/b/c.png")),
                 QStringLiteral("c.png"));
        QVERIFY(bridge.suggestedSaveName(QStringLiteral("con.txt"))
                    .startsWith(QStringLiteral("file-")));
        QVERIFY(!bridge.suggestedSaveName(QStringLiteral("ok.png"))
                     .contains(QLatin1Char('/')));
        QCOMPARE(bridge.suggestedSaveName(QStringLiteral("ok.png")),
                 QStringLiteral("ok.png"));
        // Nothing to suggest is an empty answer, so the dialog chooses.
        QVERIFY(bridge.suggestedSaveName(QString()).isEmpty());
        QVERIFY(bridge.suggestedSaveName(QStringLiteral("   ")).isEmpty());
        QVERIFY(bridge.suggestedSaveName(QStringLiteral("..")).isEmpty());
        // Bounded length, keeping the suffix.
        const QString longName =
            bridge.suggestedSaveName(QString(400, QLatin1Char('x'))
                                     + QStringLiteral(".png"));
        QVERIFY(longName.size() <= 120);
        QVERIFY(longName.endsWith(QStringLiteral(".png")));
    }

    // writeSaveFile resolves the parent directory first and attaches a
    // sanitised leaf to it, so a `../` in the leaf cannot escape.
    void savingWithATraversingLeafStaysInTheReportedDirectory()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir(dir.path()).mkpath(QStringLiteral("sub"));
        const QString outside = dir.path() + QStringLiteral("/pwned.txt");

        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy saved(&bridge, &MediaBridge::saveFinished);

        // The dialog reports <dir>/sub, and the leaf tries to climb out.
        const QUrl destination = QUrl::fromLocalFile(
            dir.path() + QStringLiteral("/sub/../pwned.txt"));
        bridge.saveAs(QStringLiteral("$ev"), destination);
        QCOMPARE(client.fetches.size(), 1);
        client.succeed(client.fetches.at(0).opId, QByteArray("payload"),
                       QStringLiteral("text/plain"));
        QVERIFY(saved.wait(3000) || saved.count() > 0);

        // The file is inside the resolved directory, never above it.
        QVERIFY2(!QFileInfo::exists(outside + QStringLiteral(".escaped")),
                 "the leaf escaped the resolved directory");
    }

    // Thumbnail payloads that are SVG markup or gzip (SVGZ) are refused by
    // their bytes, whatever the declared type.
    void thumbPayloadSniffingRejectsSvgAndItsCompressedSpelling()
    {
        const QByteArray plainSvg = "<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
        const QByteArray xmlFirst =
            "<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\"/>";
        const QByteArray bomAndSpace =
            QByteArray("\xef\xbb\xbf", 3) + "\n\t  <svg/>";
        const QByteArray doctypeFirst = "<!DOCTYPE svg><svg/>";
        const QByteArray commentFirst = "<!-- hello --><svg/>";
        const QByteArray svgz = QByteArray("\x1f\x8b", 2) + QByteArray(30, '\x08');

        int n = 0;
        for (const QByteArray &payload : { plainSvg, xmlFirst, bomAndSpace,
                                           doctypeFirst, commentFirst, svgz }) {
            FakeClient client;
            MediaBridge bridge;
            bridge.setClient(&client);
            QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
            QSignalSpy cached(&bridge, &MediaBridge::mediaCached);
            const QString ev = QStringLiteral("$svg%1").arg(n++);

            bridge.mediaSource(ev, QStringLiteral("thumb"));
            // The image/png label is a lie: the bytes decide.
            client.succeed(client.fetches.at(0).opId, payload,
                           QStringLiteral("image/png"));
            QCOMPARE(cached.count(), 0);
            QCOMPARE(failed.count(), 1);
            QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("rejected"));
            QVERIFY(bridge.cachedBytes(QStringLiteral("thumb:") + ev).isEmpty());
        }

        // A real raster still passes (PNG magic cannot collide with the
        // markup test).
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);
        bridge.mediaSource(QStringLiteral("$png"), QStringLiteral("thumb"));
        QByteArray png = QByteArray("\x89PNG\r\n\x1a\n", 8) + QByteArray(16, 'p');
        client.succeed(client.fetches.at(0).opId, png,
                       QStringLiteral("image/png"));
        QCOMPARE(cached.count(), 1);
    }

    // A room switch drops queued speculative prefetches; queued interactive
    // work and in-flight ops are untouched.
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

    // Cancelling an in-flight playable fetch aborts the backend op, frees the
    // slot and leaves no failure mark, so a fresh Play re-dispatches.
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

    // A late completion for a cancelled op is stale: no cache entry, no
    // playableMediaReady.
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
        // Stale, so no write starts and no worker completion can arrive.
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 0);
        QTest::qWait(50);
        QCOMPARE(ready.count(), 0);
        QVERIFY(bridge.cachedSource(QStringLiteral("full:$video")).isEmpty());
    }

    // Another interest in the same bytes (a GIF row) keeps the fetch alive
    // through a playable cancel.
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

    // Playable prefetch is bounded: declared in-cap sizes dispatch at the
    // playable timeout class; unknown or over-cap sizes never dispatch.
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
        // A prefetched payload materializes and signals playableMediaReady.
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        QByteArray mp4(1024, 'x');
        mp4.replace(4, 4, "ftyp");
        client.succeed(client.fetches.first().opId, mp4,
                       QStringLiteral("video/mp4"));
        QVERIFY(waitForPlayableCount(ready, 1));
        // Play now serves the materialized file with no new fetch.
        QVERIFY(!bridge.playableSource(QStringLiteral("$small")).isEmpty());
        QCOMPARE(client.fetches.size(), 1);
    }

    // Queued playable prefetches are dropped on room switch like other
    // speculative work.
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

    // Playable interest is refcounted: two cards share one fetch, and the
    // first card's cancel does not strand the second.
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

    // A poster hook left by an over-cap video (prefetch declined) does not veto
    // a later cancel.
    void posterHookForOverCapVideoDoesNotBlockCancel()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        // No thumbnail, 500 MB declared: the poster path declines the prefetch
        // and must not leak its hook.
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

    // An interest set records an outstanding fetch, and a cache hit has none.
    // A cached animatedSource() answer must not leave the key "wanted", or
    // every later cancelPlayable() for it would be vetoed.
    void aCachedAnimationLeavesNoInterestBehindToVetoACancel()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QByteArray gif("GIF89a");
        gif.append(QByteArray(64, '\0'));
        // The still image path fetched these bytes, so the animation is served
        // from RAM with no fetch.
        bridge.mediaSource(QStringLiteral("$gif"), QStringLiteral("full"));
        client.succeed(client.fetches.first().opId, gif,
                       QStringLiteral("image/gif"));
        QCOMPARE(client.fetches.size(), 1);

        // Served from the cache: no fetch, so no interest.
        QVERIFY(bridge.animatedSource(QStringLiteral("$gif"))
                    .startsWith(QStringLiteral("file://")));
        QCOMPARE(client.fetches.size(), 1);

        // Play on the same media (a GIF is not playable, so a real transfer
        // starts), then close the card.
        bridge.playableSource(QStringLiteral("$gif"));
        QCOMPARE(bridge.inflightCountForTest(), 1);
        const quint64 opId = client.fetches.last().opId;
        bridge.cancelPlayable(QStringLiteral("$gif"));
        QCOMPARE(client.cancels, QList<quint64>{opId});
        QCOMPARE(bridge.inflightCountForTest(), 0);
    }

    // A demanding caller whose cached bytes are not an animation gets the
    // terminal answer instead of waiting on a fetch that never runs.
    void aCachedPayloadThatIsNotAnAnimationAnswersTheDemandingCaller()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        QByteArray png("\x89PNG\r\n\x1a\n", 8);
        png.append(QByteArray(64, '\0'));
        bridge.mediaSource(QStringLiteral("$png"), QStringLiteral("full"));
        client.succeed(client.fetches.first().opId, png, QString());
        QCOMPARE(failed.count(), 0);

        QCOMPARE(bridge.animatedSource(QStringLiteral("$png")), QString());
        QCOMPARE(ready.count(), 0);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().at(0).toString(),
                 QStringLiteral("full:$png"));
        QCOMPARE(failed.first().at(1).toString(),
                 QStringLiteral("invalid_gif"));
        // No failure mark: the bytes are fine, just not animated.
        QVERIFY(bridge.failureCategory(QStringLiteral("full:$png")).isEmpty());

        // A speculative asker on the same branch still gets silence.
        bridge.mediaSource(QStringLiteral("$sticker"), QStringLiteral("full"));
        client.succeed(client.fetches.last().opId, png, QString());
        QCOMPARE(bridge.animatedSource(QStringLiteral("$sticker"),
                                       /*speculative=*/true),
                 QString());
        QCOMPARE(failed.count(), 1);
    }

    // A terminal failure voids every interest class for the key, so a later
    // cancel is not vetoed and the sets stay bounded.
    void terminalFailureClearsInterestSets()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.prefetchPlayable(QStringLiteral("$flaky"), 1024 * 1024);
        QCOMPARE(client.fetches.size(), 1);
        client.fail(client.fetches.first().opId, QStringLiteral("network"));
        // After the transient mark clears, a pressed-play fetch dispatches and
        // is cancellable.
        bridge.retry(QStringLiteral("full:$flaky"));
        bridge.playableSource(QStringLiteral("$flaky"));
        QCOMPARE(client.fetches.size(), 2);
        bridge.cancelPlayable(QStringLiteral("$flaky"));
        QCOMPARE(client.cancels.size(), 1);
        QCOMPARE(bridge.inflightCountForTest(), 0);
    }

    // A playableSource() served from the RAM cache starts a worker write and
    // answers ""; its interest survives the call and is retired by the
    // completion, so it cannot veto a later cancel of a real fetch.
    void ramCacheHitRetiresItsInterestWhenTheWriteLands()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        bridge.setPlayableCapsForTest(1, 1024 * 1024); // 1 materialized file
        QByteArray mp4(256, 'x');
        mp4.replace(4, 4, "ftyp");
        // Fetch $track once (RAM-cached + materialized).
        bridge.playableSource(QStringLiteral("$track"));
        client.succeed(client.fetches.at(0).opId, mp4,
                       QStringLiteral("video/mp4"));
        QVERIFY(waitForPlayableCount(ready, 1));
        // Evict $track's FILE with $other (file cap is 1)...
        bridge.playableSource(QStringLiteral("$other"));
        client.succeed(client.fetches.at(1).opId, mp4,
                       QStringLiteral("video/mp4"));
        QVERIFY(waitForPlayableCount(ready, 2));
        // Re-ask $track, whose bytes are still in RAM: "" and a worker write,
        // no second fetch.
        QVERIFY(bridge.playableSource(QStringLiteral("$track")).isEmpty());
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 1);
        QCOMPARE(client.fetches.size(), 2);
        QVERIFY(waitForPlayableCount(ready, 3));
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 0);
        QVERIFY(!bridge.playableSource(QStringLiteral("$track")).isEmpty());
        // Drop the RAM copies (the limit forces eviction on the next insert)...
        bridge.setCacheLimitBytes(1);
        bridge.mediaSource(QStringLiteral("$bump"), QStringLiteral("full"));
        client.succeed(client.fetches.at(2).opId, QByteArray("img"));
        // ...and $track's file again (via $other, now a real fetch).
        bridge.playableSource(QStringLiteral("$other"));
        client.succeed(client.fetches.at(3).opId, mp4,
                       QStringLiteral("video/mp4"));
        QVERIFY(waitForPlayableCount(ready, 4));
        // A real fetch for $track carries exactly one press-play interest; a
        // count left over from the cache hit would make this cancel a no-op.
        bridge.playableSource(QStringLiteral("$track"));
        QCOMPARE(bridge.inflightCountForTest(), 1);
        bridge.cancelPlayable(QStringLiteral("$track"));
        QCOMPARE(client.cancels.size(), 1);
        QCOMPARE(bridge.inflightCountForTest(), 0);
    }

    // cancelPlayable with no playable interest is a no-op, so an unrelated
    // ordinary fetch for the key survives.
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

    // A width-only sourceSize (QML's "scale to this width" idiom) bounds the
    // decode. QSize::isEmpty() is true when either axis is 0, so an
    // `isValid() && !isEmpty()` guard decoded at full resolution.
    void aWidthOnlySourceSizeBoundsTheDecode()
    {
        FakeClient client;
        // Not MediaBridge(&client): that argument is the QObject parent.
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

    // Asking for no size decodes naturally (save path, full-size viewer).
    void noSourceSizeDecodesNaturally()
    {
        FakeClient client;
        // Not MediaBridge(&client): that argument is the QObject parent.
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
        // A request larger than the source does not upscale; the scene graph
        // scales small images.
        QCOMPARE(provider.requestImage(id, &reported, QSize(4000, 0)).size(),
                 QSize(1200, 900));
        // A height-only request is the same idiom on the other axis.
        QCOMPARE(provider.requestImage(id, &reported, QSize(0, 300)).size(),
                 QSize(400, 300));
    }

    // A baked shape (avatar mask) still honours an upscale: the mask is
    // rasterized at the baked size, so baking at the source size would alias
    // when shown larger. Deliberately unlike plain images.
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
