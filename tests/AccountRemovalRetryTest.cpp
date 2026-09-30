// "Remove this account" from the login screen's card, when some files will
// not go (an open database, a permission): the card must say what is still
// here and try again, and must never be left with buttons that no longer do
// anything (the record is gone, so "Open it" and "Remove" both return early).
//
// The retry deletes from the identity resolved before the record went; a
// store path is never re-derived (CLAUDE.md §6).

#include "app/AppController.h"
#include "app/SettingsManager.h"
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
        // Never the real keyring.
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
        // A directory whose contents cannot be deleted, as an open database on
        // Windows or a permission would do.
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

        // The login screen's card names the account; the user removes it.
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

        // Still blocked: a retry reports the same, and keeps the card.
        controller.retryAccountRemoval();
        QCOMPARE(controller.localSessionFailureReasonCode(),
                 QStringLiteral("removal_incomplete"));

        // Whatever held the files lets go; the retry finishes and the card
        // goes.
        QVERIFY(QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                                  | QFileDevice::ExeOwner));
        controller.retryAccountRemoval();
        QCOMPARE(controller.localSessionFailureReasonCode(), QString());
        QVERIFY(controller.accountRemovalLeftovers().isEmpty());
        QVERIFY(!QFileInfo::exists(identity.rustStorePath));
        QVERIFY(!QFileInfo::exists(identity.accountRoot));
    }

    // The card invites a new sign-in. Once the account is saved again (or a
    // sign-in is running), "Try again" deletes nothing: those files are the
    // new session's (review RR1).
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
        // A file inside: an EMPTY directory is removable without write access
        // to it (only its parent's), and the removal would then complete.
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

        // The user signs in as the same account again: a record is saved and
        // the store is the new session's.
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

    // A complete removal ends the card, as before.
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
};

QTEST_MAIN(AccountRemovalRetryTest)
#include "AccountRemovalRetryTest.moc"
