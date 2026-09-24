// Server-side message search: debounced dispatch, roomName resolution against
// the room list, next_batch paging, stale superseding, scope-change clears
// and sign-out invalidation. Encrypted rooms are excluded by the server, which
// this mock cannot observe.

#include "matrix/MockMatrixClient.h"
#include "models/MessageSearchController.h"

#include <QSignalSpy>
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
};

QTEST_MAIN(MessageSearchControllerTest)
#include "MessageSearchControllerTest.moc"
