// The Rust-lane handlers for the read-only context view, driven with real
// payloads through the dispatcher and no FFI handle.
//
// Rust enqueues context_closed for the view it replaces AHEAD of the
// replacement's own context_reset, and for a reopen of the same event (the
// invalid-diff recovery, or close and reopen within one poll tick) both name
// the same timeline id. The closed handler must not tear down the request the
// reset is about to answer.

#include "matrix/RustSdkMatrixClient.h"
#include "app/SettingsManager.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

class ContextHandlersTest : public QObject
{
    Q_OBJECT

    static QJsonObject reset(const QString &room, const QString &event, int gen)
    {
        return QJsonObject{
            { QStringLiteral("type"), QStringLiteral("context_reset") },
            { QStringLiteral("room_id"), room },
            { QStringLiteral("event_id"), event },
            { QStringLiteral("context_generation"), gen },
            { QStringLiteral("lifecycle"), 1 },
            { QStringLiteral("items"), QJsonArray{} },
        };
    }
    static QJsonObject closed(const QString &room, const QString &event)
    {
        return QJsonObject{
            { QStringLiteral("type"), QStringLiteral("context_closed") },
            { QStringLiteral("room_id"), room },
            { QStringLiteral("event_id"), event },
        };
    }

private Q_SLOTS:
    void aStaleClosedForTheSameEventDoesNotRejectTheReopensReset()
    {
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        const QString room = QStringLiteral("!r:example.org");
        const QString event = QStringLiteral("$e:example.org");
        const QString id = MatrixClient::contextTimelineId(room, event);

        // First view adopted.
        client.m_contextTracker.request(id);
        QSignalSpy resets(&client, &MatrixClient::timelineReset);
        client.handleRustEventForTest(reset(room, event, 1));
        QCOMPARE(resets.count(), 1);

        // Reopen of the same event: C++ requests anew, then the poll drains
        // [context_closed(old), context_reset(new generation)].
        client.m_contextTracker.request(id);
        client.handleRustEventForTest(closed(room, event));
        client.handleRustEventForTest(reset(room, event, 2));
        QCOMPARE(resets.count(), 2);
        QVERIFY(client.m_contextTracker.accepts(id, 2));
    }

    void aClosedForAnUntrackedViewIsHarmless()
    {
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        client.handleRustEventForTest(closed(QStringLiteral("!r:x"),
                                             QStringLiteral("$e:x")));
        QVERIFY(!client.m_contextTracker.hasActiveTimeline());
    }

    void aResetForAViewNeverRequestedIsRejected()
    {
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        QSignalSpy resets(&client, &MatrixClient::timelineReset);
        client.handleRustEventForTest(reset(QStringLiteral("!r:x"),
                                            QStringLiteral("$e:x"), 1));
        QCOMPARE(resets.count(), 0);
    }

    // A redaction refused because the room's timeline is not live yet still
    // gets its one result: a bulk deletion waits for it before sending the
    // next, and a silent refusal stalled the run (review S1). The real room is
    // reported, never the composite thread id.
    void aRedactionRefusedBeforeTheTimelineIsLiveStillReportsAResult()
    {
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        QSignalSpy finished(&client, &MatrixClient::redactionFinished);
        const QString room = QStringLiteral("!r:example.org");
        client.redactEvent(room, QStringLiteral("$e:example.org"), QString());
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.at(0).at(0).toString(), room);
        QCOMPARE(finished.at(0).at(1).toString(),
                 QStringLiteral("$e:example.org"));
        QCOMPARE(finished.at(0).at(2).toBool(), false);

        const QString thread = MatrixClient::threadTimelineId(
            room, QStringLiteral("$root:example.org"));
        client.redactEvent(thread, QStringLiteral("$t:example.org"), QString());
        QCOMPARE(finished.count(), 2);
        QCOMPARE(finished.at(1).at(0).toString(), room);
    }

    // GitHub #48: the classic-sync repair's "a view opened during the clear"
    // reaches the client as a signal (AppController reopens the views on it).
    // Lives here because this suite already drives RustSdkMatrixClient's
    // event handler directly.
    void aRepairUnderAnOpenViewAsksForAReload()
    {
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        QSignalSpy repaired(&client, &MatrixClient::eventCacheRepaired);
        client.handleRustEventForTest(QJsonObject{
            { QStringLiteral("type"), QStringLiteral("event_cache_repaired") },
        });
        QCOMPARE(repaired.count(), 1);
    }

    // ...and only from the running session: a signing-out handle's events
    // still pass the poll gate (shutdown completions), but this one reopens
    // views and must not reach the next session (§9).
    void aRepairReloadFromASigningOutSessionIsIgnored()
    {
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        const quint64 old = client.m_lifecycle.beginSession();
        client.m_lifecycle.beginSignOut(old);
        QVERIFY(client.m_lifecycle.acceptsShutdownCompletion(old));
        QSignalSpy repaired(&client, &MatrixClient::eventCacheRepaired);
        const QJsonObject event{
            { QStringLiteral("type"), QStringLiteral("event_cache_repaired") },
            { QStringLiteral("lifecycle"), 1 },
        };
        client.handleRustEvent(event, old);
        QCOMPARE(repaired.count(), 0);
        // The running session's own request still works.
        client.handleRustEvent(event, client.m_lifecycle.beginSession());
        QCOMPARE(repaired.count(), 1);
    }
};

QTEST_MAIN(ContextHandlersTest)
#include "ContextHandlersTest.moc"
