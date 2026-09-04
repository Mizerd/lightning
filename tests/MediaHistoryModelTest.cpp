// The media browser's model: what it shows, and what it CLAIMS about how much
// of history it has seen.
//
// The second half is the point. A browser that renders "no images" the same
// way after 60 events and after 40,000 is lying about one of them, and the
// old Media tab did exactly that — it showed whatever the timeline had
// loaded, with a line of prose asking the user to scroll the conversation.
//
// Driven by emitting the client's own signals, so these are the real
// deliveries the Rust backend makes rather than a mock of the model's
// internals.

#include "matrix/MockMatrixClient.h"
#include "models/MediaHistoryModel.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {
constexpr int kTimeoutMs = 3000;

QVariantMap entry(const QString &eventId, const QString &kind,
                  const QString &sender = QStringLiteral("@a:example.org"),
                  const QString &url = QString())
{
    return QVariantMap{
        { QStringLiteral("eventId"), eventId },
        { QStringLiteral("sender"), sender },
        { QStringLiteral("timestampMs"), qint64(1'700'000'000'000) },
        { QStringLiteral("kind"), kind },
        { QStringLiteral("body"), QStringLiteral("body of ") + eventId },
        { QStringLiteral("filename"), eventId + QStringLiteral(".bin") },
        { QStringLiteral("mimetype"), QStringLiteral("application/octet-stream") },
        { QStringLiteral("size"), qint64(10) },
        { QStringLiteral("mxc"), QStringLiteral("mxc://example.org/") + eventId },
        { QStringLiteral("url"), url },
        { QStringLiteral("host"), url.isEmpty() ? QString()
                                                : QStringLiteral("example.org") },
    };
}

bool login(MockMatrixClient &client)
{
    QSignalSpy spy(&client, &MatrixClient::loginSucceeded);
    client.login(QStringLiteral("https://mock.local"),
                 QStringLiteral("alice"), QStringLiteral("x"));
    return spy.wait(kTimeoutMs);
}
} // namespace

class MediaHistoryModelTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // "Nothing here" and "nothing here YET" are different answers, and the
    // model has to be able to tell a view which one it is holding.
    void completenessIsReportedSeparatelyFromEmptiness()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:example.org"));

        QCOMPARE(model.rowCount(), 0);
        QVERIFY(!model.complete());
        QCOMPARE(model.scannedTotal(), 0);

        // A page that found nothing in 400 events is NOT completeness.
        Q_EMIT client.mediaHistoryPage(0, QStringLiteral("!r:example.org"), {},
                                       400, 400, 0, false, false);
        QCOMPARE(model.rowCount(), 0);
        QVERIFY2(!model.complete(),
                 "a page that matched nothing must not read as the end of "
                 "history");
        QCOMPARE(model.scannedTotal(), 400);
    }

    // A failed page leaves the rest of history UNKNOWN. Reporting it as
    // complete would turn a server error into "that is everything".
    void aFailedPageIsNeverCompleteness()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:example.org"));

        Q_EMIT client.mediaHistoryFailed(0, QStringLiteral("!r:example.org"),
                                         QStringLiteral("server said no"));
        QVERIFY(!model.complete());
        QVERIFY(!model.loading());
        QCOMPARE(model.lastError(), QStringLiteral("server said no"));
    }

    // Undecryptable history is a THIRD state. Without it, an encrypted room
    // whose keys are missing just looks like a room with less media.
    void unreadableHistoryIsCountedAndSurfaced()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:example.org"));

        Q_EMIT client.mediaHistoryPage(0, QStringLiteral("!r:example.org"),
                                       { entry(QStringLiteral("$1"),
                                               QStringLiteral("image")) },
                                       50, 50, 7, false, true);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.undecryptableCount(), 7);
        QVERIFY(model.encryptedRoom());
    }

    // Categories. "media" is the combined visual view and must NOT include
    // files or links — the first version of the browser shipped with the
    // model's category never set, so the Media tab listed everything.
    void theMediaCategoryIsVisualOnly()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:example.org"));

        const QVariantList page = {
            entry(QStringLiteral("$img"), QStringLiteral("image")),
            entry(QStringLiteral("$vid"), QStringLiteral("video")),
            entry(QStringLiteral("$aud"), QStringLiteral("audio")),
            entry(QStringLiteral("$voi"), QStringLiteral("voice")),
            entry(QStringLiteral("$fil"), QStringLiteral("file")),
            entry(QStringLiteral("$lnk"), QStringLiteral("link"),
                  QStringLiteral("@a:example.org"),
                  QStringLiteral("https://example.org/x")),
        };
        Q_EMIT client.mediaHistoryPage(0, QStringLiteral("!r:example.org"),
                                       page, 6, 6, 0, true, false);
        QCOMPARE(model.loadedCount(), 6);

        model.setCategory(QStringLiteral("media"));
        QCOMPARE(model.rowCount(), 2);      // image + video only

        // Audio holds recordings too: a reader looking for "that voice note"
        // looks in Audio, not in a category of its own.
        model.setCategory(QStringLiteral("audio"));
        QCOMPARE(model.rowCount(), 2);      // audio + voice

        model.setCategory(QStringLiteral("file"));
        QCOMPARE(model.rowCount(), 1);
        model.setCategory(QStringLiteral("link"));
        QCOMPARE(model.rowCount(), 1);
        model.setCategory(QString());
        QCOMPARE(model.rowCount(), 6);
    }

    // A link event contributes one row PER URL, so the event id alone cannot
    // be the identity — and pages can overlap at a boundary, so something has
    // to stop the same row arriving twice.
    void thePairOfEventAndUrlIsTheIdentity()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:example.org"));

        const QVariantList page = {
            entry(QStringLiteral("$e"), QStringLiteral("link"),
                  QStringLiteral("@a:example.org"),
                  QStringLiteral("https://example.org/one")),
            entry(QStringLiteral("$e"), QStringLiteral("link"),
                  QStringLiteral("@a:example.org"),
                  QStringLiteral("https://example.org/two")),
        };
        Q_EMIT client.mediaHistoryPage(0, QStringLiteral("!r:example.org"),
                                       page, 1, 1, 0, false, false);
        QCOMPARE(model.loadedCount(), 2);

        // The same page again — an overlapping boundary — adds nothing.
        Q_EMIT client.mediaHistoryPage(0, QStringLiteral("!r:example.org"),
                                       page, 1, 2, 0, false, false);
        QCOMPARE(model.loadedCount(), 2);
    }

    // A page for a room the panel has moved away from must not land. The
    // panel can be pointed at another room while a request is in flight.
    void aPageForAnotherRoomIsIgnored()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!mine:example.org"));

        Q_EMIT client.mediaHistoryPage(
            0, QStringLiteral("!other:example.org"),
            { entry(QStringLiteral("$x"), QStringLiteral("image")) },
            1, 1, 0, true, false);
        QCOMPARE(model.rowCount(), 0);
        QVERIFY2(!model.complete(),
                 "another room's completeness must not be adopted");
    }

    // Filters run over what is loaded, and they compose.
    void filtersComposeOverTheLoadedSet()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:example.org"));

        const QVariantList page = {
            entry(QStringLiteral("$a"), QStringLiteral("file"),
                  QStringLiteral("@ann:example.org")),
            entry(QStringLiteral("$b"), QStringLiteral("file"),
                  QStringLiteral("@bob:example.org")),
        };
        Q_EMIT client.mediaHistoryPage(0, QStringLiteral("!r:example.org"),
                                       page, 2, 2, 0, true, false);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.knownSenders().size(), 2);

        model.setSenderFilter(QStringLiteral("@ann:example.org"));
        QCOMPARE(model.rowCount(), 1);

        // Text search looks at the filename too, and composes with sender.
        model.setQuery(QStringLiteral("$b"));
        QCOMPARE(model.rowCount(), 0);
        model.setSenderFilter(QString());
        QCOMPARE(model.rowCount(), 1);
    }

    // One account's attachments must never be listed under the next.
    void signingOutDropsTheBrowse()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:example.org"));
        Q_EMIT client.mediaHistoryPage(
            0, QStringLiteral("!r:example.org"),
            { entry(QStringLiteral("$x"), QStringLiteral("image")) },
            1, 1, 0, true, false);
        QCOMPARE(model.rowCount(), 1);

        client.logout();
        QTest::qWait(50);
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(model.scannedTotal(), 0);
        QVERIFY(model.roomId().isEmpty());
    }
};

QTEST_MAIN(MediaHistoryModelTest)
#include "MediaHistoryModelTest.moc"
