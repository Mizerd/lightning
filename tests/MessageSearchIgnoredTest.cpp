
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
}

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
        Q_EMIT client.ignoredUsersChanged({ QStringLiteral("@eve:mock.local") });

        model.setQuery(QStringLiteral("needle"));
        QTRY_COMPARE(model.state(), QStringLiteral("results"));
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.rowAt(0).value(QStringLiteral("eventId")).toString(),
                 QStringLiteral("$ok"));
    }

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

    void anAnswerToAnOlderRequestCannotReplaceTheList()
    {
        MockMatrixClient client;
        client.mockIgnoredUsers = { QStringLiteral("@eve:mock.local") };
        QSignalSpy login(&client, &MatrixClient::loginSucceeded);
        client.login(QStringLiteral("https://mock.local"),
                     QStringLiteral("alice"), QStringLiteral("x"));
        QVERIFY(login.wait(3000));
        client.startSync();
        const quint64 older = client.requestIgnoredUsers();
        QSignalSpy answered(&client, &MatrixClient::ignoredUsersReceived);
        QTRY_COMPARE(answered.size(), 1);

        MessageSearchController model;
        model.setDebounceMs(0);
        model.setClient(&client);
        model.setSource(QStringLiteral("server"));
        QTRY_COMPARE(answered.size(), 2);
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
