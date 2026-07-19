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
