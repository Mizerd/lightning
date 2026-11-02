
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

        client.m_contextTracker.request(id);
        QSignalSpy resets(&client, &MatrixClient::timelineReset);
        client.handleRustEventForTest(reset(room, event, 1));
        QCOMPARE(resets.count(), 1);

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
        QCOMPARE(finished.at(0).at(3).toString(), QStringLiteral("not_ready"));

        const QString thread = MatrixClient::threadTimelineId(
            room, QStringLiteral("$root:example.org"));
        client.redactEvent(thread, QStringLiteral("$t:example.org"), QString());
        QCOMPARE(finished.count(), 2);
        QCOMPARE(finished.at(1).at(0).toString(), room);
    }

    void aRedactionResultIsACategoryAndItsTextIsNeverShown()
    {
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        QSignalSpy finished(&client, &MatrixClient::redactionFinished);
        QSignalSpy errors(&client, &MatrixClient::errorOccurred);
        client.handleRustEventForTest(QJsonObject{
            { QStringLiteral("type"),
              QStringLiteral("timeline_redaction_finished") },
            { QStringLiteral("room_id"), QStringLiteral("!r:example.org") },
            { QStringLiteral("event_id"), QStringLiteral("$e:example.org") },
            { QStringLiteral("ok"), false },
            { QStringLiteral("category"), QStringLiteral("rate_limited") },
            { QStringLiteral("error"),
              QStringLiteral("secret https://matrix.example.org/x") },
        });
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.at(0).at(2).toBool(), false);
        QCOMPARE(finished.at(0).at(3).toString(), QStringLiteral("rate_limited"));
        QCOMPARE(errors.count(), 1);
        const QString shown = errors.at(0).at(0).toString();
        QCOMPARE(shown, MatrixClient::redactionFailureText(
                            QStringLiteral("rate_limited")));
        QVERIFY(!shown.contains(QStringLiteral("secret")));

        client.handleRustEventForTest(QJsonObject{
            { QStringLiteral("type"),
              QStringLiteral("timeline_redaction_finished") },
            { QStringLiteral("room_id"), QStringLiteral("!r:example.org") },
            { QStringLiteral("event_id"), QStringLiteral("$f:example.org") },
            { QStringLiteral("ok"), true },
            { QStringLiteral("category"), QString() },
        });
        QCOMPARE(finished.count(), 2);
        QVERIFY(finished.at(1).at(3).toString().isEmpty());
        QCOMPARE(errors.count(), 1);
    }

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
        client.handleRustEvent(event, client.m_lifecycle.beginSession());
        QCOMPARE(repaired.count(), 1);
    }
};

QTEST_MAIN(ContextHandlersTest)
#include "ContextHandlersTest.moc"
