// Sharing a PLACE: `m.location` (MSC3488) and live beacons (MSC3672).
//
// The one rule everything else rests on: a geo URI is a field of a message
// ANYONE can send, so a point that does not parse — or is not on Earth —
// must leave the coordinates ABSENT rather than at 0,0. Zero is a real spot
// in the Atlantic, and a UI reading it would draw a confident link to the
// wrong place, which is worse than showing nothing.

#include "matrix/MockMatrixClient.h"
#include "matrix/RustTimelineIngest.h"
#include "models/MessageComposer.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest/QtTest>

namespace {

QJsonObject locationItem(const QString &body, double lat, double lon,
                         bool withPoint = true)
{
    QJsonObject o{
        { QStringLiteral("event_id"), QStringLiteral("$loc:example.org") },
        { QStringLiteral("sender"), QStringLiteral("@a:example.org") },
        { QStringLiteral("msgtype"), QStringLiteral("location") },
        { QStringLiteral("body"), body },
    };
    if (withPoint) {
        o.insert(QStringLiteral("locationLat"), lat);
        o.insert(QStringLiteral("locationLon"), lon);
    }
    return o;
}

// A client that records what it was asked to send.
class SendingClient final : public MockMatrixClient
{
    Q_OBJECT
public:
    bool supportsSendLocation() const override { return true; }
    int calls = 0;
    double lastLat = 0.0;
    double lastLon = 0.0;
    QString lastDescription;
    void sendLocation(const QString &, double lat, double lon,
                      const QString &description) override
    {
        ++calls;
        lastLat = lat;
        lastLon = lon;
        lastDescription = description;
    }
};

const QString kRoom = QStringLiteral("!r:example.org");

} // namespace

class LocationMessageTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // A location arrives as its OWN row kind, not as text or a file.
    void aLocationIsItsOwnRowKind()
    {
        const TimelineEvent e = matrix::rust_timeline::eventFromItemJson(
            locationItem(QStringLiteral("Big Ben"), 51.5008, -0.1247), kRoom);
        QCOMPARE(e.type, TimelineEvent::Location);
        QVERIFY(e.locationHasPoint);
        QVERIFY(qAbs(e.locationLat - 51.5008) < 1e-9);
        QVERIFY(qAbs(e.locationLon - (-0.1247)) < 1e-9);
        // The body survives: it is what every surface that does not know
        // what a location is — search, notifications, the room-list preview
        // — shows instead.
        QCOMPARE(e.body, QStringLiteral("Big Ben"));
    }

    // THE RULE. Absent coordinates must not become 0,0.
    void anUnreadablePointLeavesTheCoordinatesAbsentRatherThanAtZero()
    {
        const TimelineEvent e = matrix::rust_timeline::eventFromItemJson(
            locationItem(QStringLiteral("Somewhere"), 0, 0, false), kRoom);
        QCOMPARE(e.type, TimelineEvent::Location);
        QVERIFY2(!e.locationHasPoint,
                 "an absent point must not read as a valid one");
    }

    // ...and a genuine 0,0 is a real place: the equator at the prime
    // meridian. It must NOT be mistaken for the absent case, which is why
    // the flag exists rather than a magic-value check.
    void aGenuineZeroPointIsARealPlace()
    {
        const TimelineEvent e = matrix::rust_timeline::eventFromItemJson(
            locationItem(QStringLiteral("Null Island"), 0.0, 0.0), kRoom);
        QVERIFY2(e.locationHasPoint,
                 "0,0 is the equator at the prime meridian, not 'no point'");
        QCOMPARE(e.locationLat, 0.0);
        QCOMPARE(e.locationLon, 0.0);
    }

    void aLiveShareCarriesWhetherItIsStillCurrent()
    {
        QJsonObject live = locationItem(QStringLiteral("On my way"),
                                        51.5, -0.12);
        live.insert(QStringLiteral("locationLive"), true);
        live.insert(QStringLiteral("locationLiveActive"), true);
        const TimelineEvent a = matrix::rust_timeline::eventFromItemJson(live, kRoom);
        QVERIFY(a.locationLive);
        QVERIFY(a.locationLiveActive);

        // An EXPIRED live share is a different thing from a current one:
        // rendering the second as the first tells the reader somebody is
        // somewhere they may have left an hour ago.
        live.insert(QStringLiteral("locationLiveActive"), false);
        const TimelineEvent b = matrix::rust_timeline::eventFromItemJson(live, kRoom);
        QVERIFY(b.locationLive);
        QVERIFY(!b.locationLiveActive);
    }

    void theOptionalFieldsCrossWhenPresentAndAreEmptyWhenNot()
    {
        QJsonObject full = locationItem(QStringLiteral("Here"), 1.0, 2.0);
        full.insert(QStringLiteral("locationUncertaintyM"), 35.0);
        full.insert(QStringLiteral("locationDescription"),
                    QStringLiteral("The pub"));
        full.insert(QStringLiteral("locationAsset"), QStringLiteral("m.pin"));
        const TimelineEvent e = matrix::rust_timeline::eventFromItemJson(full, kRoom);
        QCOMPARE(e.locationUncertaintyM, 35.0);
        QCOMPARE(e.locationDescription, QStringLiteral("The pub"));
        QCOMPARE(e.locationAsset, QStringLiteral("m.pin"));

        const TimelineEvent bare = matrix::rust_timeline::eventFromItemJson(
            locationItem(QStringLiteral("Here"), 1.0, 2.0), kRoom);
        QCOMPARE(bare.locationUncertaintyM, 0.0);
        QVERIFY(bare.locationDescription.isEmpty());
    }

    // The composer refuses the same points the bridge would, so the UI can
    // disable its own button rather than sending and being told no.
    void theComposerRefusesAPointThatIsNotOnEarth()
    {
        SendingClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!r:example.org"));
        QVERIFY(composer.locationSupported());

        QVERIFY(composer.locationIsValid(51.5, -0.12));
        QVERIFY(composer.locationIsValid(0.0, 0.0));
        QVERIFY(composer.locationIsValid(-90.0, -180.0));
        QVERIFY(composer.locationIsValid(90.0, 180.0));
        QVERIFY(!composer.locationIsValid(91.0, 0.0));
        QVERIFY(!composer.locationIsValid(0.0, 181.0));
        QVERIFY(!composer.locationIsValid(qQNaN(), 0.0));
        QVERIFY(!composer.locationIsValid(qInf(), 0.0));

        composer.sendLocation(91.0, 0.0, QStringLiteral("nowhere"));
        QCOMPARE(client.calls, 0);

        composer.sendLocation(51.5, -0.12, QStringLiteral("The pub"));
        QCOMPARE(client.calls, 1);
        QCOMPARE(client.lastDescription, QStringLiteral("The pub"));
    }

    // A location sent while the composer holds a thread root would land in
    // the ROOM — the bridge's send path uses the room timeline — and a
    // thread reply appearing in the main timeline is §8's first invariant.
    void aLocationIsNeverSentWhileTheComposerIsInAThread()
    {
        SendingClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!r:example.org"));
        composer.beginThreadReply(QStringLiteral("$root:example.org"),
                                  QStringLiteral("the root"));

        composer.sendLocation(51.5, -0.12, QStringLiteral("The pub"));
        QVERIFY2(client.calls == 0,
                 "a location sent from a thread would land in the room");

        composer.cancelReplyOrEdit();
        composer.sendLocation(51.5, -0.12, QStringLiteral("The pub"));
        QCOMPARE(client.calls, 1);
    }
};

QTEST_MAIN(LocationMessageTest)
#include "LocationMessageTest.moc"
