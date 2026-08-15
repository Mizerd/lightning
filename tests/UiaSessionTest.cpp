// v0.7.x reusable User-Interactive Authentication + device sign-out —
// UiaController policy against the scriptable MockMatrixClient UIA surface.
// Pins:
//   * a no-challenge sign-out completes end-to-end (busy flips, the exact
//     device ids reach the backend, signOutFinished reports ok);
//   * the CURRENT device is refused as a guard — signing out this session
//     is the normal logout flow, never a device deletion;
//   * a real UIA challenge opens the password prompt, a wrong password
//     reopens it with retry offered, cancel closes it terminally and a
//     stale later answer is refused;
//   * terminal failure categories surface as their honest messages;
//   * the OAuth management-URL path hands over exactly the scripted URL and
//     an undeterminable URL is a reported failure, not silence;
//   * sign-out / account switch mid-challenge clears the challenge so a
//     later account can never answer it.
//
// CREDENTIAL RULES: the password passes through submitPassword() transiently
// and is never retained — see the structural assertion in
// passwordIsNeverRetainedInControllerState().
//
// HONEST SCOPE: policy and wiring only. Real /delete_devices UIA round trips
// against a homeserver and MAS/OAuth account consoles are NOT exercised here
// and are NOT TESTED.

#include "app/UiaController.h"
#include "matrix/MockMatrixClient.h"

#include <QMetaProperty>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

constexpr int kSignalTimeoutMs = 2000;

const QString kCurrentDevice = QStringLiteral("DEV1");
const QString kOtherDevice = QStringLiteral("DEV2");
// Deliberately does not contain the substring "password": the stages list
// legitimately carries "m.login.password", and the non-retention scan must
// not trip on it.
const QString kGoodSecret = QStringLiteral("correct-uia-answer");
const QString kWrongSecret = QStringLiteral("wrong-uia-answer");

} // namespace

class UiaSessionTest : public QObject
{
    Q_OBJECT

private:
    // Drive the controller into an active password challenge.
    static bool raiseChallenge(UiaController &ctl, MockMatrixClient &client)
    {
        client.mockUiaRequired = true;
        client.mockUiaPassword = kGoodSecret;
        ctl.signOutDevices({ kOtherDevice }, kCurrentDevice);
        return QTest::qWaitFor([&ctl] { return ctl.challengeActive(); },
                               kSignalTimeoutMs);
    }

private Q_SLOTS:
    void noChallengeSignOutCompletes()
    {
        MockMatrixClient client;
        UiaController ctl;
        ctl.setClient(&client);
        QVERIFY(ctl.supported());
        QVERIFY(!ctl.busy());

        QSignalSpy finished(&ctl, &UiaController::signOutFinished);
        ctl.signOutDevices({ kOtherDevice }, kCurrentDevice);
        // The operation is in flight until the backend answers.
        QVERIFY(ctl.busy());
        QCOMPARE(client.lastDeletedDevices, QStringList{ kOtherDevice });

        QVERIFY(finished.wait(kSignalTimeoutMs));
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished.at(0).at(0).toBool());              // ok
        QVERIFY(!finished.at(0).at(1).toString().isEmpty()); // message
        QVERIFY(!ctl.busy());
        QVERIFY(!ctl.challengeActive());
    }

    void currentDeviceIsRefusedAsAGuard()
    {
        MockMatrixClient client;
        UiaController ctl;
        ctl.setClient(&client);

        QSignalSpy finished(&ctl, &UiaController::signOutFinished);
        // The current device (and duplicates/empties of it) filter to an
        // empty list, and an empty list dispatches NOTHING — signing out
        // this session belongs to the logout flow with its store cleanup.
        ctl.signOutDevices({ kCurrentDevice, kCurrentDevice, QString() },
                           kCurrentDevice);
        QVERIFY(!ctl.busy());
        QTest::qWait(80);
        QCOMPARE(client.lastDeletedDevices, QStringList());
        QCOMPARE(finished.size(), 0);
    }

    void uiaChallengeOpensThePasswordStage()
    {
        MockMatrixClient client;
        UiaController ctl;
        ctl.setClient(&client);

        QVERIFY(raiseChallenge(ctl, client));
        QVERIFY(ctl.challengeActive());
        QVERIFY(ctl.passwordStage());
        QVERIFY(!ctl.wrongPassword());
        QVERIFY(ctl.stages().contains(QStringLiteral("m.login.password")));

        QSignalSpy finished(&ctl, &UiaController::signOutFinished);
        ctl.submitPassword(kGoodSecret);
        // The challenge closes optimistically; a fresh uiaRequired would
        // reopen it on rejection.
        QVERIFY(!ctl.challengeActive());
        QVERIFY(finished.wait(kSignalTimeoutMs));
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished.at(0).at(0).toBool()); // ok
        QVERIFY(!ctl.busy());
    }

    void wrongPasswordReopensTheChallengeAndRetrySucceeds()
    {
        MockMatrixClient client;
        UiaController ctl;
        ctl.setClient(&client);
        QVERIFY(raiseChallenge(ctl, client));

        QSignalSpy finished(&ctl, &UiaController::signOutFinished);
        ctl.submitPassword(kWrongSecret);
        // Rejected: the challenge reopens with retry offered — the
        // operation is NOT terminally failed.
        QTRY_VERIFY_WITH_TIMEOUT(ctl.challengeActive(), kSignalTimeoutMs);
        QVERIFY(ctl.wrongPassword());
        QVERIFY(ctl.passwordStage());
        QCOMPARE(finished.size(), 0);

        ctl.submitPassword(kGoodSecret);
        QVERIFY(finished.wait(kSignalTimeoutMs));
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished.at(0).at(0).toBool()); // ok
        QVERIFY(!ctl.wrongPassword());
    }

    void cancelClosesTheChallengeTerminally()
    {
        MockMatrixClient client;
        UiaController ctl;
        ctl.setClient(&client);
        QVERIFY(raiseChallenge(ctl, client));

        QSignalSpy finished(&ctl, &UiaController::signOutFinished);
        ctl.cancel();
        QVERIFY(!ctl.challengeActive());
        QVERIFY(!ctl.busy());
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.at(0).at(0).toBool()); // ok == false
        QVERIFY(finished.at(0).at(1).toString().contains(
            QStringLiteral("cancelled")));
        // The backend's pending challenge is gone too, so a late answer
        // has nothing to attach to.
        QCOMPARE(client.lastDeleteOp, quint64(0));

        // A later submit is refused outright: no new signal, no new state.
        ctl.submitPassword(kGoodSecret);
        QTest::qWait(80);
        QCOMPARE(finished.size(), 1);
        QVERIFY(!ctl.challengeActive());
        QVERIFY(!ctl.busy());
    }

    void terminalFailureCategorySurfacesHonestly()
    {
        MockMatrixClient client;
        UiaController ctl;
        ctl.setClient(&client);
        client.mockDeleteFailCategory = QStringLiteral("not_found");

        QSignalSpy finished(&ctl, &UiaController::signOutFinished);
        ctl.signOutDevices({ kOtherDevice }, kCurrentDevice);
        QVERIFY(finished.wait(kSignalTimeoutMs));
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.at(0).at(0).toBool()); // ok == false
        // The device is already gone — the message says so, rather than a
        // generic error.
        QCOMPARE(finished.at(0).at(1).toString(),
                 QStringLiteral("That session no longer exists."));
        QVERIFY(!ctl.busy());
    }

    void managementUrlHandsOverTheScriptedUrl()
    {
        MockMatrixClient client;
        UiaController ctl;
        ctl.setClient(&client);
        client.mockManagementUrl =
            QStringLiteral("https://account.example.org/sessions");

        QSignalSpy ready(&ctl, &UiaController::managementUrlReady);
        ctl.requestManagementUrl(QString());
        QVERIFY(ctl.busy());
        QVERIFY(ready.wait(kSignalTimeoutMs));
        QCOMPARE(ready.size(), 1);
        QCOMPARE(ready.at(0).at(0).toString(),
                 QStringLiteral("https://account.example.org/sessions"));
        QVERIFY(!ctl.busy());

        // An undeterminable URL is a reported failure, never silence.
        client.mockManagementUrl.clear();
        QSignalSpy finished(&ctl, &UiaController::signOutFinished);
        ctl.requestManagementUrl(QString());
        QVERIFY(finished.wait(kSignalTimeoutMs));
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.at(0).at(0).toBool()); // ok == false
        QVERIFY(!finished.at(0).at(1).toString().isEmpty());
        QCOMPARE(ready.size(), 1); // no phantom URL alongside the failure
    }

    void loggedOutMidChallengeClearsIt()
    {
        MockMatrixClient client;
        UiaController ctl;
        ctl.setClient(&client);
        QVERIFY(raiseChallenge(ctl, client));

        // A signed-out (or switched) session must never keep a challenge
        // that a later account could answer.
        client.logout();
        QVERIFY(!ctl.challengeActive());
        QVERIFY(!ctl.passwordStage());
        QVERIFY(!ctl.wrongPassword());
        QVERIFY(ctl.stages().isEmpty());
        QVERIFY(!ctl.busy());
    }

    void passwordIsNeverRetainedInControllerState()
    {
        MockMatrixClient client;
        UiaController ctl;
        ctl.setClient(&client);
        QVERIFY(raiseChallenge(ctl, client));

        ctl.submitPassword(kGoodSecret);
        QTRY_VERIFY_WITH_TIMEOUT(!ctl.busy(), kSignalTimeoutMs);

        // Structural: the controller exposes NO property that could carry
        // the submitted password — its state is booleans and stage names
        // only. Credential non-retention below this boundary (the C++
        // transit buffer and the Rust scrub) is enforced by design in those
        // layers, not observable from here; this pins the QML-facing
        // surface. Scan every metaobject property's readable value.
        const QMetaObject *mo = ctl.metaObject();
        for (int i = 0; i < mo->propertyCount(); ++i) {
            const QMetaProperty prop = mo->property(i);
            const QVariant value = prop.read(&ctl);
            const QString rendered = value.toString()
                + value.toStringList().join(QLatin1Char(' '));
            QVERIFY2(!rendered.contains(kGoodSecret),
                     qPrintable(QStringLiteral("property '%1' leaks the "
                                               "submitted password")
                                    .arg(QLatin1String(prop.name()))));
        }
    }
};

QTEST_MAIN(UiaSessionTest)
#include "UiaSessionTest.moc"
