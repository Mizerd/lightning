// Fields the Rust lane computes must survive the bridge. SfuCallController's
// tests drive a fake client that emits the signal directly, so only the real
// dispatcher (`handleRustEventForTest`) with the real payload shows whether
// `RustSdkMatrixClient::handleRustEvent` carries a field.
//
// `delayed_category` in `rtc_membership_published` says whether a refused
// delayed event means "no MSC4140 endpoint" (permanent) or "this write was
// refused" (transient).

#include "app/SettingsManager.h"

#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix/RustSdkMatrixClient.h"
#endif

class RtcBridgePayloadTest : public QObject
{
    Q_OBJECT

#ifdef ENABLE_RUST_SDK_BACKEND
private:
    /// The payload rust/src/rtc.rs enqueues, field for field. Spelled out
    /// here rather than built by a helper so that a rename on the Rust side
    /// shows up as a failing test and not as a silently absent field.
    static QJsonObject publishedEvent(const QString &delayId,
                                      const QString &delayedCategory)
    {
        QJsonObject out;
        out.insert(QStringLiteral("type"),
                   QStringLiteral("rtc_membership_published"));
        out.insert(QStringLiteral("op_id"), 7);
        out.insert(QStringLiteral("room_id"), QStringLiteral("!r:example.org"));
        out.insert(QStringLiteral("ok"), true);
        out.insert(QStringLiteral("category"), QString());
        out.insert(QStringLiteral("event_id"), QStringLiteral("$evt"));
        out.insert(QStringLiteral("delay_id"), delayId);
        out.insert(QStringLiteral("delayed_category"), delayedCategory);
        return out;
    }
#endif

private slots:
    /// rooms.rs reports `own_can_publish_rtc_membership` as null when our own
    /// member is not in the store yet. That must reach the call gate as
    /// UNKNOWN (no key), not as false, which would read as refused.
    void anUnknownCallPermissionIsNotReportedAsRefused()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        QSignalSpy spy(&client, &MatrixClient::roomMembersReceived);
        QJsonObject event;
        event.insert(QStringLiteral("type"), QStringLiteral("room_members"));
        event.insert(QStringLiteral("op_id"), 9);
        event.insert(QStringLiteral("room_id"),
                     QStringLiteral("!r:example.org"));
        event.insert(QStringLiteral("ok"), true);
        event.insert(QStringLiteral("partial"), true);
        event.insert(QStringLiteral("own_can_publish_rtc_membership"),
                     QJsonValue(QJsonValue::Null));
        client.handleRustEventForTest(event);
        QCOMPARE(spy.count(), 1);
        QVariantMap snapshot = spy.takeFirst().at(2).toMap();
        QVERIFY2(!snapshot.contains(QStringLiteral("canPublishCallMembership")),
                 "an unknown permission crossed the bridge as a refusal");

        // A real answer still crosses, both ways.
        event.insert(QStringLiteral("own_can_publish_rtc_membership"), false);
        client.handleRustEventForTest(event);
        QCOMPARE(spy.count(), 1);
        snapshot = spy.takeFirst().at(2).toMap();
        QCOMPARE(snapshot.value(QStringLiteral("canPublishCallMembership")),
                 QVariant(false));
        event.insert(QStringLiteral("own_can_publish_rtc_membership"), true);
        client.handleRustEventForTest(event);
        snapshot = spy.takeFirst().at(2).toMap();
        QCOMPARE(snapshot.value(QStringLiteral("canPublishCallMembership")),
                 QVariant(true));
#endif
    }

    void theDelayedRefusalReasonSurvivesTheBridge()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        QSignalSpy spy(&client, &MatrixClient::rtcMembershipPublished);
        // A homeserver with no MSC4140 endpoint at all: no delay id, and the
        // category that says the absence is PERMANENT rather than a refusal
        // this once. rtc.rs latches on exactly this vocabulary.
        client.handleRustEventForTest(
            publishedEvent(QString(), QStringLiteral("unrecognized")));
        QCOMPARE(spy.count(), 1);
        const QList<QVariant> args = spy.takeFirst();
        QCOMPARE(args.at(1).toBool(), true);
        QCOMPARE(args.at(3).toString(), QStringLiteral("$evt"));
        QVERIFY(args.at(4).toString().isEmpty());
        // The field under test. Empty here is the defect, not a default.
        QCOMPARE(args.at(5).toString(), QStringLiteral("unrecognized"));
#endif
    }

    /// `auto_key_recovery`'s `inconclusive` count must survive the bridge: it
    /// is what distinguishes one downloaded session out of 32 from a server
    /// that refused thirty-one times.
    void theInconclusiveCountSurvivesTheBridge()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        QSignalSpy spy(&client, &RustSdkMatrixClient::cryptoBootstrapEvent);

        QJsonObject out;
        out.insert(QStringLiteral("type"),
                   QStringLiteral("crypto_bootstrap"));
        out.insert(QStringLiteral("kind"),
                   QStringLiteral("auto_key_recovery"));
        out.insert(QStringLiteral("state"), QStringLiteral("ok"));
        out.insert(QStringLiteral("count"), 1);
        out.insert(QStringLiteral("inconclusive"), 31);
        client.handleRustEventForTest(out);

        QCOMPARE(spy.count(), 1);
        const QList<QVariant> args = spy.takeFirst();
        QCOMPARE(args.at(0).toString(), QStringLiteral("auto_key_recovery"));
        QCOMPARE(args.at(1).toString(), QStringLiteral("ok"));
        QCOMPARE(args.at(2).toULongLong(), 1ULL);
        // The field under test. 0 here is the defect, not a default.
        QCOMPARE(args.at(3).toULongLong(), 31ULL);
#endif
    }

    /// A kind with no such notion must report zero rather than stale data.
    void anEventWithoutTheCountReportsZero()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        QSignalSpy spy(&client, &RustSdkMatrixClient::cryptoBootstrapEvent);

        QJsonObject out;
        out.insert(QStringLiteral("type"),
                   QStringLiteral("crypto_bootstrap"));
        out.insert(QStringLiteral("kind"), QStringLiteral("backup_state"));
        out.insert(QStringLiteral("state"), QStringLiteral("enabled"));
        client.handleRustEventForTest(out);

        QCOMPARE(spy.count(), 1);
        const QList<QVariant> args = spy.takeFirst();
        QCOMPARE(args.at(2).toULongLong(), 0ULL);
        QCOMPARE(args.at(3).toULongLong(), 0ULL);
#endif
    }

    void anArmedDelayedRetractionCarriesNoReason()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        // Success must not invent a reason: an empty `delayed_category` beside
        // a non-empty delay id means nothing went wrong.
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        QSignalSpy spy(&client, &MatrixClient::rtcMembershipPublished);
        client.handleRustEventForTest(
            publishedEvent(QStringLiteral("delay-1"), QString()));
        QCOMPARE(spy.count(), 1);
        const QList<QVariant> args = spy.takeFirst();
        QCOMPARE(args.at(4).toString(), QStringLiteral("delay-1"));
        QVERIFY(args.at(5).toString().isEmpty());
#endif
    }
};

QTEST_MAIN(RtcBridgePayloadTest)
#include "RtcBridgePayloadTest.moc"
