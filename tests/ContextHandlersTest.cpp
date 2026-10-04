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
};

QTEST_MAIN(ContextHandlersTest)
#include "ContextHandlersTest.moc"
