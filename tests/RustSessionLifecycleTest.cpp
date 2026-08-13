#include "matrix/RustSessionPolicy.h"
#include "matrix/SessionLifecycleGuard.h"

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

// The central OAuth safety property: a device the authorization server just
// created must never be attached to a crypto store that belongs to a
// different device. This is the same class of bug that password login was
// fixed for in v0.5.5, reached by a different route — OAuth only learns the
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

    // And critically: refusing the sign-in must NOT invite a destructive
    // local reset. The store this refusal protects belongs to a real device
    // whose keys are still valid — the remedy is activating that account from
    // the switcher, not deleting it. Offering "reset" here would turn a
    // correct safety refusal into the data-loss bug it exists to prevent.
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

QTEST_MAIN(RustSessionLifecycleTest)
#include "RustSessionLifecycleTest.moc"
