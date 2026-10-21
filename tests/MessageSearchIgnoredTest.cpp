// Search must not surface messages from an ignored user (m.ignored_user_list),
// in either source: the homeserver does not apply the ignore list to /search.
// Regression for C31-F01. Registration hunk: see the fixer notes
// (message-search-ignored-test, same source set as message-search-test).

#include "matrix/MockMatrixClient.h"
#include "models/MessageSearchController.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {
QVariantMap row(const QString &eventId, const QString &sender)
{
    QVariantMap r;
    r.insert(QStringLiteral("roomId"), QStringLiteral("!general:mock.local"));
    r.insert(QStringLiteral("eventId"), eventId);
    r.insert(QStringLiteral("sender"), sender);
    r.insert(QStringLiteral("senderDisplayName"), sender);
    r.insert(QStringLiteral("senderAvatarUrl"), QString());
    r.insert(QStringLiteral("timestampMs"), qint64(1700000000000));
    r.insert(QStringLiteral("msgtype"), QStringLiteral("m.text"));
    r.insert(QStringLiteral("body"), QStringLiteral("needle ") + eventId);
    return r;
}
} // namespace

class MessageSearchIgnoredTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void serverResultsFromAnIgnoredUserAreHidden()
    {
        MockMatrixClient client;
        QSignalSpy login(&client, &MatrixClient::loginSucceeded);
        client.login(QStringLiteral("https://mock.local"),
                     QStringLiteral("alice"), QStringLiteral("x"));
        QVERIFY(login.wait(3000));
        client.startSync();

        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        model.setSource(QStringLiteral("server"));

        client.mockSearchResults = {
            row(QStringLiteral("$ok"), QStringLiteral("@bob:mock.local")),
            row(QStringLiteral("$bad"), QStringLiteral("@eve:mock.local")),
        };
        // The list arrives as the sync push ModerationController also reads.
        Q_EMIT client.ignoredUsersChanged({ QStringLiteral("@eve:mock.local") });

        model.setQuery(QStringLiteral("needle"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.rowAt(0).value(QStringLiteral("eventId")).toString(),
                 QStringLiteral("$ok"));
    }

    // The SDK's ignore-list subscriber yields on a CHANGE, so a session that
    // resumes with an unchanged list may never push one: the controller asks
    // (MASTER-2 L2). And the answer to that ask must not overwrite a newer
    // push, which the case above covers (its push lands before the answer).
    void aListThatWasNeverPushedIsAskedFor()
    {
        MockMatrixClient client;
        client.mockIgnoredUsers = { QStringLiteral("@eve:mock.local") };
        QSignalSpy login(&client, &MatrixClient::loginSucceeded);
        client.login(QStringLiteral("https://mock.local"),
                     QStringLiteral("alice"), QStringLiteral("x"));
        QVERIFY(login.wait(3000));
        client.startSync();

        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        model.setSource(QStringLiteral("server"));
        client.mockSearchResults = {
            row(QStringLiteral("$ok"), QStringLiteral("@bob:mock.local")),
            row(QStringLiteral("$bad"), QStringLiteral("@eve:mock.local")),
        };
        QSignalSpy answered(&client, &MatrixClient::ignoredUsersReceived);
        QVERIFY(answered.wait(2000));

        model.setQuery(QStringLiteral("needle"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.rowAt(0).value(QStringLiteral("eventId")).toString(),
                 QStringLiteral("$ok"));
    }

    // Recheck N3: an answer to an EARLIER request (op ids rise per client),
    // e.g. one still in flight from the previous account across a sign-out,
    // must not overwrite the list our own, later ask returned.
    void anAnswerToAnOlderRequestCannotReplaceTheList()
    {
        MockMatrixClient client;
        client.mockIgnoredUsers = { QStringLiteral("@eve:mock.local") };
        QSignalSpy login(&client, &MatrixClient::loginSucceeded);
        client.login(QStringLiteral("https://mock.local"),
                     QStringLiteral("alice"), QStringLiteral("x"));
        QVERIFY(login.wait(3000));
        client.startSync();
        const quint64 older = client.requestIgnoredUsers(); // an earlier ask
        QSignalSpy answered(&client, &MatrixClient::ignoredUsersReceived);
        QTRY_COMPARE(answered.size(), 1); // the earlier ask answered first

        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);          // our own, later ask
        model.setSource(QStringLiteral("server"));
        QTRY_COMPARE(answered.size(), 2);
        // The earlier request's answer, delivered late and saying "nobody".
        Q_EMIT client.ignoredUsersReceived(older, true, {});

        client.mockSearchResults = {
            row(QStringLiteral("$ok"), QStringLiteral("@bob:mock.local")),
            row(QStringLiteral("$bad"), QStringLiteral("@eve:mock.local")),
        };
        model.setQuery(QStringLiteral("needle"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        QCOMPARE(model.rowCount(), 1);
    }

    void unignoringShowsThemAgainOnTheNextQuery()
    {
        MockMatrixClient client;
        QSignalSpy login(&client, &MatrixClient::loginSucceeded);
        client.login(QStringLiteral("https://mock.local"),
                     QStringLiteral("alice"), QStringLiteral("x"));
        QVERIFY(login.wait(3000));
        client.startSync();

        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        model.setSource(QStringLiteral("server"));
        client.mockSearchResults = {
            row(QStringLiteral("$bad"), QStringLiteral("@eve:mock.local")),
            row(QStringLiteral("$ok"), QStringLiteral("@bob:mock.local")),
        };
        Q_EMIT client.ignoredUsersChanged({ QStringLiteral("@eve:mock.local") });
        Q_EMIT client.ignoredUsersChanged({});
        model.setQuery(QStringLiteral("needle"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        QCOMPARE(model.rowCount(), 2);
    }
};

QTEST_MAIN(MessageSearchIgnoredTest)
#include "MessageSearchIgnoredTest.moc"
