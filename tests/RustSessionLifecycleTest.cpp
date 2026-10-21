#include "matrix/LoginFailureText.h"
#include "matrix/RustSessionPolicy.h"
#include "matrix/SessionLifecycleGuard.h"

#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

class RustSessionLifecycleTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void signOutInvalidatesOldCallbacks();
    void newLoginGetsNewGeneration();
    void unknownTokenSemanticsFollowGeneration();
    void passwordLoginStorePolicy();
    void oauthLoginStorePolicy();
    void oauthReauthorizationKeepsItsOwnStore();
    void restoreStorePolicy();
    void mismatchClassifierIsNarrow();
    void aSignInTheKeyringLostIsNeverAResetAndResumesOnlyItsOwnAccount();
    void aPasswordSignInFailureNeverShowsTheBackendsMessage();
    void theEventDrainStopsWithTheHandleItPolls();
    void aSignOutIsRecordedBeforeTheServerLogoutAndFinishedAtTheNextStart();

private:
    matrix::app_data::AccountIdentity identity(const QString &user) const;
    QTemporaryDir m_dataHome;
};

void RustSessionLifecycleTest::initTestCase()
{
    QVERIFY(m_dataHome.isValid());
    qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
}

matrix::app_data::AccountIdentity RustSessionLifecycleTest::identity(
    const QString &user) const
{
    matrix::app_data::AccountIdentity out;
    const bool ok = matrix::app_data::resolveAccountIdentity(
        QStringLiteral("https://matrix.example"), user, &out);
    Q_ASSERT(ok);
    return out;
}

void RustSessionLifecycleTest::signOutInvalidatesOldCallbacks()
{
    SessionLifecycleGuard guard;
    const quint64 old = guard.beginSession();
    QVERIFY(guard.acceptsActive(old));

    const quint64 signedOut = guard.beginSignOut(old);
    QVERIFY(signedOut > old);
    QVERIFY(!guard.acceptsActive(old));
    QVERIFY(guard.acceptsShutdownCompletion(old));

    // rooms, timelines, and sync errors all use acceptsActive(), so none of
    // them can repopulate or set Error once sign-out starts.
    QVERIFY(!guard.acceptsActive(old));
    guard.finishSignOut();
    QVERIFY(!guard.acceptsShutdownCompletion(old));
}

void RustSessionLifecycleTest::newLoginGetsNewGeneration()
{
    SessionLifecycleGuard guard;
    const quint64 first = guard.beginSession();
    guard.beginSignOut(first);
    guard.finishSignOut();
    const quint64 second = guard.beginSession();
    QVERIFY(second > first);
    QVERIFY(!guard.acceptsActive(first));
    QVERIFY(guard.acceptsActive(second));
}

void RustSessionLifecycleTest::unknownTokenSemanticsFollowGeneration()
{
    const QString error = QStringLiteral(
        "[401 / M_UNKNOWN_TOKEN] Invalid access token passed");
    QVERIFY(matrix::rust_session::isUnknownToken(error));

    SessionLifecycleGuard guard;
    const quint64 active = guard.beginSession();
    QVERIFY(guard.acceptsActive(active)); // active-session 401 remains real
    guard.beginSignOut(active);
    QVERIFY(!guard.acceptsActive(active)); // shutdown 401 is stale/ignored
}

void RustSessionLifecycleTest::passwordLoginStorePolicy()
{
    const auto target = identity(QStringLiteral("alice"));
    using Reason = matrix::rust_session::StoreBlockReason;

    // The saved-session inputs are the TARGET account's own record —
    // other signed-in accounts never influence a login decision.
    QCOMPARE(matrix::rust_session::passwordLoginBlockReason(
                 target, false, false, {}),
             Reason::None);
    // A fresh login for one account is allowed even when the target has no
    // record — regardless of what other accounts exist.
    QCOMPARE(matrix::rust_session::passwordLoginBlockReason(
                 target, false, true, QStringLiteral("DEVICE")),
             Reason::None);
    QCOMPARE(matrix::rust_session::passwordLoginBlockReason(
                 target, true, false, {}),
             Reason::MissingSessionMetadata);
    QCOMPARE(matrix::rust_session::passwordLoginBlockReason(
                 target, true, true, {}),
             Reason::MissingDeviceId);
    QCOMPARE(matrix::rust_session::passwordLoginBlockReason(
                 target, true, true, QStringLiteral("DEVICE")),
             Reason::ExistingStoreNeedsRestore);
}

// A device the authorization server just created must never be attached to a
// crypto store that belongs to a different device. OAuth only learns the
// account after the code exchange, so the check happens in phase B.
void RustSessionLifecycleTest::oauthLoginStorePolicy()
{
    using matrix::rust_session::oauthLoginBlockReason;
    using matrix::rust_session::StoreBlockReason;

    const auto target = identity(QStringLiteral("@alice:matrix.example"));

    // First OAuth sign-in for this account: nothing on disk, nothing to
    // collide with.
    QCOMPARE(oauthLoginBlockReason(target, /*storeExists=*/false,
                                   /*targetHasSavedSession=*/false, QString{},
                                   QStringLiteral("NEWDEVICE")),
             StoreBlockReason::None);

    // A store exists but no record proves whose it is.
    QCOMPARE(oauthLoginBlockReason(target, true, false, QString{},
                                   QStringLiteral("NEWDEVICE")),
             StoreBlockReason::MissingSessionMetadata);

    // A record without a device id cannot be compared against.
    QCOMPARE(oauthLoginBlockReason(target, true, true, QString{},
                                   QStringLiteral("NEWDEVICE")),
             StoreBlockReason::MissingDeviceId);

    // The server did not name the device: refuse rather than guess.
    QCOMPARE(oauthLoginBlockReason(target, true, true,
                                   QStringLiteral("OLDDEVICE"), QString{}),
             StoreBlockReason::MissingDeviceId);
    QCOMPARE(oauthLoginBlockReason(target, true, true,
                                   QStringLiteral("OLDDEVICE"),
                                   QStringLiteral("   ")),
             StoreBlockReason::MissingDeviceId);

    // THE case this policy exists for: sign out, sign in again with OAuth, get
    // a brand-new device, and find the previous device's store still on disk.
    // Adopting it would be the store/device ownership bug.
    QCOMPARE(oauthLoginBlockReason(target, true, true,
                                   QStringLiteral("OLDDEVICE"),
                                   QStringLiteral("NEWDEVICE")),
             StoreBlockReason::ExistingStoreNeedsRestore);

    // Device IDs are opaque, case-sensitive server strings. A case variant is
    // a DIFFERENT device and must not be treated as a match.
    QCOMPARE(oauthLoginBlockReason(target, true, true,
                                   QStringLiteral("ABCDEF"),
                                   QStringLiteral("abcdef")),
             StoreBlockReason::ExistingStoreNeedsRestore);
}

// Re-authorizing the SAME device — an expired OAuth session signing in again —
// is the ordinary path and must keep its own store, otherwise every token
// expiry would strand the account's Megolm history.
void RustSessionLifecycleTest::oauthReauthorizationKeepsItsOwnStore()
{
    using matrix::rust_session::oauthLoginBlockReason;
    using matrix::rust_session::StoreBlockReason;

    const auto target = identity(QStringLiteral("@alice:matrix.example"));

    QCOMPARE(oauthLoginBlockReason(target, /*storeExists=*/true,
                                   /*targetHasSavedSession=*/true,
                                   QStringLiteral("SAMEDEVICE"),
                                   QStringLiteral("SAMEDEVICE")),
             StoreBlockReason::None);

    // Surrounding whitespace in a stored record is trimmed on both sides
    // before the comparison, so a record written by an older build still
    // matches its own device.
    QCOMPARE(oauthLoginBlockReason(target, true, true,
                                   QStringLiteral("  SAMEDEVICE  "),
                                   QStringLiteral("SAMEDEVICE")),
             StoreBlockReason::None);

    // Refusing the sign-in must not suggest a destructive local reset: the
    // protected store belongs to a real device whose keys are still valid,
    // and the remedy is activating that account from the switcher.
    QVERIFY(!matrix::rust_session::suggestsLocalReset(
        StoreBlockReason::ExistingStoreNeedsRestore));
    // The reasons that DO warrant a reset are the ones where nothing
    // recoverable is being protected — a broken or unattributable record.
    QVERIFY(matrix::rust_session::suggestsLocalReset(
        StoreBlockReason::MissingSessionMetadata));
    QVERIFY(matrix::rust_session::suggestsLocalReset(
        StoreBlockReason::InvalidSavedIdentity));
}

void RustSessionLifecycleTest::restoreStorePolicy()
{
    const auto target = identity(QStringLiteral("alice"));
    using Reason = matrix::rust_session::StoreBlockReason;
    QCOMPARE(matrix::rust_session::restoreBlockReason(target, true, {}),
             Reason::MissingDeviceId);
    QCOMPARE(matrix::rust_session::restoreBlockReason(
                 target, false, QStringLiteral("DEVICE")),
             Reason::MissingStoreForSavedSession);
    QCOMPARE(matrix::rust_session::restoreBlockReason(
                 target, true, QStringLiteral("DEVICE")),
             Reason::None);
}

void RustSessionLifecycleTest::mismatchClassifierIsNarrow()
{
    QVERIFY(matrix::rust_session::isStoreOwnershipMismatch(QStringLiteral(
        "the store doesn't match the account in the constructor")));
    QVERIFY(matrix::rust_session::isStoreOwnershipMismatch(QStringLiteral(
        "account in the store doesn't match the restored session")));
    QVERIFY(matrix::rust_session::isStoreOwnershipMismatch(QStringLiteral(
        "This local Lightning Rust SDK store belongs to a different Matrix "
        "session or device.")));
    QVERIFY(!matrix::rust_session::isStoreOwnershipMismatch(QStringLiteral(
        "Matrix Rust SDK login failed: invalid password")));
    QVERIFY(!matrix::rust_session::isStoreOwnershipMismatch(QStringLiteral(
        "Matrix Rust SDK login failed: network unavailable")));
}

// The keyring answered "no such item" for a sign-in this install saved: the
// store is whole, so no reset, and only exactly that account on its own
// homeserver, signing in with a password, continues as the recorded device.
void RustSessionLifecycleTest::aSignInTheKeyringLostIsNeverAResetAndResumesOnlyItsOwnAccount()
{
    using matrix::rust_session::lostSignInDeviceMayResume;
    using Reason = matrix::rust_session::StoreBlockReason;
    QVERIFY(!matrix::rust_session::suggestsLocalReset(Reason::KeyringLostSession));
    QCOMPARE(matrix::rust_session::diagnosticName(Reason::KeyringLostSession),
             QStringLiteral("keyring_lost_session"));
    QVERIFY(!matrix::rust_session::suggestsLocalResetForCode(
        QStringLiteral("keyring_lost_session")));

    const QString user = QStringLiteral("@kai:example.org");
    const QString server = QStringLiteral("https://example.org");
    const QString device = QStringLiteral("KAIDEVICE");
    QVERIFY(lostSignInDeviceMayResume(user, server, user, server, device, false));
    // Another server typed with the full id: the password goes nowhere else.
    QVERIFY(!lostSignInDeviceMayResume(user, QStringLiteral("https://evil.example"),
                                       user, server, device, false));
    // Localparts are case-sensitive.
    QVERIFY(!lostSignInDeviceMayResume(QStringLiteral("@Kai:example.org"), server,
                                       user, server, device, false));
    // A browser sign-in cannot ask for the device.
    QVERIFY(!lostSignInDeviceMayResume(user, server, user, server, device, true));
    // Nothing to continue as.
    QVERIFY(!lostSignInDeviceMayResume(user, server, user, server, QString(), false));
    QVERIFY(!lostSignInDeviceMayResume(user, server, user, server,
                                       QStringLiteral("  "), false));
    QVERIFY(!lostSignInDeviceMayResume(QString(), server, user, server, device, false));
}

// B042/B119: an unknown account (Conduit's 404 M_NOT_FOUND) and every reason
// without its own words showed "Matrix Rust SDK login failed: the server
// returned an error: [404 / M_NOT_FOUND] This account does not exist." The
// backend's message is never the text; only the fixed reason token may show.
void RustSessionLifecycleTest::aPasswordSignInFailureNeverShowsTheBackendsMessage()
{
    using matrix::login_failure::passwordText;
    const QString backend = QStringLiteral(
        "Matrix Rust SDK login failed: the server returned an error: "
        "[404 / M_NOT_FOUND] This account does not exist.");
    const auto leaks = [&](const QString &text) {
        return text.contains(QLatin1String("Matrix Rust SDK"))
            || text.contains(QLatin1String("This account does not exist"))
            || text.contains(QLatin1String("the server returned"));
    };

    // An unknown account gets words of its own, and the full-id hint.
    const QString notFound = passwordText(QStringLiteral("http_404_M_NOT_FOUND"), backend);
    QVERIFY2(!leaks(notFound), qPrintable(notFound));
    QVERIFY2(notFound.contains(QLatin1String("@name:example.org")), qPrintable(notFound));
    // The ones that already had words keep them.
    QCOMPARE(passwordText(QStringLiteral("http_403_M_FORBIDDEN"), backend),
             QStringLiteral("Wrong username or password."));
    QCOMPARE(passwordText(QStringLiteral("connect"), backend),
             QStringLiteral("Can't reach the server. Check your connection and try again."));

    // A server that answers too slowly is not an unreachable one (C48-F08).
    const QString slow = passwordText(QStringLiteral("timeout"), backend);
    QVERIFY2(slow.contains(QLatin1String("taking too long")), qPrintable(slow));
    QVERIFY(!slow.contains(QLatin1String("Can't reach")));

    // Anything else: generic words plus the token as a support code.
    const QString other = passwordText(QStringLiteral("http_500_M_UNKNOWN"), backend);
    QVERIFY2(!leaks(other), qPrintable(other));
    QVERIFY2(other.contains(QLatin1String("(http_500_M_UNKNOWN)")), qPrintable(other));
    // A build failure that is a discovery failure says the server is unreachable;
    // any other build failure is generic, never the store's or SDK's text.
    const QString unreachable = passwordText(
        QStringLiteral("client_build_failed"),
        QStringLiteral("failed to build Matrix Rust SDK client: error sending "
                       "request for url (http://127.0.0.1:18115/.well-known/matrix/client)"));
    QVERIFY2(unreachable.startsWith(QLatin1String("Can't reach a Matrix server")),
             qPrintable(unreachable));
    QVERIFY(!unreachable.contains(QLatin1String("127.0.0.1")));
    const QString build = passwordText(
        QStringLiteral("client_build_failed"),
        QStringLiteral("failed to build Matrix Rust SDK client: Error from the SQLite store"));
    QVERIFY2(!build.contains(QLatin1String("SQLite")) && !leaks(build), qPrintable(build));
    // A token that is not from the fixed vocabulary is not echoed.
    const QString odd = passwordText(QStringLiteral("token=syl_abc <b>"), backend);
    QVERIFY2(!odd.contains(QLatin1String("syl_abc")) && !leaks(odd), qPrintable(odd));
    QVERIFY(!leaks(passwordText(QString(), backend)));
}

// B104: a failed sign-in releases the handle from inside handleRustEvent, and
// the drain went on polling it: 255 "error: Rust SDK backend handle is null."
// answers, each logged as a malformed event (20 shown, "repeated 235 times").
// A source contract, since the loop needs a live client: both drain loops end
// on an "error:" answer and when the handle they poll is released or replaced.
void RustSessionLifecycleTest::theEventDrainStopsWithTheHandleItPolls()
{
    QFile file(QStringLiteral(QT_TESTCASE_SOURCEDIR "/src/matrix/RustSdkMatrixClient.cpp"));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(file.errorString()));
    const QString source = QString::fromUtf8(file.readAll());
    const qsizetype start = source.indexOf(
        QLatin1String("void RustSdkMatrixClient::pollRustEvents()"));
    QVERIFY(start >= 0);
    const qsizetype end = source.indexOf(QLatin1String("\nvoid RustSdkMatrixClient::"), start + 1);
    QVERIFY(end > start);
    const QString body = source.mid(start, end - start);

    // Each loop from its poll call to the next one (or the end of the body).
    const QStringList polls = { QStringLiteral("mx_rust_poll_command_event(m_rustHandle)"),
                                QStringLiteral("mx_rust_poll_event(m_rustHandle)") };
    for (int i = 0; i < polls.size(); ++i) {
        const qsizetype from = body.indexOf(polls.at(i));
        QVERIFY2(from >= 0, qPrintable(polls.at(i)));
        const qsizetype to = i + 1 < polls.size() ? body.indexOf(polls.at(i + 1)) : body.size();
        QVERIFY(to > from);
        const QString loop = body.mid(from, to - from);
        QVERIFY2(loop.contains(QLatin1String("rustPollFailed(raw)")),
                 qPrintable(polls.at(i) + QStringLiteral(": an \"error:\" answer is not ended")));
        QVERIFY2(loop.contains(QRegularExpression(QStringLiteral(
                     "!m_rustHandle\\s*\\|\\|\\s*m_handleGeneration\\s*!=\\s*eventGeneration"))),
                 qPrintable(polls.at(i) + QStringLiteral(": keeps polling a released handle")));
    }
}

// B044: a quit or kill between the server logout and the store deletion left
// the store on disk with no record to find it by. A source contract over the
// client (it needs a live handle to run): the pending sign-out is recorded
// before either logout call, cleared only after the store removal succeeded,
// and finished when the client is constructed, before any restore.
void RustSessionLifecycleTest::aSignOutIsRecordedBeforeTheServerLogoutAndFinishedAtTheNextStart()
{
    QFile file(QStringLiteral(QT_TESTCASE_SOURCEDIR "/src/matrix/RustSdkMatrixClient.cpp"));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(file.errorString()));
    const QString source = QString::fromUtf8(file.readAll());
    const auto body = [&](const QString &signature) {
        const qsizetype start = source.indexOf(signature);
        if (start < 0)
            return QString();
        const qsizetype end =
            source.indexOf(QLatin1String("\nvoid RustSdkMatrixClient::"), start + 1);
        const qsizetype otherEnd =
            source.indexOf(QLatin1String("\nRustSdkMatrixClient::"), start + 1);
        qsizetype stop = end < 0 ? source.size() : end;
        if (otherEnd > 0 && otherEnd < stop)
            stop = otherEnd;
        return source.mid(start, stop - start);
    };

    const QString logout = body(QStringLiteral("void RustSdkMatrixClient::logout()"));
    QVERIFY(!logout.isEmpty());
    const qsizetype recorded = logout.indexOf(QLatin1String("recordPendingSignOut("));
    QVERIFY2(recorded >= 0, "logout() never records the pending sign-out");
    for (const char *call : { "mx_rust_logout(", "mx_rust_oauth_logout(", "finishSignOut(" }) {
        const qsizetype at = logout.indexOf(QLatin1String(call));
        QVERIFY2(at > recorded, call);
    }

    const QString finish = body(QStringLiteral("void RustSdkMatrixClient::finishSignOut("));
    QVERIFY(!finish.isEmpty());
    const qsizetype removal = finish.indexOf(QLatin1String("removeAccountRustState(identity)"));
    const qsizetype cleared = finish.indexOf(QLatin1String("clearPendingSignOut("));
    QVERIFY(removal >= 0);
    QVERIFY2(cleared > removal, "the marker must outlive a store that was not removed");
    // Both the store and the secrets (M1, review round 1): a keyring locked at
    // that moment leaves a possibly live token at rest, and the marker is the
    // only thing still naming it once the record is gone.
    QVERIFY(finish.mid(removal, cleared - removal)
                .contains(QLatin1String("files.ok() && sessionOk")));

    const QString interrupted =
        body(QStringLiteral("void finishInterruptedSignOuts(SettingsManager *settings)"));
    QVERIFY(!interrupted.isEmpty());
    const qsizetype startupSession = interrupted.indexOf(QLatin1String("clearSessionForAccount("));
    const qsizetype startupRemoval = interrupted.indexOf(QLatin1String("removeAccountRustState(identity)"));
    const qsizetype startupCleared = interrupted.indexOf(QLatin1String("clearPendingSignOut("));
    QVERIFY(startupSession >= 0 && startupRemoval >= 0);
    QVERIFY2(startupCleared > startupSession && startupCleared > startupRemoval,
             "the startup completion clears the marker before both halves ran");
    QVERIFY2(interrupted.mid(startupRemoval, startupCleared - startupRemoval)
                 .contains(QLatin1String("files.ok() && sessionOk")),
             "the startup completion forgets a sign-out whose secrets are still at rest");

    const QString constructor = body(QStringLiteral(
        "RustSdkMatrixClient::RustSdkMatrixClient(SettingsManager *settings"));
    QVERIFY2(constructor.contains(QLatin1String("finishInterruptedSignOuts(m_settings)")),
             "an interrupted sign-out is never finished at the next start");
}

QTEST_MAIN(RustSessionLifecycleTest)
#include "RustSessionLifecycleTest.moc"
