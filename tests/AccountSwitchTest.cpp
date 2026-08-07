// v0.7: active-account switch lifecycle tests. Boot a real AppController on
// the mock backend and drive the full switch path: activate a saved account,
// switch to another, confirm the room/composer targets clear, the switching
// state toggles, the login screen is never shown mid-switch, logout falls
// back to a remaining account, and background-account removal leaves the
// active session alone.

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "auth/AccountManager.h"
#include "auth/AuthManager.h"
#include "gif/GifSearchController.h"
#include "gif/GifStarredStore.h"
#include "storage/AppDataPaths.h"
#include "storage/SecretStore.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

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

    QString readSecret(const QString &userId,
                       const QString &key) const override
    {
        return m_values.value(userId + QLatin1Char('/') + key);
    }

    bool deleteSecret(const QString &userId, const QString &key) override
    {
        m_values.remove(userId + QLatin1Char('/') + key);
        return true;
    }

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

namespace {
const QString kAlice = QStringLiteral("@alice:one.example");
const QString kBob = QStringLiteral("@bob:two.example");
const QString kHsOne = QStringLiteral("https://one.example");
const QString kHsTwo = QStringLiteral("https://two.example");
} // namespace

class AccountSwitchTest : public QObject
{
    Q_OBJECT

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
            QStringLiteral("account-switch-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    // v0.6.5: opening a room hydrates its member roster exactly once per
    // account session — the roster feeds the displayNameFor cache that
    // mention chips, reply headers, and thread summaries resolve through
    // (before this, only the member panel or an @-composition ever fetched
    // it, so plain reading kept bare localparts). A switch/logout clears
    // the once-per-room memory so the next account hydrates afresh.
    void roomOpenHydratesMemberRosterOncePerSession()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());

        auto *client = app.findChild<MatrixClient *>();
        QVERIFY(client != nullptr);
        QSignalSpy rosters(client, &MatrixClient::roomMembersReceived);

        const QString roomA = QStringLiteral("!mock-room:mock.local");
        app.setCurrentRoomId(roomA);
        QTRY_COMPARE(rosters.count(), 1);

        // Re-opening the same room in the same session is a no-op fetch.
        app.setCurrentRoomId(QString());
        app.setCurrentRoomId(roomA);
        QTest::qWait(10);
        QCOMPARE(rosters.count(), 1);

        // A FAILED fetch un-marks the room: the next open retries instead
        // of failing closed for the whole session (the mock always answers
        // ok=true, so the failure is injected through the same signal the
        // backend would emit).
        QVariantMap failed;
        failed.insert(QStringLiteral("ok"), false);
        Q_EMIT client->roomMembersReceived(0, roomA, failed);
        // (the injected emission itself is rosters #2)
        app.setCurrentRoomId(QString());
        app.setCurrentRoomId(roomA);
        QTRY_COMPARE(rosters.count(), 3); // the retry actually fired

        // A different account starts fresh: the once-per-room memory died
        // with the detached session.
        app.switchToAccount(kBob);
        QTRY_VERIFY(!app.accountSwitching());
        app.setCurrentRoomId(roomA);
        QTRY_COMPARE(rosters.count(), 4);
    }

    void switchActivatesTargetWithoutLoginScreen()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));

        // Activate alice from the (logged-out) start state.
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QCOMPARE(app.auth()->currentUserId(), kAlice);
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);
        QCOMPARE(app.currentScreen(), AppController::MainScreen);

        // Open a room so the switch has composer/thread targets to clear.
        app.setCurrentRoomId(QStringLiteral("!mock-room:mock.local"));
        QVERIFY(!app.currentRoomId().isEmpty());

        bool sawLoginScreen = false;
        connect(&app, &AppController::currentScreenChanged, this,
                [&app, &sawLoginScreen] {
            if (app.currentScreen() == AppController::LoginScreen)
                sawLoginScreen = true;
        });

        app.switchToAccount(kBob);
        // Switching state is set synchronously and the room target is gone
        // before the previous session could route anything.
        QVERIFY(app.accountSwitching());
        QVERIFY(app.currentRoomId().isEmpty());

        QTRY_VERIFY(!app.accountSwitching());
        QCOMPARE(app.auth()->currentUserId(), kBob);
        QCOMPARE(app.settings()->activeAccountUserId(), kBob);
        QCOMPARE(app.settings()->deviceId(), QStringLiteral("BOBDEV"));
        QCOMPARE(app.currentScreen(), AppController::MainScreen);
        QVERIFY(!sawLoginScreen);

        // Both accounts keep their credentials.
        QCOMPARE(app.settings()->accessTokenFor(kAlice),
                 QStringLiteral("alice-token-fixture"));
        QCOMPARE(app.settings()->accessTokenFor(kBob),
                 QStringLiteral("bob-token-fixture"));
    }

    // The live regression: after A -> B, the switcher popover kept the
    // pre-switch isActive flags because the accounts LIST property never
    // re-notified on an active-account change — clicking A hit the
    // "already active" guard and silently did nothing, trapping the user
    // on B. Drive A -> B -> A -> B -> A through the same decision inputs
    // the QML rows use and assert the list refreshes on every hop.
    void repeatedSwitchingKeepsEveryRowSelectable()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));

        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QCOMPARE(app.accounts()->activeUserId(), kAlice);

        const auto rowState = [&app](const QString &userId) -> QVariantMap {
            const QVariantList rows = app.accounts()->accounts();
            for (const QVariant &row : rows) {
                const QVariantMap record = row.toMap();
                if (record.value(QStringLiteral("userId")).toString()
                    == userId)
                    return record;
            }
            return {};
        };

        const QStringList hops = { kBob, kAlice, kBob, kAlice };
        for (const QString &target : hops) {
            // The switcher's model refresh contract: the list property must
            // have re-notified since the last switch, so the QML Repeater
            // is looking at CURRENT flags, not the previous account's.
            QSignalSpy listRefreshed(app.accounts(),
                                     &AccountManager::accountsChanged);
            QSignalSpy activeChanged(app.accounts(),
                                     &AccountManager::activeUserIdChanged);

            // The QML row's click guard inputs (live state, fresh list):
            // the target row must NOT present as active, so the click
            // reaches switchToAccount.
            const QVariantMap targetRow = rowState(target);
            QVERIFY(!targetRow.isEmpty());
            QCOMPARE(targetRow.value(QStringLiteral("isActive")).toBool(),
                     false);
            QVERIFY(target != app.accounts()->activeUserId());

            app.switchToAccount(target);
            QTRY_VERIFY(!app.accountSwitching());
            QCOMPARE(app.auth()->currentUserId(), target);
            QCOMPARE(app.accounts()->activeUserId(), target);
            QCOMPARE(app.currentScreen(), AppController::MainScreen);

            // Both notify chains fired, so a bound switcher re-reads rows.
            QVERIFY(listRefreshed.count() > 0);
            QVERIFY(activeChanged.count() > 0);

            // The fresh list marks exactly the new account active.
            QCOMPARE(rowState(target).value(QStringLiteral("isActive"))
                         .toBool(),
                     true);
            const QString other = target == kAlice ? kBob : kAlice;
            QCOMPARE(rowState(other).value(QStringLiteral("isActive"))
                         .toBool(),
                     false);
        }

        // Five hops later both credentials are intact and nothing is stuck
        // in the switching state.
        QVERIFY(!app.accountSwitching());
        QCOMPARE(app.settings()->accessTokenFor(kAlice),
                 QStringLiteral("alice-token-fixture"));
        QCOMPARE(app.settings()->accessTokenFor(kBob),
                 QStringLiteral("bob-token-fixture"));
    }

    void switchToUnknownAccountFailsCleanly()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);

        QSignalSpy errors(&app, &AppController::errorReported);
        app.switchToAccount(QStringLiteral("@nobody:one.example"));
        QCOMPARE(errors.count(), 1);
        QVERIFY(!app.accountSwitching());
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);
        QCOMPARE(app.auth()->currentUserId(), kAlice);
    }

    void logoutContinuesWithRemainingAccount()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kBob);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kBob);

        // The mock backend does not remove the account record on logout
        // (the Rust backend does); drop it here so the fallback set is
        // realistic.
        app.settings()->clearSessionForAccount(kBob);
        app.auth()->logout();

        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);
        QCOMPARE(app.currentScreen(), AppController::MainScreen);
    }

    void failedAddAccountRestoresPreviousAccount()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);
        QCOMPARE(app.currentScreen(), AppController::MainScreen);

        // Enter add-account mode and fail the attempt (mock magic password).
        app.showLogin();
        QCOMPARE(app.currentScreen(), AppController::LoginScreen);
        app.auth()->login(kHsTwo, QStringLiteral("bob"),
                          QStringLiteral("mock-fail"));

        // The previous account's session is restored in the background; the
        // login screen stays up so the user can read the error and retry.
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);
        QTRY_VERIFY(app.auth()->isLoggedIn());
        QCOMPARE(app.currentScreen(), AppController::LoginScreen);
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);

        // Back returns to a healthy shell — no stale error, live session.
        app.showMain();
        QCOMPARE(app.currentScreen(), AppController::MainScreen);
        QVERIFY(app.auth()->isLoggedIn());

        // A retry that succeeds lands in the shell as the new account.
        app.showLogin();
        app.auth()->login(kHsTwo, QStringLiteral("bob"),
                          QStringLiteral("pw"));
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        QCOMPARE(app.auth()->currentUserId(),
                 QStringLiteral("@bob:two.example"));
    }

    void removingBackgroundAccountKeepsActiveSession()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kBob);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kBob);

        // Give alice on-disk account state to prove removal is scoped.
        const QString aliceRoot = matrix::app_data::accountRoot(kAlice);
        const QString bobRoot = matrix::app_data::accountRoot(kBob);
        QVERIFY(QDir().mkpath(aliceRoot + QStringLiteral("/matrix-rust-sdk-store")));
        QVERIFY(QDir().mkpath(bobRoot + QStringLiteral("/matrix-rust-sdk-store")));

        app.removeAccount(kAlice);

        QVERIFY(!app.settings()->hasSavedAccount(kAlice));
        QVERIFY(app.settings()->accessTokenFor(kAlice).isEmpty());
        QVERIFY(!QDir(aliceRoot).exists());
        // The active account is untouched.
        QCOMPARE(app.auth()->currentUserId(), kBob);
        QCOMPARE(app.settings()->activeAccountUserId(), kBob);
        QCOMPARE(app.settings()->accessTokenFor(kBob),
                 QStringLiteral("bob-token-fixture"));
        QVERIFY(QDir(bobRoot).exists());
    }

    // v0.6.6 (review CRITICAL-1): a genuine sign-out — the common case,
    // reached via AuthManager::logout(), not "remove account" — must delete
    // the client-local starred-GIF store for the account that WAS active.
    // Real (server) logout already deletes the Rust crypto store this same
    // way; this is the app-level store's own equivalent, exercised here
    // through AppController::onLoggedOut regardless of backend.
    void signOutDeletesStarredGifStore()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);

        const QByteArray gif = QByteArray("GIF89a\x10\x00\x10\x00", 10);
        app.gif()->starredStore()->starBytes(QStringLiteral("mk"), gif);
        QCOMPARE(app.gif()->starredStore()->count(), 1);
        const QString starredDir = matrix::app_data::starredGifsDir(kAlice);
        QVERIFY(QDir(starredDir).exists());

        app.auth()->logout();

        // The mock backend never clears the saved account record on
        // logout (see logoutContinuesWithRemainingAccount's own comment),
        // so with only one saved account onLoggedOut()'s fallback loop
        // finds that SAME account again and signs back in — a real-world
        // quirk of this backend, not what this test is about. What matters
        // for CRITICAL-1 is that the directory was actually deleted (never
        // just incidentally emptied by something that runs later): a fresh
        // re-login's openStarredStoreFor() never recreates a directory by
        // merely opening it (creation is deferred to the first real star),
        // so the deletion is still observable afterward.
        QTRY_VERIFY(!app.accountSwitching());
        QVERIFY(!QDir(starredDir).exists());
        QCOMPARE(app.gif()->starredStore()->count(), 0);
    }

    // "Remove account" on the currently-ACTIVE, logged-in account routes
    // through the exact same AuthManager::logout() path as a plain sign-out
    // (AppController::removeAccount's active branch) — the starred-GIF
    // store must be deleted there too, and the switch falls back to the
    // remaining account.
    void removingActiveAccountDeletesStarredGifStore()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);

        const QByteArray gif = QByteArray("GIF89a\x10\x00\x10\x00", 10);
        app.gif()->starredStore()->starBytes(QStringLiteral("mk"), gif);
        const QString aliceStarredDir = matrix::app_data::starredGifsDir(kAlice);
        QVERIFY(QDir(aliceStarredDir).exists());

        app.removeAccount(kAlice); // active + logged in -> logout path

        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kBob); // fell back to bob
        QVERIFY(!QDir(aliceStarredDir).exists());
    }

    // Background-account removal: the explicit starred-gifs cleanup fires
    // for the removed account, and — the point of this test — a DIFFERENT
    // account's starred store is never touched, even though the removed
    // account is resolved from a saved record and not the live session.
    void removingBackgroundAccountDeletesOnlyItsStarredGifStore()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kBob);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kBob);

        // Alice is a BACKGROUND account here (bob is active) — give her a
        // starred-gif file directly on disk (content-addressed, exactly
        // the shape GifStarredStore itself writes) without ever opening a
        // live store for her, since only the active account's store is
        // ever open.
        const QString aliceStarredDir = matrix::app_data::starredGifsDir(kAlice);
        QVERIFY(QDir().mkpath(aliceStarredDir));
        QFile aliceGif(aliceStarredDir + QStringLiteral("/") + QString(64, QLatin1Char('a'))
                       + QStringLiteral(".gif"));
        QVERIFY(aliceGif.open(QIODevice::WriteOnly));
        aliceGif.write("GIF89a\x10\x00\x10\x00", 10);
        aliceGif.close();

        // Bob (the ACTIVE account) has his own real starred GIF.
        const QByteArray gif = QByteArray("GIF89a\x10\x00\x10\x00", 10);
        app.gif()->starredStore()->starBytes(QStringLiteral("mk"), gif);
        QCOMPARE(app.gif()->starredStore()->count(), 1);
        const QString bobStarredDir = matrix::app_data::starredGifsDir(kBob);
        QVERIFY(QDir(bobStarredDir).exists());

        app.removeAccount(kAlice); // background account

        QVERIFY(!QDir(aliceStarredDir).exists());
        // Bob's store — a completely different account's data — survives
        // untouched, both on disk and in the live (still-open) instance.
        QVERIFY(QDir(bobStarredDir).exists());
        QCOMPARE(app.gif()->starredStore()->count(), 1);
        QCOMPARE(app.auth()->currentUserId(), kBob);
    }

    // The mirror image of the deletion tests, pinning the m_accountSwitching
    // gate itself: a LIVE-session account SWITCH is not a sign-out, and must
    // never delete the outgoing account's starred store. Without the gate in
    // AppController::onLoggedOut (detachSession emits loggedOut mid-switch)
    // every switch would silently destroy the outgoing account's decrypted
    // GIFs — and before this test, removing that gate left every suite
    // green.
    void switchingAccountsPreservesTheOutgoingStarredGifStore()
    {
        // Self-cleaning fixture: earlier cases in this binary legitimately
        // leave starred files behind (the suite shares one XDG home), and
        // this test's premise is that BOTH accounts start with none.
        // Guard against an empty path: QDir(QString()) resolves to "." and
        // removeRecursively() would then eat the working directory.
        const QString aliceDirToClean = matrix::app_data::starredGifsDir(kAlice);
        const QString bobDirToClean = matrix::app_data::starredGifsDir(kBob);
        QVERIFY(!aliceDirToClean.isEmpty());
        QVERIFY(!bobDirToClean.isEmpty());
        QDir(aliceDirToClean).removeRecursively();
        QDir(bobDirToClean).removeRecursively();

        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);

        const QByteArray gif = QByteArray("GIF89a\x10\x00\x10\x00", 10);
        app.gif()->starredStore()->starBytes(QStringLiteral("mk"), gif);
        const QString aliceStarredDir = matrix::app_data::starredGifsDir(kAlice);
        const QString aliceGifFile = QDir(aliceStarredDir)
                .entryList({ QStringLiteral("*.gif") }, QDir::Files)
                .value(0);
        QVERIFY(!aliceGifFile.isEmpty());

        app.switchToAccount(kBob); // live session -> detachSession path
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kBob);

        // Alice's directory AND her actual stored bytes survive the switch.
        QVERIFY(QDir(aliceStarredDir).exists());
        QVERIFY(QFile::exists(aliceStarredDir + QStringLiteral("/")
                              + aliceGifFile));
        // The live store instance now belongs to Bob: his (empty) directory,
        // none of Alice's rows. QTRY: the open for the incoming account
        // rides onLoginSucceeded, which the mock delivers through a
        // deferred hop after accountSwitching flips false.
        QTRY_COMPARE(app.gif()->starredStore()->currentDirectory(),
                     matrix::app_data::starredGifsDir(kBob));
        QCOMPARE(app.gif()->starredStore()->count(), 0);
        // And the survival assertions hold STILL — the open of Bob's store
        // must not have deleted or mutated Alice's data as a side effect.
        QVERIFY(QDir(aliceStarredDir).exists());
        QVERIFY(QFile::exists(aliceStarredDir + QStringLiteral("/")
                              + aliceGifFile));
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_GUILESS_MAIN(AccountSwitchTest)
#include "AccountSwitchTest.moc"
