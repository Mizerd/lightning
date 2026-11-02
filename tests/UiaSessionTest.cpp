
#include "app/UiaController.h"
#include "matrix/MockMatrixClient.h"

#include <QMetaProperty>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

constexpr int kSignalTimeoutMs = 2000;

const QString kCurrentDevice = QStringLiteral("DEV1");
const QString kOtherDevice = QStringLiteral("DEV2");
const QString kGoodSecret = QStringLiteral("correct-uia-answer");
const QString kWrongSecret = QStringLiteral("wrong-uia-answer");

}

class UiaSessionTest : public QObject
{
    Q_OBJECT

private:
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
        QVERIFY(ctl.busy());
        QCOMPARE(client.lastDeletedDevices, QStringList{ kOtherDevice });

        QVERIFY(finished.wait(kSignalTimeoutMs));
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished.at(0).at(0).toBool());
        QVERIFY(!finished.at(0).at(1).toString().isEmpty());
        QVERIFY(!ctl.busy());
        QVERIFY(!ctl.challengeActive());
    }

    void currentDeviceIsRefusedAsAGuard()
    {
        MockMatrixClient client;
        UiaController ctl;
        ctl.setClient(&client);

        QSignalSpy finished(&ctl, &UiaController::signOutFinished);
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
        QVERIFY(!ctl.challengeActive());
        QVERIFY(finished.wait(kSignalTimeoutMs));
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished.at(0).at(0).toBool());
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
        QTRY_VERIFY_WITH_TIMEOUT(ctl.challengeActive(), kSignalTimeoutMs);
        QVERIFY(ctl.wrongPassword());
        QVERIFY(ctl.passwordStage());
        QCOMPARE(finished.size(), 0);

        ctl.submitPassword(kGoodSecret);
        QVERIFY(finished.wait(kSignalTimeoutMs));
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished.at(0).at(0).toBool());
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
        QVERIFY(!finished.at(0).at(0).toBool());
        QVERIFY(finished.at(0).at(1).toString().contains(
            QStringLiteral("cancelled")));
        QCOMPARE(client.lastDeleteOp, quint64(0));

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
        QVERIFY(!finished.at(0).at(0).toBool());
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

        client.mockManagementUrl.clear();
        QSignalSpy finished(&ctl, &UiaController::signOutFinished);
        ctl.requestManagementUrl(QString());
        QVERIFY(finished.wait(kSignalTimeoutMs));
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.at(0).at(0).toBool());
        QVERIFY(!finished.at(0).at(1).toString().isEmpty());
        QCOMPARE(ready.size(), 1);
    }

    void loggedOutMidChallengeClearsIt()
    {
        MockMatrixClient client;
        UiaController ctl;
        ctl.setClient(&client);
        QVERIFY(raiseChallenge(ctl, client));

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
