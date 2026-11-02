// Shared places: `m.location` (MSC3488) and live beacons (MSC3672). A geo URI
// is sender-controlled, so a point that does not parse or is not on Earth
// leaves the coordinates absent, never 0,0 (a real spot in the Atlantic).

#include "matrix/MockMatrixClient.h"
#include "matrix/RustTimelineIngest.h"

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

const QString kRoom = QStringLiteral("!r:example.org");

}

class LocationMessageTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aLocationIsItsOwnRowKind()
    {
        const TimelineEvent e = matrix::rust_timeline::eventFromItemJson(
            locationItem(QStringLiteral("Big Ben"), 51.5008, -0.1247), kRoom);
        QCOMPARE(e.type, TimelineEvent::Location);
        QVERIFY(e.locationHasPoint);
        QVERIFY(qAbs(e.locationLat - 51.5008) < 1e-9);
        QVERIFY(qAbs(e.locationLon - (-0.1247)) < 1e-9);
        QCOMPARE(e.body, QStringLiteral("Big Ben"));
    }

    void anUnreadablePointLeavesTheCoordinatesAbsentRatherThanAtZero()
    {
        const TimelineEvent e = matrix::rust_timeline::eventFromItemJson(
            locationItem(QStringLiteral("Somewhere"), 0, 0, false), kRoom);
        QCOMPARE(e.type, TimelineEvent::Location);
        QVERIFY2(!e.locationHasPoint,
                 "an absent point must not read as a valid one");
    }

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

};

QTEST_MAIN(LocationMessageTest)
#include "LocationMessageTest.moc"
