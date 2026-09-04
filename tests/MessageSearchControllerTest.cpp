// v0.7.x server-side message search: debounced dispatch, roomName
// resolution against the authoritative room list, next_batch paging, stale
// superseding, scope-change clears, and sign-out invalidation. Encrypted
// rooms are excluded SERVER-side (the server cannot search ciphertext);
// that exclusion is a protocol fact the UI discloses, not something this
// model could observe against the mock.

#include "matrix/MockMatrixClient.h"
#include "models/MessageSearchController.h"

#include <QSignalSpy>
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
        // THIS SUITE IS ABOUT SERVER SEARCH. Since the local index landed the
        // controller prefers "local" wherever a backend has one, and
        // MockMatrixClient now does — so a case that means the /search path
        // has to say so, or it silently starts testing the other one.
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
        // THIS SUITE IS ABOUT SERVER SEARCH. Since the local index landed the
        // controller prefers "local" wherever a backend has one, and
        // MockMatrixClient now does — so a case that means the /search path
        // has to say so, or it silently starts testing the other one.
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
        // THIS SUITE IS ABOUT SERVER SEARCH. Since the local index landed the
        // controller prefers "local" wherever a backend has one, and
        // MockMatrixClient now does — so a case that means the /search path
        // has to say so, or it silently starts testing the other one.
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
        // THIS SUITE IS ABOUT SERVER SEARCH. Since the local index landed the
        // controller prefers "local" wherever a backend has one, and
        // MockMatrixClient now does — so a case that means the /search path
        // has to say so, or it silently starts testing the other one.
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
        // THIS SUITE IS ABOUT SERVER SEARCH. Since the local index landed the
        // controller prefers "local" wherever a backend has one, and
        // MockMatrixClient now does — so a case that means the /search path
        // has to say so, or it silently starts testing the other one.
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
        // THIS SUITE IS ABOUT SERVER SEARCH. Since the local index landed the
        // controller prefers "local" wherever a backend has one, and
        // MockMatrixClient now does — so a case that means the /search path
        // has to say so, or it silently starts testing the other one.
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
        // THIS SUITE IS ABOUT SERVER SEARCH. Since the local index landed the
        // controller prefers "local" wherever a backend has one, and
        // MockMatrixClient now does — so a case that means the /search path
        // has to say so, or it silently starts testing the other one.
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
    // Found by driving the real client against a real homeserver: with the
    // find bar open on an encrypted room, the coverage line went 31 -> 32
    // while the list below it still read "No messages found in this room's
    // history" for the message that had just been indexed. Only editing the
    // query revealed it.
    //
    // Two paths, and the second is the one that was broken. A sweep runs on
    // a five-minute timer underneath whatever is on screen; and an explicit
    // "Index this room" used to refresh ONLY when it wrote something, so a
    // sweep that had already indexed the message seconds earlier left the
    // button doing visibly nothing.
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

        // An explicit "Index this room" refreshes even when it writes
        // NOTHING. The mock's deepen answers written == 0, which is exactly
        // the live case: the sweep had already indexed the message, so the
        // button used to complete while the stale list stayed on screen.
        before = searches.count();
        model.indexRoomHistory(QStringLiteral("!general:mock.local"));
        QTRY_VERIFY_WITH_TIMEOUT(searches.count() > before, kSignalTimeoutMs);
    }

    // ── The date bounds, ISOLATED ────────────────────────────────────────
    //
    // The combined-filter case above sets afterMs/beforeMs alongside four
    // other filters, and its two negative rows already fail on msgtype and on
    // mentions — so it passes unchanged with both date clauses DELETED. This
    // one differs ONLY by timestamp and sets ONLY the bounds, so it fails if
    // either clause goes or flips its comparison.
    //
    // The upper bound is deliberately EXCLUSIVE (`>= m_beforeMs` rejects),
    // which is what makes "before 1 Jan" mean the whole of 31 Dec and not one
    // millisecond of 1 Jan; `atUpperBound` pins that, and it is the assertion
    // an inclusive comparison would break.
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

    // ── The two producers must spell the sender the same way ─────────────
    //
    // MessageSearchController is shared by SERVER search and the LOCAL index,
    // and it reads one key: senderDisplayName. Both local-search producers
    // (the Rust bridge and this mock) used to emit "senderName" instead, so
    // every local result reached the find bar with an empty sender — which
    // rendered in the results list and in the Accessible name, and which no
    // unit test could see because none of them read a local row's sender.
    //
    // Asserts the VALUE, not merely that a role exists: an empty string is
    // exactly what the defect produced.
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
