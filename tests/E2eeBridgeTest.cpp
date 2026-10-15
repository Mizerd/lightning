// The Rust lane's trust and recovery payloads, driven through the real
// dispatcher with the payloads Rust sends and no FFI handle.
//
// A fake client emits signals itself and cannot see a field the bridge drops
// or a raw string it forwards; these cases can.
//
// * Sessions: every trust label on the page reads one field, `trusted`, which
//   Rust derives as `isCurrent ? crossSigned : verified`
//   (`recovery::session_row_trusted`). It must survive the bridge.
// * Recovery: a failed restore carries a category, and the user sees our
//   translated sentence for it, never matrix-sdk's English text (which for a
//   key that does not decode quotes part of what was typed).

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
        // The two rows where crossSigned and the SDK's trust disagree, as Rust
        // computes them: SAS-verified without cross-signing (trusted), and
        // signed by a key this session never verified (not trusted).
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
        // The old payload's `message` is included as well: a bridge that
        // still forwarded it would show it.
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

    // Review M2: recover() signs this device before it touches the key
    // backup, so a failure there arrives with the session verified. The text
    // must say so, not send the user off to verify it.
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
        // This attempt verified the session, then the backup step failed.
        const QString became = failed("network", true, true);
        QVERIFY2(became.contains(QStringLiteral("now verified")), qPrintable(became));
        QVERIFY2(!became.contains(QStringLiteral("Verify this session")),
                 qPrintable(became));
        // Verified earlier (by SAS): verified, but not "now" (review N1).
        const QString already = failed("network", true, false);
        QVERIFY2(already.contains(QStringLiteral("is verified"))
                     && !already.contains(QStringLiteral("now")),
                 qPrintable(already));
        // Not verified: no claim at all.
        QVERIFY(!failed("network", false, false).contains(QStringLiteral("verified")));
        // A key-entry failure keeps its own sentence on an already verified
        // session (review N1): the mistyped key is what the user must fix.
        QCOMPARE(failed("recovery_key_wrong", true, false),
                 BackupController::recoveryFailureText(
                     QStringLiteral("recovery_key_wrong")));
        QCOMPARE(failed("recovery_key_malformed", true, false),
                 BackupController::recoveryFailureText(
                     QStringLiteral("recovery_key_malformed")));
    }

    // Review L7: the deferred reopen's two guards. Driven through the real
    // admitRecoveryReopen with a short window; the reopen is a counter.
    void aDeferredReopenRunsOnlyForATimelineStillOpenAndStillOwed()
    {
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        client.m_recoveryReopens.setWindowMsForTest(40);
        const QString room = QStringLiteral("!r:example.org");
        int reopened = 0;
        // How often the deferred reopen consulted the guard: proof that it
        // fired, without depending on any log line (review N4).
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
            QVERIFY(!admit()); // capped: one deferred reopen scheduled
        };

        // Still open when the window frees: it runs, once.
        client.m_timelineTracker.request(room);
        exhaust();
        QTRY_COMPARE_WITH_TIMEOUT(reopened, 1, 2000);
        QTest::qWait(120);
        QCOMPARE(reopened, 1);
        QCOMPARE(asked, 1);

        // The user moved to another room meanwhile: the deferred reopen fires
        // (it is still owed, so it is counted) and skips the reopen. Asserted
        // on state, not on the log line, so a log configuration cannot change
        // the verdict (review N4).
        client.m_recoveryReopens.clear();
        exhaust();
        client.m_timelineTracker.request(QStringLiteral("!other:example.org"));
        QTRY_COMPARE_WITH_TIMEOUT(asked, 2, 2000); // it fired and asked...
        QTest::qWait(120);
        QCOMPARE(reopened, 1);                      // ...and did not reopen

        // The session ended before the timer: nothing owed, nothing runs.
        client.m_timelineTracker.request(room);
        client.m_recoveryReopens.clear();
        exhaust();
        client.m_recoveryReopens.clear();
        QTest::qWait(200);
        QCOMPARE(reopened, 1);
        QCOMPARE(asked, 2); // never even asked: nothing was owed
    }
};

QTEST_MAIN(E2eeBridgeTest)
#include "E2eeBridgeTest.moc"
