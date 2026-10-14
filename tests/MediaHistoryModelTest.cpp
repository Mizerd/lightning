// The media browser's model: what it shows, and what it claims about how much
// of history it has seen ("no images" after 60 events and after 40,000 are
// different claims).
//
// Driven by emitting the client's own signals, as the Rust backend delivers
// them.

#include "matrix/MockMatrixClient.h"
#include "models/MediaHistoryModel.h"

#include <QDateTime>
#include <QSignalSpy>
#include <QTimeZone>
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

/// `entry()` plus the media registry key, which the Rust scanner sets to the
/// event id. `entry()` omits it (theRegistryKeyRidesTheRow needs a row
/// without one).
QVariantMap keyedEntry(const QString &eventId, const QString &kind,
                       const QString &sender = QStringLiteral("@a:example.org"))
{
    QVariantMap out = entry(eventId, kind, sender);
    out.insert(QStringLiteral("mediaKey"), eventId);
    return out;
}

bool login(MockMatrixClient &client)
{
    QSignalSpy spy(&client, &MatrixClient::loginSucceeded);
    client.login(QStringLiteral("https://mock.local"),
                 QStringLiteral("alice"), QStringLiteral("x"));
    return spy.wait(kTimeoutMs);
}
} // namespace

/// A client that can walk history and counts the pages it is asked for.
class WalkingClient : public MockMatrixClient
{
public:
    bool supportsMediaHistory() const override { return true; }
    quint64 requestMediaHistoryPage(const QString &, int, bool restart) override
    {
        restarts.append(restart);
        ++pagesRequested;
        lastOp = ++nextOp;
        return lastOp;
    }
    QList<bool> restarts;
    int pagesRequested = 0;
    quint64 lastOp = 0;
    quint64 nextOp = 100;
};

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

    // Tiles fetch through the media registry when the scanner registered a
    // key (the only path that decrypts an encrypted thumbnail). The key rides
    // the row as `mediaKey`; a row without one keeps the plain mxc route.
    void theRegistryKeyRidesTheRow()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:example.org"));
        QVariantMap keyed = entry(QStringLiteral("$enc"), QStringLiteral("image"));
        keyed.insert(QStringLiteral("mediaKey"), QStringLiteral("$enc"));
        keyed.insert(QStringLiteral("encrypted"), true);
        const QVariantList page = { keyed, entry(QStringLiteral("$plain"), QStringLiteral("image")) };
        Q_EMIT client.mediaHistoryPage(0, QStringLiteral("!r:example.org"),
                                       page, 2, 2, 0, false, false);
        QCOMPARE(model.loadedCount(), 2);
        QCOMPARE(model.data(model.index(0), MediaHistoryModel::MediaKeyRole).toString(),
                 QStringLiteral("$enc"));
        QVERIFY(model.roleNames().values().contains(QByteArray("mediaKey")));
        QVERIFY(model.data(model.index(1), MediaHistoryModel::MediaKeyRole).toString().isEmpty());
    }

    // A page for a room the panel has moved away from must not land.
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

    // The search placeholder promises captions: a caption is the event body of
    // an attachment whose name travels in `filename`, and an edited caption
    // arrives already folded into `body` by the scanner.
    void theSearchMatchesTheCaptionNotOnlyTheFilename()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:example.org"));

        QVariantMap captioned = entry(QStringLiteral("$c"), QStringLiteral("image"));
        captioned.insert(QStringLiteral("filename"), QStringLiteral("IMG_0001.jpg"));
        captioned.insert(QStringLiteral("body"),
                         QStringLiteral("Sunset over the harbour"));
        QVariantMap plain = entry(QStringLiteral("$p"), QStringLiteral("image"));
        plain.insert(QStringLiteral("filename"), QStringLiteral("IMG_0002.jpg"));
        plain.insert(QStringLiteral("body"), QStringLiteral("IMG_0002.jpg"));
        Q_EMIT client.mediaHistoryPage(0, QStringLiteral("!r:example.org"),
                                       { captioned, plain }, 2, 2, 0, true, false);
        QCOMPARE(model.rowCount(), 2);

        model.setQuery(QStringLiteral("harbour"));
        QCOMPARE(model.rowCount(), 1);
        model.setQuery(QStringLiteral("HARBOUR"));
        QCOMPARE(model.rowCount(), 1);
        model.setQuery(QStringLiteral("IMG_0001"));
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

    // Clicking a picture in the Media tab opens that picture. The viewer takes
    // the list and the index, and this model owns both so QML cannot
    // re-derive an order that drifts from the one on screen.
    void imageEntriesCarryTheViewerShapeInViewOrder()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:example.org"));
        Q_EMIT client.mediaHistoryPage(
            0, QStringLiteral("!r:example.org"),
            { keyedEntry(QStringLiteral("$img1"), QStringLiteral("image")),
              keyedEntry(QStringLiteral("$vid"), QStringLiteral("video")),
              keyedEntry(QStringLiteral("$img2"), QStringLiteral("image")),
              keyedEntry(QStringLiteral("$file"), QStringLiteral("file")) },
            4, 4, 0, true, false);
        QCOMPARE(model.rowCount(), 4);

        const QVariantList images = model.imageEntries();
        QCOMPARE(images.size(), 2);
        const QVariantMap first = images.at(0).toMap();
        // The keys ImageViewerOverlay actually binds to. A shape mismatch
        // here is a blank viewer, not a compile error.
        for (const char *key : { "row", "mediaKey", "filename", "sender",
                                 "timestamp", "mime", "httpUrl", "isImage" })
            QVERIFY2(first.contains(QLatin1String(key)), key);
        QCOMPARE(first.value(QStringLiteral("mediaKey")).toString(),
                 QStringLiteral("$img1"));
        QCOMPARE(images.at(1).toMap().value(QStringLiteral("mediaKey")).toString(),
                 QStringLiteral("$img2"));
        // The key is the media registry key, never the mxc: an encrypted
        // room's mxc is not fetchable through the bridge at all.
        QVERIFY(!first.value(QStringLiteral("mediaKey")).toString()
                     .startsWith(QStringLiteral("mxc://")));
        QVERIFY(first.value(QStringLiteral("timestamp")).toDateTime().isValid());

        // The index is by SHOWN row, and the rows between the images do not
        // shift it. Row 2 is $img2 — the second image, index 1.
        QCOMPARE(model.imageIndexForRow(0), 0);
        QCOMPARE(model.imageIndexForRow(2), 1);
        // A non-image row has no index into the image list, and must not be
        // silently rounded to a neighbouring picture.
        QCOMPARE(model.imageIndexForRow(1), -1);
        QCOMPARE(model.imageIndexForRow(3), -1);
        QCOMPARE(model.imageIndexForRow(99), -1);
        QCOMPARE(model.imageIndexForRow(-1), -1);
    }

    // The index follows the FILTERED view, because that is what the user is
    // looking at. A filter that hides the first picture must not leave every
    // click one place off.
    void theImageIndexFollowsTheFilteredView()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:example.org"));
        Q_EMIT client.mediaHistoryPage(
            0, QStringLiteral("!r:example.org"),
            { keyedEntry(QStringLiteral("$a"), QStringLiteral("image"),
                          QStringLiteral("@alice:example.org")),
              keyedEntry(QStringLiteral("$b"), QStringLiteral("image"),
                          QStringLiteral("@bob:example.org")),
              keyedEntry(QStringLiteral("$c"), QStringLiteral("image"),
                          QStringLiteral("@bob:example.org")) },
            3, 3, 0, true, false);
        QCOMPARE(model.imageEntries().size(), 3);

        model.setSenderFilter(QStringLiteral("@bob:example.org"));
        const QVariantList images = model.imageEntries();
        QCOMPARE(images.size(), 2);
        QCOMPARE(images.at(0).toMap().value(QStringLiteral("mediaKey")).toString(),
                 QStringLiteral("$b"));
        QCOMPARE(model.imageIndexForRow(0), 0);
        QCOMPARE(model.imageIndexForRow(1), 1);
    }

    // The reported "4 items in the 60 messages read so far, and no way to get
    // more": a short list never scrolls, so scroll-driven paging never fired.
    // "Show all media" must keep fetching pages by itself, to the end.
    // The backend keeps one cursor per room for the session. A cleared model
    // (closing and reopening Room info) must begin at the live edge, or the
    // exhausted cursor answers "nothing older" and the grid shows 0 items.
    // Fails on the old code: every request was restart=false.
    void reopeningARoomRestartsTheBackendWalk()
    {
        WalkingClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        const QString room = QStringLiteral("!r:example.org");
        model.setRoomId(room);
        model.loadMore();
        QCOMPARE(client.restarts, QList<bool>{true});
        Q_EMIT client.mediaHistoryPage(
            client.lastOp, room,
            { keyedEntry(QStringLiteral("$a"), QStringLiteral("image")) },
            10, 10, 0, false, false);
        model.loadMore(); // continues the same walk
        QCOMPARE(client.restarts.last(), false);
        Q_EMIT client.mediaHistoryPage(client.lastOp, room, {}, 5, 15, 0, true,
                                       false);

        model.setRoomId(QString());
        model.setRoomId(room);
        model.loadMore();
        QCOMPARE(client.restarts.last(), true);
    }

    void showAllWalksPagesUntilTheStartOfHistory()
    {
        WalkingClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        const QString room = QStringLiteral("!r:example.org");
        model.setRoomId(room);

        model.loadAll();
        QVERIFY(model.walking());
        QCOMPARE(client.pagesRequested, 1);
        for (int page = 1; page <= 3; ++page) {
            Q_EMIT client.mediaHistoryPage(
                client.lastOp, room,
                { keyedEntry(QStringLiteral("$p%1").arg(page),
                             QStringLiteral("image")) },
                60, 60 * page, 0, false, false);
            QCOMPARE(client.pagesRequested, page + 1);
            QVERIFY(model.walking());
        }
        Q_EMIT client.mediaHistoryPage(client.lastOp, room, {}, 10, 190, 0,
                                       true, false);
        QVERIFY(!model.walking());
        QVERIFY(model.complete());
        QCOMPARE(model.shownCount(), 3);
        QCOMPARE(client.pagesRequested, 4);
    }

    void cancellingTheWalkStopsAskingForPages()
    {
        WalkingClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        const QString room = QStringLiteral("!r:example.org");
        model.setRoomId(room);
        model.loadAll();
        model.cancelWalk();
        QVERIFY(!model.walking());
        // The page already in flight lands, and nothing follows it.
        Q_EMIT client.mediaHistoryPage(client.lastOp, room, {}, 60, 60, 0,
                                       false, false);
        QCOMPARE(client.pagesRequested, 1);
    }

    void aWalkIsBoundedAndStopsOnAnError()
    {
        WalkingClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        const QString room = QStringLiteral("!r:example.org");
        model.setRoomId(room);
        model.loadAll();
        for (int i = 0; i < MediaHistoryModel::kMaxWalkPages + 10
                        && model.walking(); ++i)
            Q_EMIT client.mediaHistoryPage(client.lastOp, room, {}, 60, 60 * i,
                                           0, false, false);
        QVERIFY(!model.walking());
        QCOMPARE(client.pagesRequested, MediaHistoryModel::kMaxWalkPages);

        model.loadAll();
        QVERIFY(model.walking());
        Q_EMIT client.mediaHistoryFailed(client.lastOp, room,
                                         QStringLiteral("boom"));
        QVERIFY(!model.walking());
        QVERIFY(!model.complete());
    }

    // A reset sends a scrolled view back to the top, so an older page must
    // arrive as an insertion after the rows already shown.
    void anOlderPageIsAppendedNotReset()
    {
        WalkingClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        const QString room = QStringLiteral("!r:example.org");
        model.setRoomId(room);
        model.loadMore();
        Q_EMIT client.mediaHistoryPage(
            client.lastOp, room,
            { keyedEntry(QStringLiteral("$a"), QStringLiteral("image")),
              keyedEntry(QStringLiteral("$b"), QStringLiteral("image")) },
            60, 60, 0, false, false);
        QCOMPARE(model.rowCount(), 2);
        const QString firstId = model.entryAt(0).value(QStringLiteral("eventId")).toString();

        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy inserts(&model, &QAbstractItemModel::rowsInserted);
        model.loadMore();
        Q_EMIT client.mediaHistoryPage(
            client.lastOp, room,
            { keyedEntry(QStringLiteral("$c"), QStringLiteral("image")) },
            60, 120, 0, false, false);
        QCOMPARE(resets.count(), 0);
        QCOMPARE(inserts.count(), 1);
        QCOMPARE(inserts.first().at(1).toInt(), 2);
        QCOMPARE(inserts.first().at(2).toInt(), 2);
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.entryAt(0).value(QStringLiteral("eventId")).toString(), firstId);
    }

    void aViewThatCannotScrollFillsItselfWithinABound()
    {
        WalkingClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        const QString room = QStringLiteral("!r:example.org");
        model.setRoomId(room);
        for (int i = 0; i < MediaHistoryModel::kMaxFillPages + 5; ++i) {
            model.fillViewport();
            if (model.loading())
                Q_EMIT client.mediaHistoryPage(client.lastOp, room, {}, 60,
                                               60 * (i + 1), 0, false, false);
        }
        QCOMPARE(client.pagesRequested, MediaHistoryModel::kMaxFillPages);
        // A filter change is a new question and gets a fresh budget.
        model.setCategory(QStringLiteral("audio"));
        model.fillViewport();
        QCOMPARE(client.pagesRequested, MediaHistoryModel::kMaxFillPages + 1);
    }

    // ── Date range ───────────────────────────────────────────────────────
    //
    // Bounds are LOCAL calendar days: From = start of its day, To = END of its
    // day. Before this the To bound compared against the raw QDateTime, so a
    // message at 15:00 on the "To" day was excluded.
    void theToDayIsIncludedUpToItsLastMillisecond()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:example.org"));

        const auto at = [](int d, int h, int m, int s, int ms) {
            return QDateTime(QDate(2026, 10, d), QTime(h, m, s, ms))
                .toMSecsSinceEpoch();
        };
        const auto row = [&](const QString &id, qint64 ts) {
            QVariantMap r = entry(id, QStringLiteral("file"));
            r.insert(QStringLiteral("timestampMs"), ts);
            return r;
        };
        const QVariantList page{
            row(QStringLiteral("$beforeFrom"), at(4, 23, 59, 59, 999)),
            row(QStringLiteral("$fromStart"), at(5, 0, 0, 0, 0)),
            row(QStringLiteral("$afternoonOfTo"), at(7, 15, 0, 0, 0)),
            row(QStringLiteral("$toLastMs"), at(7, 23, 59, 59, 999)),
            row(QStringLiteral("$afterTo"), at(8, 0, 0, 0, 0)),
        };
        Q_EMIT client.mediaHistoryPage(0, QStringLiteral("!r:example.org"),
                                       page, 5, 5, 0, true, false);
        QCOMPARE(model.rowCount(), 5);

        QVERIFY(model.setDateRange(QStringLiteral("2026-10-05"),
                                   QStringLiteral("2026-10-07")));
        QCOMPARE(model.rowCount(), 3);
        QStringList kept;
        for (int i = 0; i < model.rowCount(); ++i)
            kept << model.entryAt(i).value(QStringLiteral("eventId")).toString();
        kept.sort();
        QCOMPARE(kept, (QStringList{ QStringLiteral("$afternoonOfTo"),
                                     QStringLiteral("$fromStart"),
                                     QStringLiteral("$toLastMs") }));

        // The same day twice is that whole day.
        QVERIFY(model.setDateRange(QStringLiteral("2026-10-07"),
                                   QStringLiteral("2026-10-07")));
        QCOMPARE(model.rowCount(), 2);

        // Open-ended on either side.
        QVERIFY(model.setDateRange(QStringLiteral("2026-10-08"), QString()));
        QCOMPARE(model.rowCount(), 1);
        QVERIFY(model.setDateRange(QString(), QStringLiteral("2026-10-04")));
        QCOMPARE(model.rowCount(), 1);
        QVERIFY(model.setDateRange(QString(), QString()));
        QCOMPARE(model.rowCount(), 5);
    }

    // A bad range is refused and changes nothing, so the view never goes
    // blank or unfiltered because of a typo.
    void anInvalidDateRangeIsRefusedAndKeepsTheLastGoodOne()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        QVERIFY(model.setDateRange(QStringLiteral("2026-10-05"),
                                   QStringLiteral("2026-10-07")));
        const QDateTime from = model.fromDate();
        const QDateTime to = model.toDate();
        QVERIFY(!model.setDateRange(QStringLiteral("2026-02-30"), QString()));
        QVERIFY(!model.setDateRange(QStringLiteral("5 Oct"), QString()));
        QVERIFY(!model.setDateRange(QStringLiteral("2026-10-09"),
                                    QStringLiteral("2026-10-01")));
        QCOMPARE(model.fromDate(), from);
        QCOMPARE(model.toDate(), to);
    }

    // The bound is the viewer's local day even when the value arrives in UTC.
    void aUtcValueIsReadAsItsLocalDay()
    {
        MediaHistoryModel model;
        model.setToDate(QDateTime(QDate(2026, 10, 7), QTime(12, 0), QTimeZone::UTC));
        const QDate localDay =
            QDateTime(QDate(2026, 10, 7), QTime(12, 0), QTimeZone::UTC)
                .toLocalTime().date();
        QCOMPARE(QDateTime::fromMSecsSinceEpoch(model.toBoundMs()).date(),
                 localDay);
        QCOMPARE(QDateTime::fromMSecsSinceEpoch(model.toBoundMs()).time(),
                 QTime(23, 59, 59, 999));
    }

    // ── GIF and sticker views ────────────────────────────────────────────
    void gifsAndStickersHaveTheirOwnViewsAndGifsStayInTheVisualOnes()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:example.org"));

        QVariantMap gifImage = keyedEntry(QStringLiteral("$gifimg"), QStringLiteral("image"));
        gifImage.insert(QStringLiteral("gif"), true);
        QVariantMap gifVideo = keyedEntry(QStringLiteral("$gifvid"), QStringLiteral("video"));
        gifVideo.insert(QStringLiteral("gif"), true);
        const QVariantList page{
            gifImage, gifVideo,
            keyedEntry(QStringLiteral("$img"), QStringLiteral("image")),
            keyedEntry(QStringLiteral("$stk"), QStringLiteral("sticker")),
        };
        Q_EMIT client.mediaHistoryPage(0, QStringLiteral("!r:example.org"),
                                       page, 4, 4, 0, true, false);

        model.setCategory(QStringLiteral("gif"));
        QCOMPARE(model.rowCount(), 2);
        model.setCategory(QStringLiteral("sticker"));
        QCOMPARE(model.rowCount(), 1);
        // Stickers are not "media"; GIFs are.
        model.setCategory(QStringLiteral("media"));
        QCOMPARE(model.rowCount(), 3);
        model.setCategory(QStringLiteral("image"));
        QCOMPARE(model.rowCount(), 2);
        model.setCategory(QStringLiteral("video"));
        QCOMPARE(model.rowCount(), 1);
        // The viewer still gets the GIF image.
        model.setCategory(QStringLiteral("image"));
        QCOMPARE(model.imageEntries().size(), 2);
    }
};

QTEST_MAIN(MediaHistoryModelTest)
#include "MediaHistoryModelTest.moc"
