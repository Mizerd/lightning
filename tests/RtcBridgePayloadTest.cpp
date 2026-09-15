// WHAT THE RUST LANE COMPUTES AND THE BRIDGE FORGETS TO CARRY.
//
// `rtc_membership_published` is a JSON payload with six fields. The C++
// handler read four of them and then FIVE of them, and `delayed_category` —
// which rust/src/rtc.rs has computed and enqueued since the delayed-events
// work — was dropped on the floor at `RustSdkMatrixClient.cpp`. Nothing could
// see it: SfuCallController's own tests drive a FAKE client that emits the
// signal directly, so they prove what the controller does with a field and
// say nothing about whether the bridge ever supplies one.
//
// That is the defect shape CLAUDE.md §16 names twice over — a test that
// composes something RESEMBLING what production composes proves nothing — and
// the only cure is to drive the real dispatcher with the real payload, which
// is what `handleRustEventForTest` exists for.
//
// The cost of the drop was diagnostic, not functional: every `delayed= false`
// in a call log was mute about WHETHER the homeserver has no MSC4140 endpoint
// (permanent, nothing to retry) or refused this one write (transient, the
// next publish tries again). Those have opposite remedies and issue #10's
// reporter had no way to tell them apart.
//
// FAIL-ON-OLD: drop the `delayed_category` line from the
// `rtc_membership_published` branch of `RustSdkMatrixClient::handleRustEvent`
// and `theDelayedRefusalReasonSurvivesTheBridge` reads an empty string.

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

    void anArmedDelayedRetractionCarriesNoReason()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        // The success case must not invent a reason: rtc.rs leaves
        // `delayed_category` empty when the arm succeeded, and a reader of
        // the log has to be able to trust that an empty reason beside a
        // non-empty delay id means nothing went wrong.
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
