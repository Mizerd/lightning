// Own display name, end to end against the mock backend: AppController's
// policy, the caller-owned op id, and the account registry's cached copy.
//
// The own-profile fetch runs once per login, sync never carries the own
// profile, and SettingsManager::updateAccountProfile emits accountsChanged
// only on a real change. So the UI must confirm from the op, not wait for the
// registry, and the controller must re-fetch after a confirmed write.
//
// Policy, wiring and the mock backend only: the real set_display_name
// request and the MSC4133 field clear are not exercised here.

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "auth/AuthManager.h"
#include "matrix/MatrixClient.h"
#include "matrix/MockMatrixClient.h"
#include "storage/SecretStore.h"

#include <QHash>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

const QString kAlice = QStringLiteral("@alice:one.example");
const QString kHsOne = QStringLiteral("https://one.example");

// Mirrors AccountSwitchTest.cpp's own FakeSecretStore: an in-memory,
// always-available secret backend so saveSession()/switchToAccount() do
// not depend on a real Secret Service being reachable here.
class FakeSecretStore final : public SecretStore
{
    Q_OBJECT
public:
    explicit FakeSecretStore(QObject *parent = nullptr) : SecretStore(parent) {}

    bool isSecure() const override { return true; }
    bool isAvailable() const override { return true; }
    QString backendName() const override { return QStringLiteral("test"); }

    bool storeSecret(const QString &userId, const QString &key,
                     const QString &value) override
    {
        m_values.insert(userId + QLatin1Char('/') + key, value);
        return true;
    }
    QString readSecret(const QString &userId, const QString &key) const override
    { return m_values.value(userId + QLatin1Char('/') + key); }
    bool deleteSecret(const QString &userId, const QString &key) override
    { return m_values.remove(userId + QLatin1Char('/') + key) > 0; }
    bool clearAccountSecrets(const QString &userId) override
    {
        const QString prefix = userId + QLatin1Char('/');
        for (auto it = m_values.begin(); it != m_values.end();) {
            if (it.key().startsWith(prefix))
                it = m_values.erase(it);
            else
                ++it;
        }
        return true;
    }
    QString lastError() const override { return {}; }

private:
    QHash<QString, QString> m_values;
};

} // namespace

class OwnProfileTest : public QObject
{
    Q_OBJECT

private:
    static MockMatrixClient *mock(AppController &app)
    {
        return qobject_cast<MockMatrixClient *>(
            app.findChild<MatrixClient *>());
    }

    static QString cachedName(AppController &app)
    {
        return app.accounts()
            ->account(kAlice)
            .value(QStringLiteral("displayName"))
            .toString();
    }

    // Boot a signed-in AppController on the mock backend. The mock answers a
    // never-set name with the localpart, so the registry settles on "alice".
    static void signIn(AppController &app, FakeSecretStore &secrets)
    {
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.switchToAccount(kAlice);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("own-profile-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    // The mock knows its own account and seeded rows, and nothing else;
    // confirming arbitrary ids would change what other mock-backed surfaces
    // believe about strangers.
    void theMockResolvesItsOwnProfileAndNotFoundForStrangers()
    {
        MockMatrixClient client;
        QSignalSpy loggedIn(&client, &MatrixClient::loginSucceeded);
        client.login(QStringLiteral("https://mock.local"),
                     QStringLiteral("alice"), QStringLiteral("x"));
        QVERIFY(loggedIn.wait(2000));

        QSignalSpy profiles(&client, &MatrixClient::userProfileFinished);
        QVERIFY(client.fetchUserProfile(QStringLiteral("@alice:mock.local"))
                != 0);
        QTRY_COMPARE(profiles.count(), 1);
        QCOMPARE(profiles.at(0).at(1).toBool(), true);
        QCOMPARE(profiles.at(0).at(3).toString(), QStringLiteral("alice"));

        QVERIFY(client.fetchUserProfile(QStringLiteral("@stranger:elsewhere"))
                != 0);
        QTRY_COMPARE(profiles.count(), 2);
        QCOMPARE(profiles.at(1).at(1).toBool(), false);
        QCOMPARE(profiles.at(1).at(5).toString(),
                 QStringLiteral("not_found"));
    }

    // The round trip. The last assertion is the one that needs the
    // completion handler's re-fetch.
    void aConfirmedRenameRefreshesTheCachedName()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        signIn(app, secrets);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(cachedName(app), QStringLiteral("alice"));
        QVERIFY(app.canEditOwnDisplayName());

        QSignalSpy saved(&app, &AppController::ownDisplayNameSaved);
        QVERIFY(app.submitOwnDisplayName(QStringLiteral("Rokas Smetonis")));
        // Busy the instant the call returns: the op id is claimed BEFORE
        // the backend is called, so a synchronous answer cannot be dropped.
        QVERIFY(app.ownDisplayNameBusy());

        QTRY_COMPARE(saved.count(), 1);
        QVERIFY(!app.ownDisplayNameBusy());
        QVERIFY(app.ownDisplayNameError().isEmpty());
        QTRY_COMPARE(cachedName(app), QStringLiteral("Rokas Smetonis"));
    }

    // Confirmation comes from the op and arrives before the registry catches
    // up (and never, if the server already held the name).
    void confirmationArrivesBeforeTheRegistryCatchesUp()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        signIn(app, secrets);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(cachedName(app), QStringLiteral("alice"));

        QString nameAtConfirmation = QStringLiteral("<never fired>");
        connect(&app, &AppController::ownDisplayNameSaved, &app,
                [&app, &nameAtConfirmation] {
            nameAtConfirmation = cachedName(app);
        });
        QSignalSpy saved(&app, &AppController::ownDisplayNameSaved);
        QVERIFY(app.submitOwnDisplayName(QStringLiteral("Bravo")));
        QTRY_COMPARE(saved.count(), 1);
        QCOMPARE(nameAtConfirmation, QStringLiteral("alice"));
        QTRY_COMPARE(cachedName(app), QStringLiteral("Bravo"));
    }

    // A refusal keeps the write unconfirmed: no saved signal, the server's
    // message shown verbatim, and the stored name untouched.
    void aRefusalReportsTheServerMessageAndCachesNothing()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        signIn(app, secrets);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(cachedName(app), QStringLiteral("alice"));
        auto *client = mock(app);
        QVERIFY(client != nullptr);
        client->mockDisplayNameFailReason =
            QStringLiteral("Display name is not allowed on this server");

        QSignalSpy saved(&app, &AppController::ownDisplayNameSaved);
        QSignalSpy state(&app, &AppController::ownDisplayNameStateChanged);
        QVERIFY(app.submitOwnDisplayName(QStringLiteral("Blocked")));
        QTRY_VERIFY(!app.ownDisplayNameBusy());
        QCOMPARE(saved.count(), 0);
        QVERIFY(state.count() >= 2); // dispatch, then the failure
        QCOMPARE(app.ownDisplayNameError(),
                 QStringLiteral("Display name is not allowed on this server"));
        QVERIFY(!client->mockDisplayNames.contains(kAlice));
        QCOMPARE(cachedName(app), QStringLiteral("alice"));

        // A retry after the server stops refusing succeeds and clears the
        // error — a failure must not latch the editor.
        client->mockDisplayNameFailReason.clear();
        QVERIFY(app.submitOwnDisplayName(QStringLiteral("Blocked")));
        QTRY_COMPARE(saved.count(), 1);
        QVERIFY(app.ownDisplayNameError().isEmpty());
    }

    // A failure with NO server message (a timeout, a transport failure)
    // must not show an empty red line, and must not invent a sentence and
    // attribute it to the server.
    void aFailureWithNoServerMessageGetsOurOwnWording()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        signIn(app, secrets);
        QTRY_VERIFY(!app.accountSwitching());
        auto *client = mock(app);
        QVERIFY(client != nullptr);
        client->mockDisplayNameFailSilently = true;

        QVERIFY(app.submitOwnDisplayName(QStringLiteral("Timeout")));
        QTRY_VERIFY(!app.ownDisplayNameBusy());
        QVERIFY(!app.ownDisplayNameError().isEmpty());
    }

    // Single-flight: a second Save while one is in flight is suppressed.
    void aSecondSubmitWhileBusyIsSuppressed()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        signIn(app, secrets);
        QTRY_VERIFY(!app.accountSwitching());
        auto *client = mock(app);
        QVERIFY(client != nullptr);
        const int before = client->displayNameWrites;

        QVERIFY(app.submitOwnDisplayName(QStringLiteral("First")));
        QVERIFY(!app.submitOwnDisplayName(QStringLiteral("Second")));
        QVERIFY(!app.clearOwnDisplayName());
        QCOMPARE(client->displayNameWrites, before + 1);
        QTRY_VERIFY(!app.ownDisplayNameBusy());
    }

    // An emptied editor never erases the name; Clear is a separate action and
    // reaches the backend as an empty string (Rust maps it to None).
    void anEmptyNameIsRefusedButAnExplicitClearIsDispatched()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        signIn(app, secrets);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(cachedName(app), QStringLiteral("alice"));
        auto *client = mock(app);
        QVERIFY(client != nullptr);
        const int before = client->displayNameWrites;

        QVERIFY(!app.submitOwnDisplayName(QStringLiteral("   \t  ")));
        QVERIFY(!app.ownDisplayNameBusy());
        QVERIFY(!app.ownDisplayNameError().isEmpty());
        QCOMPARE(client->displayNameWrites, before);
        QCOMPARE(cachedName(app), QStringLiteral("alice"));

        QSignalSpy saved(&app, &AppController::ownDisplayNameSaved);
        QVERIFY(app.clearOwnDisplayName());
        QTRY_COMPARE(saved.count(), 1);
        QCOMPARE(client->displayNameWrites, before + 1);
        QVERIFY(client->mockDisplayNames.contains(kAlice));
        QVERIFY(client->mockDisplayNames.value(kAlice).isEmpty());
        // Present-but-empty reads back as empty, not as the localpart:
        // otherwise a successful clear would be indistinguishable from a
        // failed one on every identity surface.
        QTRY_VERIFY(cachedName(app).isEmpty());
    }

    // Unchanged is a no-op: nothing is sent, and no error is raised
    // either, because nothing went wrong.
    void anUnchangedNameIsNotSent()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        signIn(app, secrets);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(cachedName(app), QStringLiteral("alice"));
        auto *client = mock(app);
        QVERIFY(client != nullptr);
        const int before = client->displayNameWrites;

        QVERIFY(!app.submitOwnDisplayName(QStringLiteral("  alice  ")));
        QCOMPARE(client->displayNameWrites, before);
        QVERIFY(!app.ownDisplayNameBusy());
        QVERIFY(app.ownDisplayNameError().isEmpty());
    }

    // Length is counted in code points: UTF-16 units would make every emoji
    // cost two and could truncate between surrogates.
    void lengthIsCountedInCodePointsNotUtf16Units()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        signIn(app, secrets);
        QTRY_VERIFY(!app.accountSwitching());
        auto *client = mock(app);
        QVERIFY(client != nullptr);

        const QString fox = QString::fromUcs4(U"\U0001F98A");
        QCOMPARE(int(fox.size()), 2);                  // a surrogate PAIR
        QCOMPARE(app.displayNameLength(fox), 1);       // ONE character
        QCOMPARE(app.displayNameLength(QString(255, QLatin1Char('a'))), 255);

        // Exactly at the ceiling with an astral character as the 255th:
        // accepted, and it arrives whole.
        const QString atLimit = QString(254, QLatin1Char('a')) + fox;
        QCOMPARE(app.displayNameLength(atLimit), 255);
        QSignalSpy saved(&app, &AppController::ownDisplayNameSaved);
        QVERIFY(app.submitOwnDisplayName(atLimit));
        QTRY_COMPARE(saved.count(), 1);
        QCOMPARE(client->mockDisplayNames.value(kAlice), atLimit);

        // One character past it: REFUSED, never truncated. A silent cut
        // would send something the user did not type and then report it
        // as saved.
        const int before = client->displayNameWrites;
        const QString overLimit = QString(255, QLatin1Char('a')) + fox;
        QCOMPARE(app.displayNameLength(overLimit), 256);
        QVERIFY(!app.submitOwnDisplayName(overLimit));
        QCOMPARE(client->displayNameWrites, before);
        QVERIFY(!app.ownDisplayNameError().isEmpty());
        QCOMPARE(client->mockDisplayNames.value(kAlice), atLimit);
    }

    // No ASCII filter, no normalisation, no case folding: emoji, ZWJ
    // sequences, combining marks, non-Latin scripts and mixed scripts all
    // reach the backend byte-identical to what was typed.
    void unicodeNamesRoundTripUnchanged()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        signIn(app, secrets);
        QTRY_VERIFY(!app.accountSwitching());
        auto *client = mock(app);
        QVERIFY(client != nullptr);

        // Written as escapes, never as raw source bytes: a suite whose
        // own file encoding is load-bearing proves nothing about the
        // pipeline it is testing.
        const QStringList names = {
            QString::fromUcs4(U"\u0104\u017Euolas U\u017Eupis"),
            QString::fromUcs4(U"\u65E5\u672C\u8A9E\u306E\u540D\u524D"),
            // ZWJ family: four astral characters joined by U+200D.
            QString::fromUcs4(U"\U0001F468\u200D\U0001F469\u200D"
                              U"\U0001F467\u200D\U0001F466"),
            // Base letter + two combining marks.
            QString::fromUcs4(U"e\u0301\u0327"),
            QString::fromUcs4(U"\u03A9 mixed \u6F22\u5B57 \U0001F98A"),
        };
        for (const QString &name : names) {
            QSignalSpy saved(&app, &AppController::ownDisplayNameSaved);
            QVERIFY(app.submitOwnDisplayName(name));
            QTRY_COMPARE(saved.count(), 1);
            QCOMPARE(client->mockDisplayNames.value(kAlice), name);
        }
    }

    // A write in flight when the session goes away belongs to the account
    // that is going away. Retiring the op id is what makes the late answer
    // stale — without it the next account's editor takes it as ITS answer.
    void anAnswerThatOutlivedItsSessionIsDropped()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        signIn(app, secrets);
        QTRY_VERIFY(!app.accountSwitching());
        auto *client = mock(app);
        QVERIFY(client != nullptr);

        QSignalSpy saved(&app, &AppController::ownDisplayNameSaved);
        QVERIFY(app.submitOwnDisplayName(QStringLiteral("Doomed")));
        QVERIFY(app.ownDisplayNameBusy());
        // The mock's completion is already queued at this point; the
        // sign-out runs first and retires the op, which is what makes the
        // queued answer stale rather than merely late.
        app.auth()->logout();
        QVERIFY(!app.ownDisplayNameBusy());
        QTest::qWait(50);
        QCOMPARE(saved.count(), 0);
        QVERIFY(app.ownDisplayNameError().isEmpty());
    }

    // Signed out, nothing is offered and nothing is dispatched: the
    // command returns void, so an unanswerable dispatch would leave the
    // editor spinning forever with nothing left to answer it.
    void signedOutRefusesEverythingWithoutDispatching()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(!app.canEditOwnDisplayName());
        QVERIFY(!app.submitOwnDisplayName(QStringLiteral("Nobody")));
        QVERIFY(!app.clearOwnDisplayName());
        QVERIFY(!app.ownDisplayNameBusy());
        QVERIFY(!app.ownDisplayNameError().isEmpty());
        app.dismissOwnDisplayNameError();
        QVERIFY(app.ownDisplayNameError().isEmpty());
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_GUILESS_MAIN(OwnProfileTest)
#include "OwnProfileTest.moc"
