// Server-side message search: debounced dispatch, roomName resolution against
// the room list, next_batch paging, stale superseding, scope-change clears
// and sign-out invalidation. Encrypted rooms are excluded by the server, which
// this mock cannot observe.

#include "app/SettingsManager.h"
#include "matrix/MockMatrixClient.h"
#include "models/MessageSearchController.h"

#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <QtTest/QtTest>

namespace {
constexpr int kSignalTimeoutMs = 3000;

QVariantMap resultRow(const QString &roomId, const QString &eventId,
                      const QString &body)
{
    QVariantMap row;
    row.insert(QStringLiteral("roomId"), roomId);
    row.insert(QStringLiteral("eventId"), eventId);
    row.insert(QStringLiteral("sender"), QStringLiteral("@bob:mock.local"));
    row.insert(QStringLiteral("senderDisplayName"), QStringLiteral("Bob"));
    row.insert(QStringLiteral("senderAvatarUrl"), QString());
    row.insert(QStringLiteral("timestampMs"), qint64(1700000000000));
    row.insert(QStringLiteral("msgtype"), QStringLiteral("m.text"));
    row.insert(QStringLiteral("body"), body);
    return row;
}

// A local-search backend whose corpus size and page size the test controls:
// it records every LIMIT the controller asks for and answers with as many
// rows as an index of `available` matches would.
class PagedLocalSearchClient final : public MockMatrixClient
{
    Q_OBJECT
public:
    QList<int> limits;   // every LIMIT asked for, in order
    int available = 0;   // how many matches the index holds

    quint64 localSearch(const QString &query, const QString &roomId,
                        int limit, int offset) override
    {
        Q_UNUSED(query);
        Q_UNUSED(roomId);
        Q_UNUSED(offset);
        limits.append(limit);
        const quint64 op = ++m_fakeOp;
        QVariantList rows;
        for (int i = 0; i < qMin(limit, available); ++i) {
            rows.append(resultRow(QStringLiteral("!general:mock.local"),
                                  QStringLiteral("$e%1").arg(i),
                                  QStringLiteral("needle %1").arg(i)));
        }
        QTimer::singleShot(0, this, [this, op, rows] {
            Q_EMIT localSearchFinished(op, true, QString(), 3, rows);
        });
        return op;
    }

private:
    // The mock's own op counter is private and nothing here runs the base
    // implementation; any distinct non-zero id serves.
    quint64 m_fakeOp = 1000000;
};
} // namespace

class MessageSearchControllerTest : public QObject
{
    Q_OBJECT

    // The index-all offer remembers its answer in SettingsManager; keep that
    // file out of the real configuration.
    QTemporaryDir m_configHome;

    static QString alice() { return QStringLiteral("@alice:mock.local"); }

    // An account record, as a real sign-in writes before loginSucceeded. The
    // token is a fixture, not a credential.
    static void recordAccount(SettingsManager &settings, const QString &userId)
    {
        settings.saveSession(QStringLiteral("https://mock.local"), userId,
                             QStringLiteral("DEVICE"),
                             QStringLiteral("token-fixture"));
    }

    static QVariantMap progress(const QString &state, int position, int total)
    {
        return {
            { QStringLiteral("state"), state },
            { QStringLiteral("position"), position },
            { QStringLiteral("total"), total },
        };
    }

    static bool login(MockMatrixClient &client)
    {
        QSignalSpy spy(&client, &MatrixClient::loginSucceeded);
        client.login(QStringLiteral("https://mock.local"),
                     QStringLiteral("alice"), QStringLiteral("x"));
        if (!spy.wait(kSignalTimeoutMs))
            return false;
        client.startSync();
        return true;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("message-search-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void debouncedQueryPopulatesWithRoomNames()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        // Server search explicitly: the mock also has a local index, which
        // the controller would otherwise prefer.
        model.setSource(QStringLiteral("server"));
        QVERIFY(model.supported());

        // "!general:mock.local" is a seeded mock room with a display name;
        // an unknown room id must fall back to the id itself.
        client.mockSearchResults = {
            resultRow(QStringLiteral("!general:mock.local"),
                      QStringLiteral("$e1"), QStringLiteral("hello world")),
            resultRow(QStringLiteral("!unknown:mock.local"),
                      QStringLiteral("$e2"), QStringLiteral("hello again")),
        };
        model.setQuery(QStringLiteral("hello"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        QCOMPARE(model.rowCount(), 2);
        const QString knownName =
            model.rowAt(0).value(QStringLiteral("roomName")).toString();
        QVERIFY(!knownName.isEmpty());
        QVERIFY(knownName != QStringLiteral("!general:mock.local"));
        QCOMPARE(model.rowAt(1).value(QStringLiteral("roomName")).toString(),
                 QStringLiteral("!unknown:mock.local"));
    }

    void emptyQueryClearsToIdle()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        // Server search explicitly: the mock also has a local index, which
        // the controller would otherwise prefer.
        model.setSource(QStringLiteral("server"));
        client.mockSearchResults = { resultRow(
            QStringLiteral("!general:mock.local"), QStringLiteral("$e1"),
            QStringLiteral("x")) };
        model.setQuery(QStringLiteral("x"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        model.setQuery(QString());
        QCOMPARE(model.state(), QStringLiteral("idle"));
        QCOMPARE(model.rowCount(), 0);
    }

    void scopeChangeDropsTheOldScopesAnswers()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        // Server search explicitly: the mock also has a local index, which
        // the controller would otherwise prefer.
        model.setSource(QStringLiteral("server"));
        client.mockSearchResults = { resultRow(
            QStringLiteral("!general:mock.local"), QStringLiteral("$e1"),
            QStringLiteral("x")) };
        model.setQuery(QStringLiteral("x"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        model.setRoomId(QStringLiteral("!general:mock.local"));
        // A different scope answers a different question: everything the
        // global scope produced is gone.
        QCOMPARE(model.state(), QStringLiteral("idle"));
        QCOMPARE(model.rowCount(), 0);
    }

    void pagesWithNextBatchAndTerminates()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        // Server search explicitly: the mock also has a local index, which
        // the controller would otherwise prefer.
        model.setSource(QStringLiteral("server"));
        client.mockSearchResults = { resultRow(
            QStringLiteral("!general:mock.local"), QStringLiteral("$e1"),
            QStringLiteral("hit one")) };
        client.mockSearchNextBatch = QStringLiteral("batch2");
        model.setQuery(QStringLiteral("hit"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        QVERIFY(model.canLoadMore());
        model.loadMore();
        QTRY_COMPARE(model.rowCount(), 2);
        QVERIFY(!model.canLoadMore());
    }

    void staleAnswersNeverRepaintANewerQuery()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        // Server search explicitly: the mock also has a local index, which
        // the controller would otherwise prefer.
        model.setSource(QStringLiteral("server"));
        client.mockSearchResults = { resultRow(
            QStringLiteral("!general:mock.local"), QStringLiteral("$old"),
            QStringLiteral("old answer")) };
        model.setQuery(QStringLiteral("first"));
        client.mockSearchResults = { resultRow(
            QStringLiteral("!general:mock.local"), QStringLiteral("$new"),
            QStringLiteral("new answer")) };
        model.setQuery(QStringLiteral("second"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.rowAt(0).value(QStringLiteral("eventId")).toString(),
                 QStringLiteral("$new"));
    }

    void appliesCombinedFiltersAndForwardsServerSenders()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        // Server search explicitly: the mock also has a local index, which
        // the controller would otherwise prefer.
        model.setSource(QStringLiteral("server"));
        model.setRoomId(QStringLiteral("!general:mock.local"));

        QVariantMap matching = resultRow(
            QStringLiteral("!general:mock.local"), QStringLiteral("$match"),
            QStringLiteral("image for Alice"));
        matching.insert(QStringLiteral("msgtype"), QStringLiteral("m.image"));
        matching.insert(QStringLiteral("mentionUserIds"),
                        QVariantList{ QStringLiteral("@alice:mock.local") });
        matching.insert(QStringLiteral("timestampMs"), qint64(1700000000000));

        QVariantMap wrongKind = matching;
        wrongKind.insert(QStringLiteral("eventId"), QStringLiteral("$text"));
        wrongKind.insert(QStringLiteral("msgtype"), QStringLiteral("m.text"));
        QVariantMap wrongMention = matching;
        wrongMention.insert(QStringLiteral("eventId"), QStringLiteral("$mention"));
        wrongMention.insert(QStringLiteral("mentionUserIds"),
                            QVariantList{ QStringLiteral("@carol:mock.local") });
        client.mockSearchResults = { matching, wrongKind, wrongMention };

        const QVariantMap filters{
            { QStringLiteral("fromUserIds"),
              QVariantList{ QStringLiteral("@bob:mock.local") } },
            { QStringLiteral("mentionUserIds"),
              QVariantList{ QStringLiteral("@alice:mock.local") } },
            { QStringLiteral("contentTypes"),
              QVariantList{ QStringLiteral("image") } },
            { QStringLiteral("afterMs"), qint64(1699999999000) },
            { QStringLiteral("beforeMs"), qint64(1700000001000) },
            { QStringLiteral("pinnedMode"), QStringLiteral("pinned") },
            { QStringLiteral("pinnedEventIds"),
              QVariantList{ QStringLiteral("$match") } },
        };
        model.setFilters(filters);
        model.setQuery(QStringLiteral("image"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.rowAt(0).value(QStringLiteral("eventId")).toString(),
                 QStringLiteral("$match"));
        QCOMPARE(client.lastSearchFilters, filters);
    }

    void loggedOutClears()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        // Server search explicitly: the mock also has a local index, which
        // the controller would otherwise prefer.
        model.setSource(QStringLiteral("server"));
        client.mockSearchResults = { resultRow(
            QStringLiteral("!general:mock.local"), QStringLiteral("$e1"),
            QStringLiteral("x")) };
        model.setQuery(QStringLiteral("x"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        client.logout();
        QTRY_COMPARE(model.state(), QStringLiteral("idle"));
        QCOMPARE(model.rowCount(), 0);
    }
    // ── Indexing under a live result list must refresh it ────────────────
    //
    // Both a background sweep and an explicit "Index this room" must refresh
    // the visible results, the latter even when it wrote nothing.
    void indexingUnderALiveResultListRefreshesIt()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        QCOMPARE(model.source(), QStringLiteral("local"));
        model.setRoomId(QStringLiteral("!general:mock.local"));
        model.setQuery(QStringLiteral("Welcome"));
        QTRY_VERIFY(model.rowCount() > 0);

        // A SWEEP that grew the index re-runs the query.
        int before = 0;
        QSignalSpy searches(&client, &MatrixClient::localSearchFinished);
        Q_EMIT client.searchIndexSwept(1, 1, 5, 999, 1);
        QTRY_VERIFY_WITH_TIMEOUT(searches.count() > before, kSignalTimeoutMs);

        // A sweep that grew NOTHING must not re-run — the guard has to be the
        // index growing, or the timer turns into a five-minute query loop.
        before = searches.count();
        Q_EMIT client.searchIndexSwept(2, 1, 0, 999, 1);
        QTest::qWait(120);
        QCOMPARE(searches.count(), before);

        // An explicit "Index this room" refreshes even when it writes nothing
        // (the mock's deepen answers written == 0).
        before = searches.count();
        model.indexRoomHistory(QStringLiteral("!general:mock.local"));
        QTRY_VERIFY_WITH_TIMEOUT(searches.count() > before, kSignalTimeoutMs);
    }

    // ── The date bounds, isolated ────────────────────────────────────────
    //
    // Rows differ only by timestamp and only the bounds are set, so this fails
    // if either clause goes or flips. The upper bound is exclusive, so "before
    // 1 Jan" means all of 31 Dec; `atUpperBound` pins that.
    void dateBoundsAloneDecideWhichRowsSurvive()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        model.setSource(QStringLiteral("server"));

        const qint64 lower = 1700000000000LL;
        const qint64 upper = 1700000010000LL;
        auto atTime = [](const QString &id, qint64 ts) {
            QVariantMap row = resultRow(QStringLiteral("!general:mock.local"),
                                        id, QStringLiteral("x"));
            row.insert(QStringLiteral("timestampMs"), ts);
            return row;
        };
        client.mockSearchResults = {
            atTime(QStringLiteral("$tooEarly"), lower - 1),
            atTime(QStringLiteral("$atLowerBound"), lower),
            atTime(QStringLiteral("$inside"), lower + 5000),
            atTime(QStringLiteral("$atUpperBound"), upper),
            atTime(QStringLiteral("$tooLate"), upper + 1),
        };
        model.setFilters(QVariantMap{
            { QStringLiteral("afterMs"), lower },
            { QStringLiteral("beforeMs"), upper },
        });
        model.setQuery(QStringLiteral("x"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));

        QStringList kept;
        for (int i = 0; i < model.rowCount(); ++i)
            kept << model.rowAt(i).value(QStringLiteral("eventId")).toString();
        std::sort(kept.begin(), kept.end());
        // Lower bound INCLUSIVE, upper bound EXCLUSIVE.
        QCOMPARE(kept, (QStringList{ QStringLiteral("$atLowerBound"),
                                     QStringLiteral("$inside") }));
    }

    // A local page is full relative to what it asked for, not to kLocalPage.
    // "Load more" re-runs local search with a bigger limit (rows + 50), so
    // comparing against the constant made an exhausted index look like it
    // had more, and the list kept auto-loading.
    void aLocalPageIsFullOnlyRelativeToWhatItAskedFor()
    {
        PagedLocalSearchClient client;
        QVERIFY(login(client));
        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        QCOMPARE(model.source(), QStringLiteral("local"));

        // The index holds EXACTLY one page: the first page comes back full,
        // which is the only evidence there is and it does say "maybe more".
        client.available = 50;
        model.setQuery(QStringLiteral("needle"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        QCOMPARE(model.rowCount(), 50);
        QCOMPARE(client.limits, (QList<int>{ 50 }));
        QVERIFY(model.canLoadMore());

        // "More" asks for 100 and gets 50 — the index has no more to give.
        model.loadMore();
        QCOMPARE(model.state(), QStringLiteral("loading_more"));
        QCOMPARE(client.limits, (QList<int>{ 50, 100 }));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        QCOMPARE(model.rowCount(), 50);
        QVERIFY2(!model.canLoadMore(),
                 "a 50-row answer to a 100-row request is an exhausted "
                 "index, and offering More again is the paging spin");

        // ...and a page that genuinely IS full still offers more, so the
        // fix cannot have been "always stop after the second page".
        client.available = 500;
        model.setQuery(QStringLiteral("needle2"));
        QTRY_VERIFY(client.limits.size() == 3);
        QTRY_VERIFY(model.canLoadMore());
        QCOMPARE(model.rowCount(), 50);
        model.loadMore();
        QCOMPARE(client.limits.last(), 100);
        QTRY_COMPARE(model.rowCount(), 100);
        QVERIFY(model.canLoadMore());
    }

    // The next limit grows from the raw page, not from the filtered rows: a
    // filter that drops a whole page must not freeze the limit at 0 + 50.
    // Every mock row has timestampMs 1700000000000, so an afterMs one
    // millisecond later drops all of them.
    void aFullyFilteredPageStillGrowsTheNextRequestsLimit()
    {
        PagedLocalSearchClient client;
        QVERIFY(login(client));
        client.available = 60;          // more than one page, fewer than two
        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        QCOMPARE(model.source(), QStringLiteral("local"));
        model.setFilters(QVariantMap{
            { QStringLiteral("afterMs"), qint64(1700000000001) },
        });
        model.setQuery(QStringLiteral("needle"));
        QTRY_COMPARE(model.state(), QStringLiteral("no_results"));
        QCOMPARE(model.rowCount(), 0);
        // 50 raw rows came back against a limit of 50: full, so there may be
        // more even though the reader sees nothing.
        QVERIFY(model.canLoadMore());

        model.loadMore();
        QTRY_VERIFY(!model.canLoadMore());
        // The second request asked for 100 — 50 RAW rows plus a page — and
        // the index answered 60, which is short and therefore exhaustion.
        QCOMPARE(client.limits, (QList<int>{ 50, 100 }));
    }

    // ── The two producers must spell the sender the same way ─────────────
    //
    // Server and local results both reach the model through
    // senderDisplayName. Asserts the value, since an empty string is the
    // failure.
    void aLocalResultCarriesItsSenderThroughToTheModel()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        QVERIFY(model.localAvailable());
        QCOMPARE(model.source(), QStringLiteral("local"));
        model.setRoomId(QStringLiteral("!general:mock.local"));
        // A needle from the mock's OWN seeded timeline — the local index
        // scans what the backend holds, not mockSearchResults.
        model.setQuery(QStringLiteral("Welcome"));
        QTRY_VERIFY(model.rowCount() > 0);
        const QModelIndex idx = model.index(0);
        const QString shown =
            model.data(idx, MessageSearchController::SenderDisplayNameRole)
                .toString();
        const QString mxid =
            model.data(idx, MessageSearchController::SenderRole).toString();
        QVERIFY2(!mxid.isEmpty(), "a local row lost its sender id");
        QVERIFY2(!shown.isEmpty(),
                 "a local row lost its sender display name — the producers "
                 "and MessageSearchController disagree on the key");
    }
    // ── "Index all rooms" ────────────────────────────────────────────────
    //
    // Old code: the controller had no index-all state and MatrixClient no
    // API to drive a pass, so none of these compiled. Each also pins what a
    // naive port would get wrong, named in its comment.

    // The controller mirrors the backend's pass and derives what the UI shows:
    // the room by NAME, an estimate from the pass's own pace, and which states
    // are under way. Storing `state` alone fails the name, the estimate and
    // the resumable checks; treating "held" as stopped fails the second block.
    void indexAllStateFollowsTheBackendsProgress()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MessageSearchController model;
        model.setClient(&client);

        model.indexAllRooms();
        QCOMPARE(client.indexAllStarts, QList<bool>{ false });
        QTRY_COMPARE(model.indexAllState(), QStringLiteral("running"));
        QVERIFY(model.indexAllActive());
        QCOMPARE(model.indexAllEtaMs(), qint64(-1));   // no room finished yet

        QVariantMap status = progress(QStringLiteral("running"), 2, 4);
        status.insert(QStringLiteral("currentRoomId"),
                      QStringLiteral("!general:mock.local"));
        status.insert(QStringLiteral("written"), 120);
        status.insert(QStringLiteral("undecryptable"), 7);
        status.insert(QStringLiteral("elapsedMs"), 60000);
        Q_EMIT client.searchIndexAllProgress(1, status);
        QCOMPARE(model.indexAllPosition(), 2);
        QCOMPARE(model.indexAllTotal(), 4);
        QCOMPARE(model.indexAllWritten(), qint64(120));
        QCOMPARE(model.indexAllUndecryptable(), qint64(7));
        QVERIFY2(!model.indexAllRoomName().isEmpty()
                     && model.indexAllRoomName()
                            != QStringLiteral("!general:mock.local"),
                 "the room in progress was not shown by its name");
        // 30 s per finished room, two rooms left.
        QCOMPARE(model.indexAllEtaMs(), qint64(60000));

        // Waiting for a call or on the server is still a pass under way.
        for (const char *state : { "held", "backoff" }) {
            Q_EMIT client.searchIndexAllProgress(
                1, progress(QString::fromLatin1(state), 2, 4));
            QVERIFY2(model.indexAllActive(), state);
            QVERIFY(!model.indexAllResumable());
        }

        model.pauseIndexAll();
        QCOMPARE(client.indexAllPauses, 1);
        QTRY_COMPARE(model.indexAllState(), QStringLiteral("paused"));
        QVERIFY(!model.indexAllActive());
        QVERIFY(model.indexAllResumable());
        QCOMPARE(model.indexAllEtaMs(), qint64(-1));

        model.cancelIndexAll();
        QCOMPARE(client.indexAllCancels, 1);
        QTRY_COMPARE(model.indexAllState(), QStringLiteral("cancelled"));
        QVERIFY(!model.indexAllResumable());
        QVERIFY(!model.indexAllActive());
    }

    // A pass that settles re-runs a local query on screen; one still running
    // does not, or every page would repaint the list being read. Re-running on
    // every progress event fails the first half.
    void aSettledIndexAllPassRefreshesALiveResultList()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!general:mock.local"));
        model.setQuery(QStringLiteral("Welcome"));
        QTRY_VERIFY(model.rowCount() > 0);

        QSignalSpy searches(&client, &MatrixClient::localSearchFinished);
        Q_EMIT client.searchIndexAllProgress(1, progress(QStringLiteral("running"), 1, 3));
        Q_EMIT client.searchIndexAllProgress(1, progress(QStringLiteral("running"), 2, 3));
        QTest::qWait(120);
        QCOMPARE(searches.count(), 0);

        Q_EMIT client.searchIndexAllProgress(1, progress(QStringLiteral("done"), 0, 0));
        QTRY_VERIFY_WITH_TIMEOUT(searches.count() > 0, kSignalTimeoutMs);
    }

    // A call and a scroll hold the pass independently; releasing one must not
    // release the other. A setter that forwards only the latest reason fails
    // the first comparison.
    void indexAllHoldsReachTheBackendAsOneBitSet()
    {
        MockMatrixClient client;
        MessageSearchController model;
        model.setClient(&client);
        model.setIndexAllHold(MatrixClient::IndexAllHoldCall, true);
        model.setIndexAllHold(MatrixClient::IndexAllHoldScroll, true);
        QCOMPARE(client.indexAllHold,
                 unsigned(MatrixClient::IndexAllHoldCall
                          | MatrixClient::IndexAllHoldScroll));
        model.setIndexAllHold(MatrixClient::IndexAllHoldCall, false);
        QCOMPARE(client.indexAllHold, unsigned(MatrixClient::IndexAllHoldScroll));

        // A hold in force carries over to a new backend.
        MockMatrixClient next;
        model.setClient(&next);
        QCOMPARE(next.indexAllHold, unsigned(MatrixClient::IndexAllHoldScroll));
    }

    // On sync the app asks only to CONTINUE: a pass must never start by
    // itself. Calling indexAllRooms() there (resumeOnly false) fails both.
    void aResumeOnSyncNeverStartsAFreshPass()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MessageSearchController model;
        model.setClient(&client);
        QSignalSpy answers(&client, &MatrixClient::searchIndexAllProgress);

        model.resumeIndexAllIfPending();
        QCOMPARE(client.indexAllStarts, QList<bool>{ true });
        QVERIFY(answers.wait(kSignalTimeoutMs));
        QCOMPARE(model.indexAllState(), QStringLiteral("idle"));

        client.mockIndexAllPending = true;   // interrupted last session
        model.resumeIndexAllIfPending();
        QTRY_COMPARE(model.indexAllState(), QStringLiteral("running"));
    }

    // ── The one-time offer after a sign-in ───────────────────────────────
    //
    // Shown once after a sign-in made here, and the answer is remembered —
    // on disk, per account. An offer that ignored the stored answer, or kept
    // it in memory only, fails the re-read.
    void theIndexAllOfferIsMadeOnceAfterASignInMadeHere()
    {
        SettingsManager settings;
        recordAccount(settings, alice());
        MockMatrixClient client;
        QVERIFY(login(client));
        MessageSearchController model;
        model.setClient(&client);
        model.setSettings(&settings);
        QSignalSpy offerChanged(&model,
                                &MessageSearchController::indexAllOfferChanged);

        model.offerIndexAllAfterSignIn(alice(), /*interactiveSignIn=*/true);
        QVERIFY(model.indexAllOffered());
        QCOMPARE(offerChanged.count(), 1);

        model.answerIndexAllOffer(QStringLiteral("no"));
        QVERIFY(!model.indexAllOffered());
        QVERIFY2(client.indexAllStarts.isEmpty(), "No started a pass");
        QCOMPARE(settings.indexAllOfferAnswer(alice()), QStringLiteral("no"));

        // The same account signing in here again is not asked again, even by
        // a fresh settings object reading the file.
        SettingsManager reread;
        model.setSettings(&reread);
        model.offerIndexAllAfterSignIn(alice(), true);
        QVERIFY2(!model.indexAllOffered(), "the offer was made twice");
        model.setSettings(nullptr);
    }

    // Never for a restored session, and a restore (an account switch)
    // withdraws an offer armed for the account before it. An account with no
    // record cannot remember an answer, so it is not asked either. Offering
    // on every loginSucceeded fails the first check.
    void aRestoredSessionIsNeverOfferedIndexAll()
    {
        SettingsManager settings;
        recordAccount(settings, alice());
        MockMatrixClient client;
        QVERIFY(login(client));
        MessageSearchController model;
        model.setClient(&client);
        model.setSettings(&settings);

        model.offerIndexAllAfterSignIn(alice(), /*interactiveSignIn=*/false);
        QVERIFY2(!model.indexAllOffered(), "a restored session was offered");
        QVERIFY2(settings.indexAllOfferAnswer(alice()).isEmpty(),
                 "a restore consumed the one offer");

        model.offerIndexAllAfterSignIn(alice(), true);
        QVERIFY(model.indexAllOffered());
        model.offerIndexAllAfterSignIn(QStringLiteral("@bob:mock.local"), false);
        QVERIFY2(!model.indexAllOffered(),
                 "an offer survived a switch to another account");
        model.answerIndexAllOffer(QStringLiteral("yes"));
        QVERIFY2(client.indexAllStarts.isEmpty(),
                 "a withdrawn offer could still start a pass");

        model.offerIndexAllAfterSignIn(QStringLiteral("@carol:mock.local"), true);
        QVERIFY2(!model.indexAllOffered(),
                 "an account with no record was offered");

        // Sign-out withdraws it too.
        model.offerIndexAllAfterSignIn(alice(), true);
        QVERIFY(model.indexAllOffered());
        client.logout();
        QTRY_VERIFY(!model.indexAllOffered());
        model.setSettings(nullptr);
    }

    // Yes starts the pass (a fresh one, not a resume) and is remembered.
    void yesToTheIndexAllOfferStartsThePass()
    {
        SettingsManager settings;
        recordAccount(settings, alice());
        MockMatrixClient client;
        QVERIFY(login(client));
        MessageSearchController model;
        model.setClient(&client);
        model.setSettings(&settings);

        model.offerIndexAllAfterSignIn(alice(), true);
        QVERIFY(model.indexAllOffered());
        model.answerIndexAllOffer(QStringLiteral("yes"));
        QCOMPARE(client.indexAllStarts, QList<bool>{ false });
        QTRY_COMPARE(model.indexAllState(), QStringLiteral("running"));
        QVERIFY(!model.indexAllOffered());
        QCOMPARE(settings.indexAllOfferAnswer(alice()), QStringLiteral("yes"));
        model.setSettings(nullptr);
    }
};

QTEST_MAIN(MessageSearchControllerTest)
#include "MessageSearchControllerTest.moc"
