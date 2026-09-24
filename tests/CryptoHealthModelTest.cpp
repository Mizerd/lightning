// The read-only E2EE health model. Snapshots are fed as the Rust bridge emits
// them (sanitized maps), pinning the mapping: readiness states, tri-state
// trust, backup/recovery distinctions, generation isolation and the
// no-secrets contract, without a homeserver or crypto store.

#include "crypto/CryptoHealthModel.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {
QVariantMap baseSnapshot()
{
    QVariantMap snapshot;
    snapshot.insert(QStringLiteral("device_id"), QStringLiteral("LIGHTDEV1"));
    snapshot.insert(QStringLiteral("device_verified"), false);
    snapshot.insert(QStringLiteral("device_cross_signed"), false);
    snapshot.insert(QStringLiteral("own_identity_available"), true);
    snapshot.insert(QStringLiteral("own_identity_verified"), false);
    snapshot.insert(QStringLiteral("has_master"), true);
    snapshot.insert(QStringLiteral("has_self_signing"), true);
    snapshot.insert(QStringLiteral("has_user_signing"), true);
    snapshot.insert(QStringLiteral("backup_exists_on_server"), false);
    snapshot.insert(QStringLiteral("backup_state"), QStringLiteral("unknown"));
    snapshot.insert(QStringLiteral("recovery_state"),
                    QStringLiteral("disabled"));
    snapshot.insert(QStringLiteral("secret_storage_enabled"), false);
    return snapshot;
}
} // namespace

class CryptoHealthModelTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // Backends without a crypto machine are unsupported, never an error, and
    // everything stays Unknown.
    void unsupportedBackendIsHonest()
    {
        CryptoHealthModel model;
        QVERIFY(!model.cryptoSupported());
        QVERIFY(!model.cryptoReady());
        QVERIFY(!model.cryptoError());
        QCOMPARE(model.currentDeviceVerified(), CryptoHealthModel::Unknown);
        QVERIFY(!model.statusSummary().isEmpty());
    }

    // Supported with no snapshot yet is initializing; the first snapshot
    // promotes to ready.
    void initializationPromotesToReady()
    {
        CryptoHealthModel model;
        model.setSupported(true);
        QVERIFY(model.cryptoInitializing());
        QVERIFY(!model.cryptoReady());

        model.applySnapshot(baseSnapshot(), model.generation());
        QVERIFY(!model.cryptoInitializing());
        QVERIFY(model.cryptoReady());
        QCOMPARE(model.currentDeviceId(), QStringLiteral("LIGHTDEV1"));
        QVERIFY(model.lastRefreshed().isValid());
    }

    // A dispatch failure is a distinct, recoverable state.
    void errorStateIsDistinctAndRecoverable()
    {
        CryptoHealthModel model;
        model.setSupported(true);
        model.setError(true);
        QVERIFY(model.cryptoError());
        QVERIFY(!model.cryptoReady());
        QVERIFY(!model.cryptoInitializing());
        // A later successful snapshot clears the error.
        model.applySnapshot(baseSnapshot(), model.generation());
        QVERIFY(!model.cryptoError());
        QVERIFY(model.cryptoReady());
    }

    // "Verified" for our own session means our identity has cross-signed it:
    // `is_cross_signed_by_owner()`, sent as `device_cross_signed`.
    void deviceTrustMapsToTriState()
    {
        CryptoHealthModel model;
        model.setSupported(true);

        QVariantMap unverified = baseSnapshot();
        model.applySnapshot(unverified, model.generation());
        QCOMPARE(model.currentDeviceVerified(), CryptoHealthModel::No);

        QVariantMap crossSigned = baseSnapshot();
        crossSigned.insert(QStringLiteral("device_cross_signed"), true);
        model.applySnapshot(crossSigned, model.generation());
        QCOMPARE(model.currentDeviceVerified(), CryptoHealthModel::Yes);

        QVariantMap identityVerified = baseSnapshot();
        identityVerified.insert(QStringLiteral("own_identity_verified"), true);
        model.applySnapshot(identityVerified, model.generation());
        QCOMPARE(model.ownIdentityVerified(), CryptoHealthModel::Yes);

        QVariantMap noIdentity = baseSnapshot();
        noIdentity.insert(QStringLiteral("own_identity_available"), false);
        noIdentity.insert(QStringLiteral("has_master"), false);
        noIdentity.insert(QStringLiteral("has_self_signing"), false);
        noIdentity.insert(QStringLiteral("has_user_signing"), false);
        model.applySnapshot(noIdentity, model.generation());
        QCOMPARE(model.ownIdentityVerified(), CryptoHealthModel::Unknown);
        QVERIFY(!model.crossSigningAvailable());
        QVERIFY(!model.crossSigningReady());
    }

    // `device_verified` must never promote this label. matrix-sdk-crypto marks
    // the own device `LocalTrust::Verified` on creation, so `is_verified()` is
    // always true for it and says nothing about cross-signing. matrix-sdk's
    // own `Encryption::verification_state()` uses
    // `is_cross_signed_by_owner()`. Rust no longer emits the field; this
    // guards against re-introduction by requiring the model to ignore it.
    void isVerifiedAloneMustNotPromoteTheOwnSessionsLabel()
    {
        CryptoHealthModel model;
        model.setSupported(true);

        // The live state: is_verified() true (always, for us), nothing
        // cross-signed, no keys.
        QVariantMap freshSession = baseSnapshot();
        freshSession.insert(QStringLiteral("device_verified"), true);
        freshSession.insert(QStringLiteral("device_cross_signed"), false);
        model.applySnapshot(freshSession, model.generation());
        QCOMPARE(model.currentDeviceVerified(), CryptoHealthModel::No);

        // Once a session of ours has cross-signed this one, it is Yes.
        QVariantMap vouchedFor = baseSnapshot();
        vouchedFor.insert(QStringLiteral("device_verified"), true);
        vouchedFor.insert(QStringLiteral("device_cross_signed"), true);
        model.applySnapshot(vouchedFor, model.generation());
        QCOMPARE(model.currentDeviceVerified(), CryptoHealthModel::Yes);
    }

    void crossSigningReadyRequiresAllKeys()
    {
        CryptoHealthModel model;
        model.setSupported(true);
        QVariantMap partial = baseSnapshot();
        partial.insert(QStringLiteral("has_user_signing"), false);
        model.applySnapshot(partial, model.generation());
        QVERIFY(model.crossSigningAvailable());
        QVERIFY(!model.crossSigningReady());

        model.applySnapshot(baseSnapshot(), model.generation());
        QVERIFY(model.crossSigningReady());
    }

    // Backup absent, existing but unusable, and usable are distinct states.
    void backupStatesAreDistinguished()
    {
        CryptoHealthModel model;
        model.setSupported(true);

        // An explicit false is a real answer: No.
        model.applySnapshot(baseSnapshot(), model.generation());
        QCOMPARE(model.keyBackupAvailable(), CryptoHealthModel::No);
        QVERIFY(!model.keyBackupUsable());

        // An absent field is "not known", not "no backup": the Rust probe does
        // a real GET /room_keys/version, which can fail transiently. Reporting
        // absence would invite abandoning a real recovery key and arm the
        // enable button, which can mint a new 4S key over the existing one.
        QVariantMap unknown = baseSnapshot();
        unknown.remove(QStringLiteral("backup_exists_on_server"));
        model.applySnapshot(unknown, model.generation());
        QCOMPARE(model.keyBackupAvailable(), CryptoHealthModel::Unknown);
        QVERIFY(!model.keyBackupUsable());

        // A null means the same as absent (a JSON null through QVariant).
        QVariantMap nulled = baseSnapshot();
        nulled.insert(QStringLiteral("backup_exists_on_server"), QVariant());
        model.applySnapshot(nulled, model.generation());
        QCOMPARE(model.keyBackupAvailable(), CryptoHealthModel::Unknown);

        QVariantMap untrusted = baseSnapshot();
        untrusted.insert(QStringLiteral("backup_exists_on_server"), true);
        untrusted.insert(QStringLiteral("backup_state"),
                         QStringLiteral("unknown"));
        model.applySnapshot(untrusted, model.generation());
        QCOMPARE(model.keyBackupAvailable(), CryptoHealthModel::Yes);
        QVERIFY(!model.keyBackupUsable());

        QVariantMap usable = baseSnapshot();
        usable.insert(QStringLiteral("backup_exists_on_server"), true);
        usable.insert(QStringLiteral("backup_state"),
                      QStringLiteral("enabled"));
        model.applySnapshot(usable, model.generation());
        QCOMPARE(model.keyBackupAvailable(), CryptoHealthModel::Yes);
        QVERIFY(model.keyBackupUsable());
    }

    void recoveryStatesMapHonestly()
    {
        CryptoHealthModel model;
        model.setSupported(true);

        model.applySnapshot(baseSnapshot(), model.generation());   // disabled
        QVERIFY(!model.recoveryAvailable());
        QVERIFY(!model.recoveryRequired());

        QVariantMap enabled = baseSnapshot();
        enabled.insert(QStringLiteral("recovery_state"),
                       QStringLiteral("enabled"));
        model.applySnapshot(enabled, model.generation());
        QVERIFY(model.recoveryAvailable());

        QVariantMap incomplete = baseSnapshot();
        incomplete.insert(QStringLiteral("recovery_state"),
                          QStringLiteral("incomplete"));
        model.applySnapshot(incomplete, model.generation());
        QVERIFY(model.recoveryRequired());
        QVERIFY(!model.recoveryAvailable());
    }

    // Logout or account switch resets everything, and snapshots from the
    // previous generation never repopulate the model.
    void generationIsolationAndReset()
    {
        CryptoHealthModel model;
        model.setSupported(true);
        const quint64 oldGeneration = model.generation();
        model.applySnapshot(baseSnapshot(), oldGeneration);
        QVERIFY(model.cryptoReady());

        model.resetForNewGeneration();
        QVERIFY(!model.cryptoReady());
        QVERIFY(model.currentDeviceId().isEmpty());
        QCOMPARE(model.currentDeviceVerified(), CryptoHealthModel::Unknown);
        QCOMPARE(model.pendingVerificationCount(), 0);

        // The old account's late answer is ignored.
        QSignalSpy spy(&model, &CryptoHealthModel::healthChanged);
        model.applySnapshot(baseSnapshot(), oldGeneration);
        QCOMPARE(spy.count(), 0);
        QVERIFY(!model.cryptoReady());

        // The new generation's snapshot applies.
        model.applySnapshot(baseSnapshot(), model.generation());
        QVERIFY(model.cryptoReady());
    }

    // The generation is captured when a query is dispatched and the answer is
    // applied with that value, so a session change in flight rejects the
    // stale answer.
    void dispatchCapturedGenerationRejectsAnswerAfterSessionChange()
    {
        CryptoHealthModel model;
        model.setSupported(true);

        // Dispatch a query: capture the generation.
        const quint64 dispatched = model.generation();

        // A session change happens before the answer.
        model.resetForNewGeneration();
        const quint64 fresh = model.generation();
        QVERIFY(fresh != dispatched);

        // The stale answer is dropped, not applied to the new session.
        QSignalSpy spy(&model, &CryptoHealthModel::healthChanged);
        model.applySnapshot(baseSnapshot(), dispatched);
        QCOMPARE(spy.count(), 0);
        QVERIFY(!model.cryptoReady());

        // The new session's answer applies.
        model.applySnapshot(baseSnapshot(), fresh);
        QVERIFY(model.cryptoReady());
    }

    void pendingVerificationCountIsBounded()
    {
        CryptoHealthModel model;
        model.setPendingVerificationCount(-3);
        QCOMPARE(model.pendingVerificationCount(), 0);
        model.setPendingVerificationCount(1);
        QCOMPARE(model.pendingVerificationCount(), 1);
    }

    // No secrets: a snapshot smuggling key-like fields never surfaces them;
    // the model exposes only its fixed semantic set.
    void modelExposesNoSecretValues()
    {
        CryptoHealthModel model;
        model.setSupported(true);
        QVariantMap sneaky = baseSnapshot();
        sneaky.insert(QStringLiteral("recovery_key"),
                      QStringLiteral("EsTk-not-a-real-key"));
        model.applySnapshot(sneaky, model.generation());

        const QMetaObject *meta = model.metaObject();
        for (int i = 0; i < meta->propertyCount(); ++i) {
            const QMetaProperty property = meta->property(i);
            const QString value =
                property.read(&model).toString().toLower();
            QVERIFY2(!value.contains(QStringLiteral("estk-not-a-real-key")),
                     property.name());
        }
    }
};

QTEST_MAIN(CryptoHealthModelTest)
#include "CryptoHealthModelTest.moc"
