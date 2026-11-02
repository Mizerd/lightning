
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
}

class CryptoHealthModelTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void unsupportedBackendIsHonest()
    {
        CryptoHealthModel model;
        QVERIFY(!model.cryptoSupported());
        QVERIFY(!model.cryptoReady());
        QVERIFY(!model.cryptoError());
        QCOMPARE(model.currentDeviceVerified(), CryptoHealthModel::Unknown);
        QVERIFY(!model.statusSummary().isEmpty());
    }

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

    void errorStateIsDistinctAndRecoverable()
    {
        CryptoHealthModel model;
        model.setSupported(true);
        model.setError(true);
        QVERIFY(model.cryptoError());
        QVERIFY(!model.cryptoReady());
        QVERIFY(!model.cryptoInitializing());
        model.applySnapshot(baseSnapshot(), model.generation());
        QVERIFY(!model.cryptoError());
        QVERIFY(model.cryptoReady());
    }

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

    void isVerifiedAloneMustNotPromoteTheOwnSessionsLabel()
    {
        CryptoHealthModel model;
        model.setSupported(true);

        QVariantMap freshSession = baseSnapshot();
        freshSession.insert(QStringLiteral("device_verified"), true);
        freshSession.insert(QStringLiteral("device_cross_signed"), false);
        model.applySnapshot(freshSession, model.generation());
        QCOMPARE(model.currentDeviceVerified(), CryptoHealthModel::No);

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

    void backupStatesAreDistinguished()
    {
        CryptoHealthModel model;
        model.setSupported(true);

        model.applySnapshot(baseSnapshot(), model.generation());
        QCOMPARE(model.keyBackupAvailable(), CryptoHealthModel::No);
        QVERIFY(!model.keyBackupUsable());

        QVariantMap unknown = baseSnapshot();
        unknown.remove(QStringLiteral("backup_exists_on_server"));
        model.applySnapshot(unknown, model.generation());
        QCOMPARE(model.keyBackupAvailable(), CryptoHealthModel::Unknown);
        QVERIFY(!model.keyBackupUsable());

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

        model.applySnapshot(baseSnapshot(), model.generation());
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

        QSignalSpy spy(&model, &CryptoHealthModel::healthChanged);
        model.applySnapshot(baseSnapshot(), oldGeneration);
        QCOMPARE(spy.count(), 0);
        QVERIFY(!model.cryptoReady());

        model.applySnapshot(baseSnapshot(), model.generation());
        QVERIFY(model.cryptoReady());
    }

    void dispatchCapturedGenerationRejectsAnswerAfterSessionChange()
    {
        CryptoHealthModel model;
        model.setSupported(true);

        const quint64 dispatched = model.generation();

        model.resetForNewGeneration();
        const quint64 fresh = model.generation();
        QVERIFY(fresh != dispatched);

        QSignalSpy spy(&model, &CryptoHealthModel::healthChanged);
        model.applySnapshot(baseSnapshot(), dispatched);
        QCOMPARE(spy.count(), 0);
        QVERIFY(!model.cryptoReady());

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

    void crossSigningSetupStateDistinguishesNoIdentityFromMissingKeys()
    {
        CryptoHealthModel model;
        QCOMPARE(model.crossSigningSetup(), QStringLiteral("unknown"));
        model.setSupported(true);
        QCOMPARE(model.crossSigningSetup(), QStringLiteral("unknown"));

        QVariantMap none = baseSnapshot();
        none.insert(QStringLiteral("own_identity_available"), false);
        none.insert(QStringLiteral("has_master"), false);
        none.insert(QStringLiteral("has_self_signing"), false);
        none.insert(QStringLiteral("has_user_signing"), false);
        model.applySnapshot(none, model.generation());
        QCOMPARE(model.crossSigningSetup(), QStringLiteral("not_set_up"));

        QVariantMap keysMissing = none;
        keysMissing.insert(QStringLiteral("own_identity_available"), true);
        model.applySnapshot(keysMissing, model.generation());
        QCOMPARE(model.crossSigningSetup(), QStringLiteral("keys_missing"));

        QVariantMap partial = keysMissing;
        partial.insert(QStringLiteral("has_master"), true);
        model.applySnapshot(partial, model.generation());
        QCOMPARE(model.crossSigningSetup(), QStringLiteral("keys_missing"));

        QVariantMap verified = baseSnapshot();
        verified.insert(QStringLiteral("device_cross_signed"), true);
        verified.insert(QStringLiteral("own_identity_verified"), true);
        model.applySnapshot(verified, model.generation());
        QCOMPARE(model.crossSigningSetup(), QStringLiteral("complete"));
    }

    void localKeysAloneAreNotACompleteSetup()
    {
        CryptoHealthModel model;
        model.setSupported(true);
        QVariantMap localOnly = baseSnapshot();
        localOnly.insert(QStringLiteral("device_cross_signed"), false);
        localOnly.insert(QStringLiteral("own_identity_verified"), true);
        model.applySnapshot(localOnly, model.generation());
        QVERIFY(model.crossSigningReady());
        QCOMPARE(model.crossSigningSetup(), QStringLiteral("unconfirmed"));

        QVariantMap signedButPartial = localOnly;
        signedButPartial.insert(QStringLiteral("device_cross_signed"), true);
        signedButPartial.insert(QStringLiteral("has_user_signing"), false);
        model.applySnapshot(signedButPartial, model.generation());
        QCOMPARE(model.crossSigningSetup(), QStringLiteral("keys_missing"));

        QVariantMap confirmed = localOnly;
        confirmed.insert(QStringLiteral("device_cross_signed"), true);
        model.applySnapshot(confirmed, model.generation());
        QCOMPARE(model.crossSigningSetup(), QStringLiteral("complete"));
    }
};

QTEST_MAIN(CryptoHealthModelTest)
#include "CryptoHealthModelTest.moc"
