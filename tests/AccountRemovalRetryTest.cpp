
#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "matrix/MockMatrixClient.h"
#include "storage/AppDataPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

class AccountRemovalRetryTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_home;

private slots:
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        qputenv("XDG_DATA_HOME", m_home.path().toUtf8() + "/data");
        qputenv("XDG_CONFIG_HOME", m_home.path().toUtf8() + "/config");
        qputenv("HOME", m_home.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("account-removal-retry-test"));
        QSettings().clear();
    }

    void aPartlyFailedRemovalBecomesARetryCardAndARetryFinishesIt()
    {
        AppController controller(AppController::MockBackend);
        controller.settings()->setSecretStore(nullptr);
        const QString server = QStringLiteral("https://example.org");
        const QString uid = QStringLiteral("@ivy:example.org");
        controller.settings()->saveSession(server, uid, QStringLiteral("DEVICEONE"),
                                           QStringLiteral("old-token"), QString(),
                                           QStringLiteral("oauth"),
                                           QStringLiteral("client-id"));
        QVERIFY(controller.settings()->hasSavedAccount(uid));

        matrix::app_data::AccountIdentity identity;
        QVERIFY(matrix::app_data::resolveAccountIdentity(server, uid, &identity));
        const QString locked = identity.rustStorePath + QStringLiteral("/locked");
        QVERIFY(QDir().mkpath(locked));
        QFile keys(locked + QStringLiteral("/matrix-sdk-crypto.sqlite3"));
        QVERIFY(keys.open(QIODevice::WriteOnly));
        keys.write("keys");
        keys.close();
        QVERIFY(QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        const auto unlock = qScopeGuard([&locked] {
            QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                              | QFileDevice::ExeOwner);
        });
        {
            QFile probe(locked + QStringLiteral("/probe"));
            if (probe.open(QIODevice::WriteOnly)) {
                probe.close();
                probe.remove();
                QSKIP("Permissions do not stop deletion here (running as root?).");
            }
        }

        controller.setLocalSessionFailure(QStringLiteral("existing_store_requires_restore"),
                                          uid, server);
        controller.removeAccount(uid);

        QVERIFY(!controller.settings()->hasSavedAccount(uid));
        QCOMPARE(controller.localSessionFailureReasonCode(),
                 QStringLiteral("removal_incomplete"));
        QCOMPARE(controller.localSessionFailureUserId(), uid);
        const QStringList left = controller.accountRemovalLeftovers();
        QVERIFY2(left.contains(identity.rustStorePath), qPrintable(left.join(QLatin1Char(','))));
        QVERIFY(QFileInfo::exists(keys.fileName()));

        controller.retryAccountRemoval();
        QCOMPARE(controller.localSessionFailureReasonCode(),
                 QStringLiteral("removal_incomplete"));

        QVERIFY(QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                                  | QFileDevice::ExeOwner));
        controller.retryAccountRemoval();
        QCOMPARE(controller.localSessionFailureReasonCode(), QString());
        QVERIFY(controller.accountRemovalLeftovers().isEmpty());
        QVERIFY(!QFileInfo::exists(identity.rustStorePath));
        QVERIFY(!QFileInfo::exists(identity.accountRoot));
    }

    void aRetryNeverTouchesAnAccountThatSignedInAgain()
    {
        AppController controller(AppController::MockBackend);
        controller.settings()->setSecretStore(nullptr);
        const QString server = QStringLiteral("https://example.org");
        const QString uid = QStringLiteral("@kim:example.org");
        controller.settings()->saveSession(server, uid, QStringLiteral("DEVICEONE"),
                                           QStringLiteral("old-token"), QString(),
                                           QStringLiteral("oauth"),
                                           QStringLiteral("client-id"));
        matrix::app_data::AccountIdentity identity;
        QVERIFY(matrix::app_data::resolveAccountIdentity(server, uid, &identity));
        const QString locked = identity.rustStorePath + QStringLiteral("/locked");
        QVERIFY(QDir().mkpath(locked));
        QFile keys(locked + QStringLiteral("/matrix-sdk-crypto.sqlite3"));
        QVERIFY(keys.open(QIODevice::WriteOnly));
        keys.write("keys");
        keys.close();
        QVERIFY(QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        const auto unlock = qScopeGuard([&locked] {
            QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                              | QFileDevice::ExeOwner);
        });
        {
            QFile probe(locked + QStringLiteral("/probe"));
            if (probe.open(QIODevice::WriteOnly)) {
                probe.close();
                probe.remove();
                QSKIP("Permissions do not stop deletion here (running as root?).");
            }
        }
        controller.setLocalSessionFailure(QStringLiteral("existing_store_requires_restore"),
                                          uid, server);
        controller.removeAccount(uid);
        QCOMPARE(controller.localSessionFailureReasonCode(),
                 QStringLiteral("removal_incomplete"));

        controller.settings()->saveSession(server, uid, QStringLiteral("DEVICETWO"),
                                           QStringLiteral("new-token"), QString(),
                                           QStringLiteral("oauth"),
                                           QStringLiteral("client-id"));
        QVERIFY(QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                                  | QFileDevice::ExeOwner));
        controller.retryAccountRemoval();
        QVERIFY2(QFileInfo::exists(identity.rustStorePath),
                 "a retry deleted the store of an account that signed in again");
        QVERIFY(controller.settings()->hasSavedAccount(uid));
    }

    void aCompleteRemovalClearsTheCard()
    {
        AppController controller(AppController::MockBackend);
        controller.settings()->setSecretStore(nullptr);
        const QString server = QStringLiteral("https://example.org");
        const QString uid = QStringLiteral("@jo:example.org");
        controller.settings()->saveSession(server, uid, QStringLiteral("DEVICEONE"),
                                           QStringLiteral("old-token"), QString(),
                                           QStringLiteral("oauth"),
                                           QStringLiteral("client-id"));
        matrix::app_data::AccountIdentity identity;
        QVERIFY(matrix::app_data::resolveAccountIdentity(server, uid, &identity));
        QVERIFY(QDir().mkpath(identity.rustStorePath));

        controller.setLocalSessionFailure(QStringLiteral("existing_store_requires_restore"),
                                          uid, server);
        controller.removeAccount(uid);
        QCOMPARE(controller.localSessionFailureReasonCode(), QString());
        QVERIFY(controller.accountRemovalLeftovers().isEmpty());
        QVERIFY(!QFileInfo::exists(identity.rustStorePath));
    }

    void removingTheNeverConnectedAccountClearsAStaleErrorStatus()
    {
        AppController controller(AppController::MockBackend);
        controller.settings()->setSecretStore(nullptr);
        const QString server = QStringLiteral("https://example.org");
        const QString uid = QStringLiteral("@ivy:example.org");
        controller.settings()->saveSession(server, uid, QStringLiteral("DEVICEONE"),
                                           QStringLiteral("old-token"), QString(),
                                           QStringLiteral("oauth"),
                                           QStringLiteral("client-id"));
        QVERIFY(controller.settings()->hasSavedAccount(uid));

        auto *mock = qobject_cast<MockMatrixClient *>(
            controller.findChild<MatrixClient *>());
        QVERIFY(mock);
        mock->failNextRestoreForTest();
        QVERIFY(mock->restoreSession());
        QTRY_COMPARE(controller.connectionStatus(), QStringLiteral("Error"));
        QVERIFY(!mock->isLoggedIn());

        controller.removeAccount(uid);

        QVERIFY(!controller.settings()->hasSavedAccount(uid));
        QCOMPARE(controller.connectionStatus(), QStringLiteral("Not connected"));
    }
};

QTEST_MAIN(AccountRemovalRetryTest)
#include "AccountRemovalRetryTest.moc"
