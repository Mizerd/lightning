
#include "matrix/MockMatrixClient.h"
#include "models/MediaHistoryModel.h"

#include <QDateTime>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
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
}

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

        Q_EMIT client.mediaHistoryPage(0, QStringLiteral("!r:example.org"), {},
                                       400, 400, 0, false, false);
        QCOMPARE(model.rowCount(), 0);
        QVERIFY2(!model.complete(),
                 "a page that matched nothing must not read as the end of "
                 "history");
        QCOMPARE(model.scannedTotal(), 400);
    }

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
        QCOMPARE(model.rowCount(), 2);

        model.setCategory(QStringLiteral("audio"));
        QCOMPARE(model.rowCount(), 2);

        model.setCategory(QStringLiteral("file"));
        QCOMPARE(model.rowCount(), 1);
        model.setCategory(QStringLiteral("link"));
        QCOMPARE(model.rowCount(), 1);
        model.setCategory(QString());
        QCOMPARE(model.rowCount(), 6);
    }

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

        Q_EMIT client.mediaHistoryPage(0, QStringLiteral("!r:example.org"),
                                       page, 1, 2, 0, false, false);
        QCOMPARE(model.loadedCount(), 2);
    }

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

        model.setQuery(QStringLiteral("$b"));
        QCOMPARE(model.rowCount(), 0);
        model.setSenderFilter(QString());
        QCOMPARE(model.rowCount(), 1);
    }

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
        for (const char *key : { "row", "mediaKey", "filename", "sender",
                                 "timestamp", "mime", "httpUrl", "isImage" })
            QVERIFY2(first.contains(QLatin1String(key)), key);
        QCOMPARE(first.value(QStringLiteral("mediaKey")).toString(),
                 QStringLiteral("$img1"));
        QCOMPARE(images.at(1).toMap().value(QStringLiteral("mediaKey")).toString(),
                 QStringLiteral("$img2"));
        QVERIFY(!first.value(QStringLiteral("mediaKey")).toString()
                     .startsWith(QStringLiteral("mxc://")));
        QVERIFY(first.value(QStringLiteral("timestamp")).toDateTime().isValid());

        QCOMPARE(model.imageIndexForRow(0), 0);
        QCOMPARE(model.imageIndexForRow(2), 1);
        QCOMPARE(model.imageIndexForRow(1), -1);
        QCOMPARE(model.imageIndexForRow(3), -1);
        QCOMPARE(model.imageIndexForRow(99), -1);
        QCOMPARE(model.imageIndexForRow(-1), -1);
    }

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
        model.loadMore();
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
        model.setCategory(QStringLiteral("audio"));
        model.fillViewport();
        QCOMPARE(client.pagesRequested, MediaHistoryModel::kMaxFillPages + 1);
    }

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

        QVERIFY(model.setDateRange(QStringLiteral("2026-10-07"),
                                   QStringLiteral("2026-10-07")));
        QCOMPARE(model.rowCount(), 2);

        QVERIFY(model.setDateRange(QStringLiteral("2026-10-08"), QString()));
        QCOMPARE(model.rowCount(), 1);
        QVERIFY(model.setDateRange(QString(), QStringLiteral("2026-10-04")));
        QCOMPARE(model.rowCount(), 1);
        QVERIFY(model.setDateRange(QString(), QString()));
        QCOMPARE(model.rowCount(), 5);
    }

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
        model.setCategory(QStringLiteral("media"));
        QCOMPARE(model.rowCount(), 3);
        model.setCategory(QStringLiteral("image"));
        QCOMPARE(model.rowCount(), 2);
        model.setCategory(QStringLiteral("video"));
        QCOMPARE(model.rowCount(), 1);
        model.setCategory(QStringLiteral("image"));
        QCOMPARE(model.imageEntries().size(), 2);
    }

    void theDeclaredSizeReachesADelegateByName()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:example.org"));
        QVariantMap wide = keyedEntry(QStringLiteral("$wide"), QStringLiteral("image"));
        wide.insert(QStringLiteral("width"), 3000);
        wide.insert(QStringLiteral("height"), 1000);
        Q_EMIT client.mediaHistoryPage(0, QStringLiteral("!r:example.org"),
                                       QVariantList{ wide }, 1, 1, 0, true, false);
        QCOMPARE(model.rowCount(), 1);

        const auto names = model.roleNames();
        QCOMPARE(names.value(MediaHistoryModel::WidthRole), QByteArray("mediaWidth"));
        QCOMPARE(names.value(MediaHistoryModel::HeightRole), QByteArray("mediaHeight"));
        QVERIFY(!names.values().contains(QByteArray("width")));
        QVERIFY(!names.values().contains(QByteArray("height")));
        QCOMPARE(model.data(model.index(0), MediaHistoryModel::WidthRole).toInt(), 3000);
        QCOMPARE(model.data(model.index(0), MediaHistoryModel::HeightRole).toInt(), 1000);
    }

    void everyRowBuildsAGridTile()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        MediaHistoryModel model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:example.org"));
        QVariantMap wide = keyedEntry(QStringLiteral("$wide"), QStringLiteral("image"));
        wide.insert(QStringLiteral("width"), 3000);
        wide.insert(QStringLiteral("height"), 1000);
        wide.insert(QStringLiteral("encrypted"), true);
        const QVariantList page{
            wide,
            keyedEntry(QStringLiteral("$vid"), QStringLiteral("video")),
            entry(QStringLiteral("$unkeyed"), QStringLiteral("image")),
        };
        Q_EMIT client.mediaHistoryPage(0, QStringLiteral("!r:example.org"),
                                       page, 3, 3, 0, true, false);
        QCOMPARE(model.rowCount(), 3);

        QQmlEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const QQmlError &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty(QStringLiteral("historyModel"),
                                                 &model);
        QQmlComponent component(&engine);
        component.setData(QByteArray(R"QML(
import QtQuick
import MatrixClient
Item {
    width: 360
    height: 360
    QtObject {
        id: bridge
        property bool supported: false
        signal mediaCached(string cacheKey)
        signal mediaRetryable(string cacheKey)
        signal mediaFetchFailed(string cacheKey, string category)
    }
    property var app: ({ mediaBridge: bridge })
    property alias grid: grid
    GridView {
        id: grid
        anchors.fill: parent
        cellWidth: 116
        cellHeight: 116
        model: historyModel
        delegate: MediaBrowserTile {
            width: 116
            height: 116
        }
    }
}
)QML"),
                          QUrl(QStringLiteral("mediahistorygrid.qml")));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *item = qobject_cast<QQuickItem *>(root.get());
        QVERIFY(item);
        QQuickWindow window;
        window.resize(360, 360);
        item->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto *grid = root->property("grid").value<QQuickItem *>();
        QVERIFY(grid);
        QMetaObject::invokeMethod(grid, "forceLayout");
        QTRY_COMPARE(grid->property("count").toInt(), 3);
        for (int row = 0; row < 3; ++row) {
            QQuickItem *tile = nullptr;
            QMetaObject::invokeMethod(grid, "itemAtIndex",
                                      Q_RETURN_ARG(QQuickItem *, tile),
                                      Q_ARG(int, row));
            QVERIFY2(tile, qPrintable(QStringLiteral(
                "row %1 built no tile: a required property the model does "
                "not supply by name. Warnings: %2")
                .arg(row).arg(warnings.join(u'\n'))));
        }
        QQuickItem *first = nullptr;
        QMetaObject::invokeMethod(grid, "itemAtIndex",
                                  Q_RETURN_ARG(QQuickItem *, first),
                                  Q_ARG(int, 0));
        QCOMPARE(first->property("mediaWidth").toInt(), 3000);
        QCOMPARE(first->property("mediaHeight").toInt(), 1000);
        for (const QString &w : std::as_const(warnings))
            QVERIFY2(!w.contains(QStringLiteral("MediaBrowserTile")), qPrintable(w));
    }
};

QTEST_MAIN(MediaHistoryModelTest)
#include "MediaHistoryModelTest.moc"
