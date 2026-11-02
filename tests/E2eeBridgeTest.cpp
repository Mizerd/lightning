
#include "matrix/RustSdkMatrixClient.h"
#include "app/SettingsManager.h"
#include "crypto/BackupController.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

class E2eeBridgeTest : public QObject
{
    Q_OBJECT

    static QJsonObject device(const QString &id, bool current, bool verified,
                              bool crossSigned, bool trusted)
    {
        return QJsonObject{
            { QStringLiteral("device_id"), id },
            { QStringLiteral("display_name"), QString() },
            { QStringLiteral("last_seen_ts"), 0 },
            { QStringLiteral("last_seen_ip"), QString() },
            { QStringLiteral("is_current"), current },
            { QStringLiteral("has_crypto_identity"), true },
            { QStringLiteral("verified"), verified },
            { QStringLiteral("cross_signed"), crossSigned },
            { QStringLiteral("trusted"), trusted },
        };
    }

private Q_SLOTS:
    void theSessionsTrustFlagSurvivesTheBridge()
    {
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        QSignalSpy lists(&client, &RustSdkMatrixClient::deviceListUpdated);
        client.handleRustEventForTest(QJsonObject{
            { QStringLiteral("type"), QStringLiteral("device_list") },
            { QStringLiteral("ok"), true },
            { QStringLiteral("devices"), QJsonArray{
                device(QStringLiteral("CURRENT"), true, true, false, false),
                device(QStringLiteral("SASONLY"), false, true, false, true),
                device(QStringLiteral("SIGNEDONLY"), false, false, true, false),
            } },
        });
        QCOMPARE(lists.count(), 1);
        const QVariantList devices = lists.first().at(1).toList();
        QCOMPARE(devices.size(), 3);
        QHash<QString, bool> trusted;
        for (const QVariant &v : devices) {
            const QVariantMap d = v.toMap();
            QVERIFY2(d.contains(QStringLiteral("trusted")),
                     "the Sessions page reads `trusted`; the bridge dropped it");
            trusted.insert(d.value(QStringLiteral("deviceId")).toString(),
                           d.value(QStringLiteral("trusted")).toBool());
        }
        QCOMPARE(trusted.value(QStringLiteral("CURRENT")), false);
        QCOMPARE(trusted.value(QStringLiteral("SASONLY")), true);
        QCOMPARE(trusted.value(QStringLiteral("SIGNEDONLY")), false);
    }

    void aWrongRecoveryKeyIsReportedInOurWordsNotTheSdks()
    {
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        QSignalSpy results(&client, &RustSdkMatrixClient::keyBackupResult);
        client.handleRustEventForTest(QJsonObject{
            { QStringLiteral("type"), QStringLiteral("key_backup_status") },
            { QStringLiteral("state"), QStringLiteral("failed") },
            { QStringLiteral("category"), QStringLiteral("recovery_key_wrong") },
            { QStringLiteral("message"),
              QStringLiteral("Matrix Rust SDK key backup recover failed: The MAC "
                             "check for the secret storage key failed") },
        });
        QCOMPARE(results.count(), 1);
        QCOMPARE(results.first().at(0).toString(), QStringLiteral("failed"));
        const QString shown = results.first().at(1).toString();
        QCOMPARE(shown, BackupController::recoveryFailureText(
                            QStringLiteral("recovery_key_wrong")));
        QVERIFY2(!shown.contains(QStringLiteral("MAC"))
                     && !shown.contains(QStringLiteral("SDK")),
                 qPrintable(shown));
    }

    void aFailureAfterVerificationSaysTheSessionIsVerified()
    {
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        QSignalSpy results(&client, &RustSdkMatrixClient::keyBackupResult);
        const auto failed = [&](const char *category, bool verified, bool became) {
            client.handleRustEventForTest(QJsonObject{
                { QStringLiteral("type"), QStringLiteral("key_backup_status") },
                { QStringLiteral("state"), QStringLiteral("failed") },
                { QStringLiteral("category"), QLatin1String(category) },
                { QStringLiteral("session_verified"), verified },
                { QStringLiteral("became_verified"), became },
            });
            return results.last().at(1).toString();
        };
        const QString became = failed("network", true, true);
        QVERIFY2(became.contains(QStringLiteral("now verified")), qPrintable(became));
        QVERIFY2(!became.contains(QStringLiteral("Verify this session")),
                 qPrintable(became));
        const QString already = failed("network", true, false);
        QVERIFY2(already.contains(QStringLiteral("is verified"))
                     && !already.contains(QStringLiteral("now")),
                 qPrintable(already));
        QVERIFY(!failed("network", false, false).contains(QStringLiteral("verified")));
        QCOMPARE(failed("recovery_key_wrong", true, false),
                 BackupController::recoveryFailureText(
                     QStringLiteral("recovery_key_wrong")));
        QCOMPARE(failed("recovery_key_malformed", true, false),
                 BackupController::recoveryFailureText(
                     QStringLiteral("recovery_key_malformed")));
    }

    void aDeferredReopenRunsOnlyForATimelineStillOpenAndStillOwed()
    {
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        client.m_recoveryReopens.setWindowMsForTest(40);
        const QString room = QStringLiteral("!r:example.org");
        int reopened = 0;
        int asked = 0;
        const auto admit = [&] {
            return client.admitRecoveryReopen(
                room, "test",
                [&client, &asked, room] {
                    ++asked;
                    return client.roomTimelineStillOpen(room);
                },
                [&reopened] { ++reopened; });
        };
        const auto exhaust = [&] {
            for (int i = 0; i < matrix::RecoveryReopenBudget::kMaxReopens; ++i)
                QVERIFY(admit());
            QVERIFY(!admit());
        };

        client.m_timelineTracker.request(room);
        exhaust();
        QTRY_COMPARE_WITH_TIMEOUT(reopened, 1, 2000);
        QTest::qWait(120);
        QCOMPARE(reopened, 1);
        QCOMPARE(asked, 1);

        client.m_recoveryReopens.clear();
        exhaust();
        client.m_timelineTracker.request(QStringLiteral("!other:example.org"));
        QTRY_COMPARE_WITH_TIMEOUT(asked, 2, 2000);
        QTest::qWait(120);
        QCOMPARE(reopened, 1);

        client.m_timelineTracker.request(room);
        client.m_recoveryReopens.clear();
        exhaust();
        client.m_recoveryReopens.clear();
        QTest::qWait(200);
        QCOMPARE(reopened, 1);
        QCOMPARE(asked, 2);
    }
};

QTEST_MAIN(E2eeBridgeTest)
#include "E2eeBridgeTest.moc"
