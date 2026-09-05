// A display-name colour the user chooses, carried in their Matrix profile so
// other Lightning clients see it (rust/src/namecolor.rs does the protocol).
//
// The colour VALUES are derived and clamped in lightning::theme and gated by
// ThemeTokensTest. What is tested here is the part that is easy to get wrong
// and expensive when it is: `colorFor()` is called from a QML binding that
// re-evaluates for every name on screen, so it has to dispatch exactly one
// request per user however often it is asked — including for a user who
// turns out to have no colour at all.

#include "matrix/MockMatrixClient.h"
#include "profile/NameColorManager.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {
constexpr int kSignalTimeoutMs = 3000;

bool login(MockMatrixClient &client)
{
    QSignalSpy spy(&client, &MatrixClient::loginSucceeded);
    client.login(QStringLiteral("https://mock.local"),
                 QStringLiteral("alice"), QStringLiteral("x"));
    return spy.wait(kSignalTimeoutMs);
}
} // namespace

class NameColorManagerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // ── One request per user, however often it is asked ───────────────────
    //
    // This is the whole reason the manager exists. A timeline of thirty
    // messages from one sender evaluates the binding thirty times before the
    // first answer can land, so the guard has to be set when the request is
    // SENT and not when the reply arrives.
    void askingRepeatedlyDispatchesExactlyOneFetchPerUser()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockNameColors.insert(QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("#aabbcc"));
        NameColorManager manager;
        manager.setClient(&client);
        QVERIFY(manager.available());

        // Thirty synchronous asks, before any answer can be delivered.
        for (int i = 0; i < 30; ++i)
            manager.colorFor(QStringLiteral("@bob:mock.local"));
        QCOMPARE(client.nameColorFetches, 1);

        QTRY_COMPARE_WITH_TIMEOUT(
            manager.colorFor(QStringLiteral("@bob:mock.local")),
            QStringLiteral("#aabbcc"), kSignalTimeoutMs);
        // ...and still one after the answer landed.
        for (int i = 0; i < 10; ++i)
            manager.colorFor(QStringLiteral("@bob:mock.local"));
        QCOMPARE(client.nameColorFetches, 1);
    }

    // "This user has no colour" is an ANSWER and must be remembered as one.
    // Storing only non-empty replies would re-ask forever for exactly the
    // users who are most common — the ones who never set a colour.
    // "make sure others don't need to restart their client to see the new
    // colour": past the refresh interval the next read re-asks, exactly once,
    // keeps serving the cached answer meanwhile, and a CHANGED reply bumps
    // the revision every name binding depends on.
    void aChangedColourReachesARunningClientOnTheNextRead()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockNameColors.insert(QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("#aabbcc"));
        NameColorManager manager;
        manager.setClient(&client);
        // A long interval for the first answer, so the waits below dispatch
        // nothing of their own; then zero, so the next read is past it.
        manager.setRefreshIntervalForTest(60 * 1000);
        QSignalSpy revisions(&manager, &NameColorManager::revisionChanged);

        manager.colorFor(QStringLiteral("@bob:mock.local"));
        QCOMPARE(client.nameColorFetches, 1);
        QTRY_COMPARE_WITH_TIMEOUT(
            manager.colorFor(QStringLiteral("@bob:mock.local")),
            QStringLiteral("#aabbcc"), kSignalTimeoutMs);
        const int fetchesAfterFirstAnswer = client.nameColorFetches;
        const int revisionsAfterFirstAnswer = revisions.count();
        manager.setRefreshIntervalForTest(0);

        // Bob picks a new colour on his own client. The next read here
        // re-asks once — the reads in between, while it is in flight, do not.
        client.mockNameColors.insert(QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("#123456"));
        QCOMPARE(manager.colorFor(QStringLiteral("@bob:mock.local")),
                 QStringLiteral("#aabbcc"));   // cached answer served meanwhile
        manager.colorFor(QStringLiteral("@bob:mock.local"));
        manager.colorFor(QStringLiteral("@bob:mock.local"));
        QCOMPARE(client.nameColorFetches, fetchesAfterFirstAnswer + 1);
        QTRY_COMPARE_WITH_TIMEOUT(
            manager.colorFor(QStringLiteral("@bob:mock.local")),
            QStringLiteral("#123456"), kSignalTimeoutMs);
        QVERIFY2(revisions.count() > revisionsAfterFirstAnswer,
                 "a changed colour must bump the revision the bindings read");
    }

    // 2026-09-06: "when display name color is changed it should update for
    // other users to see the new color in max 15-30 seconds". A read-driven
    // refresh only fires when a name re-renders; the sweep re-asks the
    // recently read names on its own, and a user nobody has looked at in a
    // while is left alone.
    void aChangedColourReachesEveryoneOnTheSweepWithoutARerender()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockNameColors.insert(QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("#aabbcc"));
        NameColorManager manager;
        manager.setClient(&client);
        manager.setRefreshIntervalForTest(60 * 1000);
        QSignalSpy revisions(&manager, &NameColorManager::revisionChanged);

        manager.colorFor(QStringLiteral("@bob:mock.local"));
        QTRY_COMPARE_WITH_TIMEOUT(
            manager.colorFor(QStringLiteral("@bob:mock.local")),
            QStringLiteral("#aabbcc"), kSignalTimeoutMs);
        const int fetches = client.nameColorFetches;
        const int revisionsBefore = revisions.count();

        // Not due yet: a sweep inside the interval asks nothing.
        manager.sweepForTest();
        QCOMPARE(client.nameColorFetches, fetches);

        // Due, and read recently: the sweep re-asks with NO read in between,
        // and the changed answer bumps the revision every name follows.
        client.mockNameColors.insert(QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("#123456"));
        manager.setRefreshIntervalForTest(0);
        manager.sweepForTest();
        QCOMPARE(client.nameColorFetches, fetches + 1);
        QTRY_COMPARE_WITH_TIMEOUT(revisions.count(), revisionsBefore + 1,
                                  kSignalTimeoutMs);
        QCOMPARE(manager.colorFor(QStringLiteral("@bob:mock.local")),
                 QStringLiteral("#123456"));

        // A user nobody has read within the window is not swept.
        const int fetchesAfterSweep = client.nameColorFetches;
        manager.setRecentReadWindowForTest(0);
        QTest::qWait(5);
        manager.sweepForTest();
        QCOMPARE(client.nameColorFetches, fetchesAfterSweep);
    }

    void aUserWithNoColourIsNotAskedAboutAgain()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        NameColorManager manager;
        manager.setClient(&client);

        QCOMPARE(manager.colorFor(QStringLiteral("@nobody:mock.local")),
                 QString());
        QTRY_COMPARE_WITH_TIMEOUT(client.nameColorFetches, 1, kSignalTimeoutMs);
        QTest::qWait(50);
        for (int i = 0; i < 10; ++i)
            manager.colorFor(QStringLiteral("@nobody:mock.local"));
        QCOMPARE(client.nameColorFetches, 1);
    }

    // Remote text on its way to a QML colour property. Rust drops anything
    // that is not #rrggbb; this is the second gate, and it exists because the
    // value is written by its owner and read by everybody else.
    void aValueThatIsNotAColourNeverReachesTheCache()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        NameColorManager manager;
        manager.setClient(&client);

        const QStringList hostile = {
            QStringLiteral("red"),
            QStringLiteral("#abc"),
            QStringLiteral("#gggggg"),
            QStringLiteral("javascript:alert(1)"),
            QStringLiteral("#aabbcc; background:url(http://evil.example)"),
        };
        int user = 0;
        for (const QString &value : hostile) {
            const QString id =
                QStringLiteral("@hostile%1:mock.local").arg(user++);
            client.mockNameColors.insert(id, value);
            manager.colorFor(id);
            QTest::qWait(40);
            QVERIFY2(manager.colorFor(id).isEmpty(),
                     qPrintable(QStringLiteral("accepted %1").arg(value)));
        }
    }

    void settingOwnColourStoresItAndClearingRemovesIt()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        NameColorManager manager;
        manager.setClient(&client);
        QSignalSpy revisions(&manager, &NameColorManager::revisionChanged);

        manager.setOwnColor(QStringLiteral("#123456"));
        QTRY_COMPARE_WITH_TIMEOUT(manager.ownColor(),
                                  QStringLiteral("#123456"), kSignalTimeoutMs);
        QVERIFY(revisions.count() > 0);

        manager.setOwnColor(QString());
        QTRY_VERIFY_WITH_TIMEOUT(manager.ownColor().isEmpty(), kSignalTimeoutMs);

        // A value that is not a colour is refused before it is sent, and says
        // so rather than failing silently.
        manager.setOwnColor(QStringLiteral("not a colour"));
        QCOMPARE(manager.lastError(), QStringLiteral("invalid_colour"));
        QVERIFY(manager.ownColor().isEmpty());
    }

    // A homeserver with no extended profile fields is a different fact from
    // "nobody has set a colour", and it must stop the asking rather than
    // generate one failed request per user forever.
    void anUnsupportedHomeserverStopsTheAskingAndHidesTheControl()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.nameColorsSupportedOnServer = false;
        NameColorManager manager;
        manager.setClient(&client);

        manager.colorFor(QStringLiteral("@bob:mock.local"));
        QTRY_VERIFY_WITH_TIMEOUT(!manager.supported(), kSignalTimeoutMs);
        const int after = client.nameColorFetches;
        for (int i = 0; i < 10; ++i)
            manager.colorFor(QStringLiteral("@carol:mock.local"));
        QCOMPARE(client.nameColorFetches, after);
    }

    // One account's colours must never surface under another's.
    void signingOutDropsEveryCachedColour()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockNameColors.insert(QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("#aabbcc"));
        NameColorManager manager;
        manager.setClient(&client);
        manager.colorFor(QStringLiteral("@bob:mock.local"));
        QTRY_COMPARE_WITH_TIMEOUT(
            manager.colorFor(QStringLiteral("@bob:mock.local")),
            QStringLiteral("#aabbcc"), kSignalTimeoutMs);

        client.logout();
        QTest::qWait(60);
        // Empty again, and asking re-dispatches rather than serving the old
        // account's answer from cache.
        const int before = client.nameColorFetches;
        QCOMPARE(manager.colorFor(QStringLiteral("@bob:mock.local")), QString());
        QCOMPARE(client.nameColorFetches, before + 1);
    }
};

QTEST_MAIN(NameColorManagerTest)
#include "NameColorManagerTest.moc"
