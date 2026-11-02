// Server-side message search: debounced dispatch, roomName resolution against
// the room list, next_batch paging, stale superseding, scope-change clears
// and sign-out invalidation. Encrypted rooms are excluded by the server, which
// this mock cannot observe.

#include "app/SettingsManager.h"
#include "matrix/MockMatrixClient.h"
#include "models/MessageSearchController.h"
#include "profile/UserProfileResolver.h"

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

class PagedLocalSearchClient final : public MockMatrixClient
{
    Q_OBJECT
public:
    QList<int> limits;
    int available = 0;

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

class FixedLocalSearchClient final : public MockMatrixClient
{
    Q_OBJECT
public:
    QVariantList rows;

    quint64 localSearch(const QString &, const QString &, int, int) override
    {
        const quint64 op = ++m_fakeOp;
        const QVariantList answer = rows;
        QTimer::singleShot(0, this, [this, op, answer] {
            Q_EMIT localSearchFinished(op, true, QString(), 3, answer);
        });
        return op;
    }

private:
    quint64 m_fakeOp = 2000000;
};

class EndlessServerClient final : public MockMatrixClient
{
    Q_OBJECT
public:
    int pagesServed = 0;

    quint64 searchMessages(const QString &, const QString &, const QString &,
                           int, const QVariantMap &) override
    {
        const quint64 op = ++m_fakeOp;
        ++pagesServed;
        QTimer::singleShot(0, this, [this, op] {
            Q_EMIT messageSearchFinished(
                op, true,
                QVariantList{ resultRow(QStringLiteral("!general:mock.local"),
                                        QStringLiteral("$p%1").arg(op),
                                        QStringLiteral("plain text")) },
                QStringLiteral("more"), 100, QString());
        });
        return op;
    }

private:
    quint64 m_fakeOp = 3000000;
};
}

class MessageSearchControllerTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_configHome;

    static QString alice() { return QStringLiteral("@alice:mock.local"); }

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

        int before = 0;
        QSignalSpy searches(&client, &MatrixClient::localSearchFinished);
        Q_EMIT client.searchIndexSwept(1, 1, 5, 999, 1);
        QTRY_VERIFY_WITH_TIMEOUT(searches.count() > before, kSignalTimeoutMs);

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
        QCOMPARE(kept, (QStringList{ QStringLiteral("$atLowerBound"),
                                     QStringLiteral("$inside") }));
    }

    void theLinkFilterWorksOnLocalRowsByTheirBody()
    {
        FixedLocalSearchClient client;
        QVERIFY(login(client));
        client.rows = {
            resultRow(QStringLiteral("!general:mock.local"), QStringLiteral("$a"),
                      QStringLiteral("see https://example.org/x for details")),
            resultRow(QStringLiteral("!general:mock.local"), QStringLiteral("$b"),
                      QStringLiteral("no address in here")),
        };
        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        QCOMPARE(model.source(), QStringLiteral("local"));
        model.setFilters(QVariantMap{
            { QStringLiteral("contentTypes"),
              QVariantList{ QStringLiteral("link") } },
        });
        model.setQuery(QStringLiteral("x"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.rowAt(0).value(QStringLiteral("eventId")).toString(),
                 QStringLiteral("$a"));
    }

    void filtersTheLocalIndexCannotEvaluateAreDeclaredAndIgnored()
    {
        FixedLocalSearchClient client;
        QVERIFY(login(client));
        client.rows = {
            resultRow(QStringLiteral("!general:mock.local"), QStringLiteral("$a"),
                      QStringLiteral("hello")),
        };
        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        QVERIFY(model.unsupportedFilters().contains(QStringLiteral("sticker")));
        QVERIFY(model.unsupportedFilters().contains(QStringLiteral("mentions")));
        model.setFilters(QVariantMap{
            { QStringLiteral("contentTypes"),
              QVariantList{ QStringLiteral("sticker") } },
            { QStringLiteral("mentionUserIds"),
              QVariantList{ QStringLiteral("@alice:mock.local") } },
        });
        QCOMPARE(model.ignoredFilters(),
                 (QStringList{ QStringLiteral("sticker"),
                               QStringLiteral("mentions") }));
        model.setQuery(QStringLiteral("hello"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        QCOMPARE(model.rowCount(), 1);

        model.setSource(QStringLiteral("server"));
        QVERIFY(model.unsupportedFilters().isEmpty());
        QVERIFY(model.ignoredFilters().isEmpty());
    }

    void aFilteredServerSearchThatRanOutOfBudgetSaysSo()
    {
        EndlessServerClient client;
        QVERIFY(login(client));
        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        model.setSource(QStringLiteral("server"));
        model.setFilters(QVariantMap{
            { QStringLiteral("contentTypes"),
              QVariantList{ QStringLiteral("image") } },
        });
        model.setQuery(QStringLiteral("plain"));
        QTRY_COMPARE(model.state(), QStringLiteral("no_results"));
        QCOMPARE(client.pagesServed, 4);
        QVERIFY(model.scanLimited());
        QVERIFY(model.canLoadMore());

        model.loadMore();
        QTRY_COMPARE(client.pagesServed, 8);
        QVERIFY(model.scanLimited());

        model.setFilters(QVariantMap());
        model.setQuery(QStringLiteral("plain2"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        QVERIFY(!model.scanLimited());
    }

    void aLocalPageIsFullOnlyRelativeToWhatItAskedFor()
    {
        PagedLocalSearchClient client;
        QVERIFY(login(client));
        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        QCOMPARE(model.source(), QStringLiteral("local"));

        client.available = 50;
        model.setQuery(QStringLiteral("needle"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        QCOMPARE(model.rowCount(), 50);
        QCOMPARE(client.limits, (QList<int>{ 50 }));
        QVERIFY(model.canLoadMore());

        model.loadMore();
        QCOMPARE(model.state(), QStringLiteral("loading_more"));
        QCOMPARE(client.limits, (QList<int>{ 50, 100 }));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        QCOMPARE(model.rowCount(), 50);
        QVERIFY2(!model.canLoadMore(),
                 "a 50-row answer to a 100-row request is an exhausted "
                 "index, and offering More again is the paging spin");

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
        client.available = 60;
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
        QVERIFY(model.canLoadMore());

        model.loadMore();
        QTRY_VERIFY(!model.canLoadMore());
        QCOMPARE(client.limits, (QList<int>{ 50, 100 }));
    }

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
    void aRowWithNoAvatarGetsTheSendersGlobalProfile()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockDisplayNames.insert(QStringLiteral("@bob:mock.local"),
                                       QStringLiteral("Bob"));
        client.mockAvatarUrls.insert(QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("mxc://mock.local/bob"));
        UserProfileResolver profiles;
        profiles.setClient(&client);
        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        model.setProfileResolver(&profiles);
        model.setSource(QStringLiteral("server"));
        client.mockSearchResults = {
            resultRow(QStringLiteral("!general:mock.local"),
                      QStringLiteral("$e1"), QStringLiteral("hello"))
        };
        model.setQuery(QStringLiteral("hello"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        const QModelIndex idx = model.index(0);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        QVERIFY(model.data(idx, MessageSearchController::SenderAvatarUrlRole)
                    .toString().isEmpty());
        QTRY_VERIFY(changed.count() > 0);
        QCOMPARE(model.data(idx, MessageSearchController::SenderAvatarUrlRole)
                     .toString(),
                 QStringLiteral("mxc://mock.local/bob"));
    }

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
        QCOMPARE(model.indexAllEtaMs(), qint64(-1));

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
        QCOMPARE(model.indexAllEtaMs(), qint64(60000));

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

    void aSettledIndexAllPassRequestsFreshIndexStats()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MessageSearchController model;
        model.setClient(&client);

        QSignalSpy stats(&client, &MatrixClient::searchIndexStatsReceived);
        Q_EMIT client.searchIndexAllProgress(1, progress(QStringLiteral("running"), 1, 3));
        QTest::qWait(60);
        QCOMPARE(stats.count(), 0);

        Q_EMIT client.searchIndexAllProgress(1, progress(QStringLiteral("done"), 0, 0));
        QTRY_VERIFY_WITH_TIMEOUT(stats.count() > 0, kSignalTimeoutMs);
    }

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

        MockMatrixClient next;
        model.setClient(&next);
        QCOMPARE(next.indexAllHold, unsigned(MatrixClient::IndexAllHoldScroll));
    }

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

        client.mockIndexAllPending = true;
        model.resumeIndexAllIfPending();
        QTRY_COMPARE(model.indexAllState(), QStringLiteral("running"));
    }

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

        model.offerIndexAllAfterSignIn(alice(),true);
        QVERIFY(model.indexAllOffered());
        QCOMPARE(offerChanged.count(), 1);

        model.answerIndexAllOffer(QStringLiteral("no"));
        QVERIFY(!model.indexAllOffered());
        QVERIFY2(client.indexAllStarts.isEmpty(), "No started a pass");
        QCOMPARE(settings.indexAllOfferAnswer(alice()), QStringLiteral("no"));

        SettingsManager reread;
        model.setSettings(&reread);
        model.offerIndexAllAfterSignIn(alice(), true);
        QVERIFY2(!model.indexAllOffered(), "the offer was made twice");
        model.setSettings(nullptr);
    }

    void aRestoredSessionIsNeverOfferedIndexAll()
    {
        SettingsManager settings;
        recordAccount(settings, alice());
        MockMatrixClient client;
        QVERIFY(login(client));
        MessageSearchController model;
        model.setClient(&client);
        model.setSettings(&settings);

        model.offerIndexAllAfterSignIn(alice(),false);
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

        model.offerIndexAllAfterSignIn(alice(), true);
        QVERIFY(model.indexAllOffered());
        client.logout();
        QTRY_VERIFY(!model.indexAllOffered());
        model.setSettings(nullptr);
    }

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

    void aLocalResultCarriesItsAttachmentFileName()
    {
        FixedLocalSearchClient client;
        QVERIFY(login(client));
        QVariantMap row = resultRow(QStringLiteral("!general:mock.local"),
                                    QStringLiteral("$img"),
                                    QStringLiteral("look at the sunset"));
        row.insert(QStringLiteral("msgtype"), QStringLiteral("m.image"));
        row.insert(QStringLiteral("filename"), QStringLiteral("IMG_2041.png"));
        client.rows = { row };
        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        model.setQuery(QStringLiteral("IMG_2041"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        QCOMPARE(model.data(model.index(0, 0),
                            MessageSearchController::FilenameRole).toString(),
                 QStringLiteral("IMG_2041.png"));
    }
};

QTEST_MAIN(MessageSearchControllerTest)
#include "MessageSearchControllerTest.moc"
