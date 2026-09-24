// Active-account switch lifecycle, on a real AppController with the mock
// backend: activate a saved account, switch, check that room/composer
// targets clear, the switching state toggles, the login screen never shows
// mid-switch, logout falls back to a remaining account, and removing a
// background account leaves the active session alone.

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "auth/AccountManager.h"
#include "auth/AuthManager.h"
#include "gif/GifSearchController.h"
#include "gif/GifStarredStore.h"
#include "spaces/RailLayoutStore.h"
#include "storage/AppDataPaths.h"
#include "storage/InsecureFallbackSecretStore.h"
#include "storage/SecretStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSettings>
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
        if (m_locked) {
            m_lastReadFailed = true;
            return {};
        }
        m_lastReadFailed = false;
        return m_values.value(userId + QLatin1Char('/') + key);
    }

    // A keyring that locks after startup: every read comes back empty and the
    // backend says so. isAvailable() stays true because it is a
    // construction-time probe, which is why lastReadFailed() exists.
    void setLocked(bool locked) { m_locked = locked; }

    // The substituted-fallback shape: when a native backend probes
    // unavailable, SecretStore substitutes InsecureFallbackSecretStore, whose
    // lastReadFailed() is permanently true while reads still succeed.
    void setPermanentlyUnvouched(bool unvouched) { m_unvouched = unvouched; }

    bool lastReadFailed() const override
    {
        return m_unvouched || m_lastReadFailed;
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
    bool m_locked = false;
    mutable bool m_lastReadFailed = false;
    bool m_unvouched = false;
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

    // A message selection belongs to its room; leaving the room ends it.
    void switchingRoomsEndsAMessageSelection()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());

        const QString roomA = QStringLiteral("!mock-room:mock.local");
        app.setCurrentRoomId(roomA);
        app.forward()->beginSelecting(roomA);
        QVERIFY(app.forward()->selecting());

        app.setCurrentRoomId(QString());
        QVERIFY2(!app.forward()->selecting(),
                 "the selection followed the reader out of its room");
        QCOMPARE(app.forward()->selectedCount(), 0);
    }

    // Opening a room hydrates its member roster once per account session (it
    // feeds the displayNameFor cache for mentions, reply headers and thread
    // summaries). A switch or logout clears that memory.
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

        // A failed fetch un-marks the room so the next open retries. The mock
        // always succeeds, so the failure is injected through the backend's
        // signal.
        QVariantMap failed;
        failed.insert(QStringLiteral("ok"), false);
        Q_EMIT client->roomMembersReceived(0, roomA, failed);
        // (the injected emission is roster #2)
        app.setCurrentRoomId(QString());
        app.setCurrentRoomId(roomA);
        QTRY_COMPARE(rosters.count(), 3); // the retry fired

        // A different account starts fresh.
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
        // Switching state is set synchronously and the room target is cleared
        // before the previous session can route anything.
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

    // After A -> B the accounts list must re-notify, or the switcher keeps
    // stale isActive flags and clicking A hits the "already active" guard.
    // Drives A -> B -> A -> B -> A through the QML rows' decision inputs.
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
            // The list property re-notified since the last switch, so the QML
            // Repeater sees current flags.
            QSignalSpy listRefreshed(app.accounts(),
                                     &AccountManager::accountsChanged);
            QSignalSpy activeChanged(app.accounts(),
                                     &AccountManager::activeUserIdChanged);

            // The target row must not present as active, so its click reaches
            // switchToAccount.
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

        // After five hops both credentials are intact and nothing is stuck
        // switching.
        QVERIFY(!app.accountSwitching());
        QCOMPARE(app.settings()->accessTokenFor(kAlice),
                 QStringLiteral("alice-token-fixture"));
        QCOMPARE(app.settings()->accessTokenFor(kBob),
                 QStringLiteral("bob-token-fixture"));
    }

    // The account switched to is the one the next launch opens. This pins the
    // promise; durability without an event loop is pinned by
    // AccountRegistryTest::theActiveAccountIsOnDiskTheMomentItChanges.
    void theSwitchedToAccountIsTheOneTheNextLaunchOpens()
    {
        {
            AppController app(AppController::MockBackend);
            FakeSecretStore secrets;
            app.settings()->setSecretStore(&secrets);
            // alice is the first account signed into.
            app.settings()->saveSession(kHsOne, kAlice,
                                        QStringLiteral("ALICEDEV"),
                                        QStringLiteral("alice-token-fixture"));
            app.settings()->saveSession(kHsTwo, kBob,
                                        QStringLiteral("BOBDEV"),
                                        QStringLiteral("bob-token-fixture"));
            app.switchToAccount(kAlice);
            QTRY_VERIFY(!app.accountSwitching());
            QTRY_COMPARE(app.auth()->currentUserId(), kAlice);

            app.switchToAccount(kBob);
            QTRY_VERIFY(!app.accountSwitching());
            QTRY_COMPARE(app.auth()->currentUserId(), kBob);
            QCOMPARE(app.settings()->activeAccountUserId(), kBob);
        }

        // A fresh process reading the same registry.
        AppController relaunched(AppController::MockBackend);
        QCOMPARE(relaunched.settings()->activeAccountUserId(), kBob);
        QTRY_COMPARE(relaunched.auth()->currentUserId(), kBob);
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

        // The mock does not remove the account record on logout (the Rust
        // backend does); drop it so the fallback set is realistic.
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

        // Back returns to a healthy shell: no stale error, live session.
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

    // A genuine sign-out (AuthManager::logout()) deletes the starred-GIF store
    // of the account that was active, as server logout deletes the crypto
    // store.
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

        // The mock never clears the account record on logout, so with one saved
        // account the fallback signs the same account back in. What matters is
        // that the directory was deleted: reopening the store does not create
        // it (creation waits for the first star), so the deletion stays
        // observable.
        QTRY_VERIFY(!app.accountSwitching());
        QVERIFY(!QDir(starredDir).exists());
        QCOMPARE(app.gif()->starredStore()->count(), 0);
    }

    // Removing the active account goes through the same logout path, so the
    // starred-GIF store is deleted there too and the app falls back to the
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

    // Removing a background account (resolved from its saved record) deletes
    // its starred store and never touches another account's.
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

        // Alice is a background account; write her starred file directly in
        // GifStarredStore's content-addressed shape, since only the active
        // account's store is ever open.
        const QString aliceStarredDir = matrix::app_data::starredGifsDir(kAlice);
        QVERIFY(QDir().mkpath(aliceStarredDir));
        QFile aliceGif(aliceStarredDir + QStringLiteral("/") + QString(64, QLatin1Char('a'))
                       + QStringLiteral(".gif"));
        QVERIFY(aliceGif.open(QIODevice::WriteOnly));
        aliceGif.write("GIF89a\x10\x00\x10\x00", 10);
        aliceGif.close();

        // Bob (active) has his own starred GIF.
        const QByteArray gif = QByteArray("GIF89a\x10\x00\x10\x00", 10);
        app.gif()->starredStore()->starBytes(QStringLiteral("mk"), gif);
        QCOMPARE(app.gif()->starredStore()->count(), 1);
        const QString bobStarredDir = matrix::app_data::starredGifsDir(kBob);
        QVERIFY(QDir(bobStarredDir).exists());

        app.removeAccount(kAlice); // background account

        QVERIFY(!QDir(aliceStarredDir).exists());
        // Bob's store survives, on disk and in the live instance.
        QVERIFY(QDir(bobStarredDir).exists());
        QCOMPARE(app.gif()->starredStore()->count(), 1);
        QCOMPARE(app.auth()->currentUserId(), kBob);
    }

    // A live account switch is not a sign-out and must never delete the
    // outgoing account's starred store; detachSession emits loggedOut
    // mid-switch, so onLoggedOut is gated on m_accountSwitching.
    void switchingAccountsPreservesTheOutgoingStarredGifStore()
    {
        // Clean both accounts' directories first (the suite shares one XDG
        // home). Guard against an empty path: QDir(QString()) is ".", and
        // removeRecursively() would delete the working directory.
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

        // Alice's directory and bytes survive the switch.
        QVERIFY(QDir(aliceStarredDir).exists());
        QVERIFY(QFile::exists(aliceStarredDir + QStringLiteral("/")
                              + aliceGifFile));
        // The live store now belongs to Bob. QTRY: the open rides
        // onLoginSucceeded, which the mock delivers after accountSwitching
        // flips false.
        QTRY_COMPARE(app.gif()->starredStore()->currentDirectory(),
                     matrix::app_data::starredGifsDir(kBob));
        QCOMPARE(app.gif()->starredStore()->count(), 0);
        // Opening Bob's store did not touch Alice's data.
        QVERIFY(QDir(aliceStarredDir).exists());
        QVERIFY(QFile::exists(aliceStarredDir + QStringLiteral("/")
                              + aliceGifFile));
    }

    // The Spaces rail arrangement (Space ids and folder names) must not leak
    // between accounts: no device-global mirror key, and RailLayoutStore's
    // cache is invalidated on a switch.
    void theRailArrangementDoesNotFollowTheUserIntoTheNextAccount()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob, QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);

        const QString aliceSpace = QStringLiteral("!alice-space:one.example");
        RailLayoutStore *rail = app.railLayout();
        QVERIFY(rail);
        const QString folder = rail->createFolder(QStringLiteral("Work"));
        QVERIFY(!folder.isEmpty());
        rail->setSpaceFolder(aliceSpace, folder);
        QCOMPARE(rail->folders().size(), 1);
        QCOMPARE(rail->folderOf(aliceSpace), folder);

        // No device-global copy: it let a fresh account read the previous
        // one's Spaces and survived account removal.
        {
            QSettings raw;
            QVERIFY2(!raw.contains(QString::fromLatin1(
                         SettingsManager::kRailLayoutKey)),
                     "the arrangement was mirrored into a device-global key");
        }

        app.switchToAccount(kBob);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kBob);

        QVERIFY2(app.railLayout()->folders().isEmpty(),
                 "the next account inherited the previous one's folders");
        QVERIFY2(app.railLayout()->folderOf(aliceSpace).isEmpty(),
                 "the next account inherited the previous one's Space ids");

        // Bob's arrangement is his; Alice's survives.
        const QString bobSpace = QStringLiteral("!bob-space:two.example");
        const QString bobFolder =
            app.railLayout()->createFolder(QStringLiteral("Personal"));
        QVERIFY(!bobFolder.isEmpty());
        app.railLayout()->setSpaceFolder(bobSpace, bobFolder);

        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);
        QCOMPARE(app.railLayout()->folders().size(), 1);
        QCOMPARE(app.railLayout()->folderOf(aliceSpace), folder);
        QVERIFY(app.railLayout()->folderOf(bobSpace).isEmpty());
    }

    // Removing the active account still runs the recursive local wipe (the
    // account directory, named after the Matrix localpart, cache.sqlite and
    // any second store root), as it does for a background account.
    void removingTheActiveAccountStillDeletesItsLocalState()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob, QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);

        // Real local state: a starred GIF creates the account directory, and
        // cache.sqlite is removed only by the recursive wipe
        // (removeAccountRustState preserves it).
        const QByteArray gif = QByteArray("GIF89a\x10\x00\x10\x00", 10);
        app.gif()->starredStore()->starBytes(QStringLiteral("mk"), gif);
        const QString aliceRoot = matrix::app_data::accountRoot(kAlice);
        QVERIFY(!aliceRoot.isEmpty());
        QVERIFY(QDir(aliceRoot).exists());
        {
            QFile cache(aliceRoot + QStringLiteral("/cache.sqlite"));
            QVERIFY(cache.open(QIODevice::WriteOnly));
            cache.write("not a real database");
        }
        const QString bobRoot = matrix::app_data::accountRoot(kBob);
        QDir().mkpath(bobRoot);
        {
            QFile bobCache(bobRoot + QStringLiteral("/cache.sqlite"));
            QVERIFY(bobCache.open(QIODevice::WriteOnly));
            bobCache.write("bob's, and nobody asked about bob");
        }

        app.removeAccount(kAlice);

        // The wipe completes after the sign-out it waits for.
        QTRY_VERIFY2(!QDir(aliceRoot).exists(),
                     "removing the signed-in account left its local data — "
                     "the account directory is named after the localpart");
        QVERIFY(!app.settings()->hasSavedAccount(kAlice));
        QVERIFY(app.settings()->accessTokenFor(kAlice).isEmpty());

        // Never widened: the other account is untouched.
        QVERIFY(app.settings()->hasSavedAccount(kBob));
        QVERIFY(QFile::exists(bobRoot + QStringLiteral("/cache.sqlite")));
        QTRY_COMPARE(app.auth()->currentUserId(), kBob);
    }

    // An unparsable fallback settings file must report a failed read, not
    // answer every read empty; otherwise an empty token reads as "no saved
    // sign-in" and the user is sent to a destructive reset for a config
    // problem.
    void anUnreadableFallbackSecretStoreReportsThatItsReadFailed()
    {
        const QString appName = QCoreApplication::applicationName();
        // A private application name, so this malformed file is never the
        // suite's own settings file.
        QCoreApplication::setApplicationName(
            QStringLiteral("account-switch-test-corrupt-secrets"));
        QString path;
        {
            QSettings probe;
            path = probe.fileName();
        }
        QDir().mkpath(QFileInfo(path).absolutePath());
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            // A line with no '=' outside a comment: QSettings reports
            // FormatError and answers every value() empty.
            file.write("[secrets]\nthis line has no equals sign\n");
        }
        {
            // Not substituted for a native store: only the read itself can
            // report failure.
            InsecureFallbackSecretStore store(nullptr, false);
            QCOMPARE(store.readSecret(kAlice, QStringLiteral("accessToken")),
                     QString());
            QVERIFY2(store.lastReadFailed(),
                     "an unparsable store answered empty and called it a "
                     "fact about the account");
            QVERIFY(!store.lastError().isEmpty());
        }
        // A healthy store still reports a clean miss as a clean miss, or the
        // destructive path would be closed for everyone. Uses its own
        // application name again.
        QFile::remove(path);
        QCoreApplication::setApplicationName(
            QStringLiteral("account-switch-test-healthy-secrets"));
        {
            QSettings probe;
            path = probe.fileName();
        }
        {
            InsecureFallbackSecretStore store(nullptr, false);
            QVERIFY(store.storeSecret(kAlice, QStringLiteral("accessToken"),
                                      QStringLiteral("tok")));
            QCOMPARE(store.readSecret(kAlice, QStringLiteral("accessToken")),
                     QStringLiteral("tok"));
            QVERIFY(!store.lastReadFailed());
            QCOMPARE(store.readSecret(kBob, QStringLiteral("accessToken")),
                     QString());
            QVERIFY2(!store.lastReadFailed(),
                     "a genuine miss was reported as an unreadable backend");
            QVERIFY(store.clearAccountSecrets(kAlice));
        }
        QFile::remove(path);
        QCoreApplication::setApplicationName(appName);
    }

    // ---- an unreadable credential store is not a signed-out account ----
    // A locked keyring or missing session bus makes every lookup empty. That
    // must not read as "sign-in expired": a fresh password login is refused
    // as ExistingStoreNeedsRestore, which loops back to the switch. Uses
    // HttpBackend because the classifier exempts the mock.
    void aLockedKeyringIsNotAnExpiredSignIn()
    {
        AppController app(AppController::HttpBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.settings()->setActiveAccountUserId(kAlice);

        const QString expired = QCoreApplication::translate(
            "AppController",
            "That account's sign-in has expired. Sign in to it again.");

        secrets.setLocked(true);

        QSignalSpy errors(&app, &AppController::errorReported);
        app.switchToAccount(kBob);

        QCOMPARE(errors.count(), 1);
        const QString said = errors.first().first().toString();
        QVERIFY2(!said.isEmpty(), "the refusal said nothing at all");
        QVERIFY2(said != expired,
                 "a locked keyring was reported as an expired sign-in; the "
                 "one action that advice asks for is refused as "
                 "ExistingStoreNeedsRestore and lands the user back here");

        // The current session is untouched: switching detaches before it
        // restores.
        QVERIFY(!app.accountSwitching());
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);
    }

    // A machine with no keyring must still switch accounts: the substituted
    // insecure fallback reports lastReadFailed() permanently while reads
    // succeed, so classification must rest on a successful read.
    void aMachineWithNoKeyringStillSwitchesAccounts()
    {
        AppController app(AppController::HttpBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.settings()->setActiveAccountUserId(kAlice);

        // Reads keep working; the backend simply cannot vouch for them.
        secrets.setPermanentlyUnvouched(true);
        QVERIFY2(!app.settings()->accessTokenFor(kBob).isEmpty(),
                 "the fixture is wrong: the substituted fallback must still "
                 "read the token back");

        QSignalSpy errors(&app, &AppController::errorReported);
        app.switchToAccount(kBob);

        QVERIFY2(errors.isEmpty(),
                 qPrintable(QStringLiteral(
                     "a machine with no keyring was refused its own account: "
                     "\"%1\"")
                     .arg(errors.isEmpty()
                              ? QString()
                              : errors.first().first().toString())));
    }

    // Non-regression guard: with a readable backend, an account whose token
    // is genuinely gone is still reported as expired.
    void anAccountWithNoTokenAndAReadableBackendIsStillExpired()
    {
        AppController app(AppController::HttpBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.settings()->setActiveAccountUserId(kAlice);
        // Bob's token is gone and the store can say so.
        QVERIFY(secrets.clearAccountSecrets(kBob));
        QVERIFY(!app.settings()->secretBackendUnavailable());

        QSignalSpy errors(&app, &AppController::errorReported);
        app.switchToAccount(kBob);

        QCOMPARE(errors.count(), 1);
        QCOMPARE(errors.first().first().toString(),
                 QCoreApplication::translate(
                     "AppController",
                     "That account's sign-in has expired. Sign in to it "
                     "again."));
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);
    }

    // Signing out into a locked keyring still lands on the login screen
    // (nothing can be restored), but says which failure it is instead of
    // skipping every account as signed out.
    void aSignOutIntoALockedKeyringSaysWhichFailureItIs()
    {
        AppController app(AppController::HttpBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(kHsOne, kAlice,
                                    QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(kHsTwo, kBob,
                                    QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.settings()->setActiveAccountUserId(kAlice);

        secrets.setLocked(true);

        QSignalSpy errors(&app, &AppController::errorReported);
        app.auth()->logout();

        QCOMPARE(app.currentScreen(), AppController::LoginScreen);
        QVERIFY(errors.count() >= 1);
        QVERIFY2(!errors.last().first().toString().isEmpty(),
                 "the sign-out fallback dropped to the login screen without "
                 "saying that the credential store, not the accounts, is "
                 "what failed");
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_GUILESS_MAIN(AccountSwitchTest)
#include "AccountSwitchTest.moc"
