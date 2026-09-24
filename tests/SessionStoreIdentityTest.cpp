// The account-identity <-> SDK-store binding. Matrix localparts are
// case-sensitive: resolveAccountIdentity() keeps what the user typed, while
// the homeserver answers with its canonical id, which is what gets persisted.
// The invariant: the store path used to create a session equals the store
// path used to restore it, and resets act on the saved account.

#include "app/SettingsManager.h"
#include "matrix/RustSessionPolicy.h"
#include "storage/AppDataPaths.h"
#include "storage/SecretStore.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QTemporaryDir>
#include <QtTest>

namespace {

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
    {
        return m_values.value(userId + QLatin1Char('/') + key);
    }
    bool deleteSecret(const QString &userId, const QString &key) override
    {
        m_values.remove(userId + QLatin1Char('/') + key);
        return true;
    }
    // Like the real backends, clearing an account with no secrets is a
    // successful no-op, so the return value cannot say whether a reset did
    // anything.
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
    bool hasSecret(const QString &userId, const QString &key) const
    {
        return m_values.contains(userId + QLatin1Char('/') + key);
    }

private:
    QHash<QString, QString> m_values;
};

constexpr auto kServer = "https://matrix.example";

} // namespace

class SessionStoreIdentityTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();

    // Identity → store path binding.
    void sameAccountResolvesToOneStoreAcrossRestarts();
    void typedLocalpartCaseIsPreservedByResolution();
    void typedCaseVariantAdoptsTheSavedCanonicalAccount();
    void exactMatchWinsOverCaseInsensitiveSibling();
    void ambiguousSavedCasingsRefuseCanonicalization();
    void distinctUsersOnOneHomeserverStayIsolated();
    void sameLocalpartOnDifferentHomeserversStaysIsolated();
    void equivalentHomeserverUrlsShareOneSlot();

    // Store adoption by recording.
    void adoptionRecordsTheStoreAndIsIdempotent();
    void recordingSurvivesRestartAndIsReversible();
    void ambiguousOwnershipRefusesAdoption();
    void bindingRefusesUnsafeOrUnscopedSlugs();
    void adoptionLeavesOtherAccountsUntouched();
    void delegatedHomeserverSlugIsReconstructedExactly();
    void delegationDivergenceIsAdoptableAndCaseScanIsNot();
    void savedSessionWithoutStoreEndsInASignInableState();

    // login() orphan-cleanup safety.
    void delegatedStoreIsOwnedAndSurvivesLoginOrphanCleanup();
    void ownershipCheckCoversAllThreeBindings();
    void unclaimedStoreIsQuarantinedNotDeleted();
    void asciiOnlyCaseFoldingForAdoptionCandidates();
    void repairQuarantinesTheStoreInsteadOfDeletingIt();
    void unreadableSecretBackendIsNeverADestructiveVerdict();
    void anUnreadableSecretBlocksTheLoginThatWouldDeleteTheStore();
    void codeKeyedResetPolicyMatchesTheEnum();

    // Reset honesty.
    void resetOfUnknownAccountReportsNoMatch();
    void resetOfCaseVariantMatchesTheSavedRecord();
    void removalSummaryDistinguishesMissingFromDeleted();

    // Failure classification.
    void everyBlockReasonHasItsOwnCode();
    void missingStoreIsNotReportedAsAForeignStore();
    void onlyRepairableReasonsOfferALocalReset();

private:
    matrix::app_data::AccountIdentity identityFor(
        const QString &user, const QString &homeserver = QLatin1String(kServer)) const;
    // Create <primaryRoot>/<slug>/matrix-rust-sdk-store with one marker file,
    // so a migration can be shown to have kept the same store.
    void seedStore(const QString &slug, const QString &marker) const;
    QString markerIn(const QString &slug) const;
    bool storeExists(const QString &slug) const;

    QTemporaryDir m_dataHome;
    QTemporaryDir m_configHome;
    std::unique_ptr<SettingsManager> m_settings;
    std::unique_ptr<FakeSecretStore> m_secrets;
};

void SessionStoreIdentityTest::initTestCase()
{
    QVERIFY(m_dataHome.isValid());
    QVERIFY(m_configHome.isValid());
    qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
    QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
    QCoreApplication::setApplicationName(QStringLiteral("session-store-identity"));
}

void SessionStoreIdentityTest::init()
{
    // Every test starts from an empty registry and an empty data root.
    m_settings.reset();
    QDir(m_configHome.path()).removeRecursively();
    QDir().mkpath(m_configHome.path());
    const QString root = matrix::app_data::primaryRoot();
    QVERIFY(!root.isEmpty());
    QDir(root).removeRecursively();
    QVERIFY(QDir().mkpath(root));

    m_secrets = std::make_unique<FakeSecretStore>();
    m_settings = std::make_unique<SettingsManager>();
    m_settings->setSecretStore(m_secrets.get());
}

matrix::app_data::AccountIdentity SessionStoreIdentityTest::identityFor(
    const QString &user, const QString &homeserver) const
{
    matrix::app_data::AccountIdentity out;
    const bool ok =
        matrix::app_data::resolveAccountIdentity(homeserver, user, &out);
    Q_ASSERT(ok);
    return out;
}

void SessionStoreIdentityTest::seedStore(const QString &slug,
                                         const QString &marker) const
{
    const QString store = matrix::app_data::primaryRoot()
        + QLatin1Char('/') + slug + QLatin1String("/matrix-rust-sdk-store");
    QVERIFY(QDir().mkpath(store));
    QFile f(store + QLatin1String("/marker"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(marker.toUtf8());
    f.close();
}

QString SessionStoreIdentityTest::markerIn(const QString &slug) const
{
    QFile f(matrix::app_data::primaryRoot() + QLatin1Char('/') + slug
            + QLatin1String("/matrix-rust-sdk-store/marker"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

bool SessionStoreIdentityTest::storeExists(const QString &slug) const
{
    return QFileInfo(matrix::app_data::primaryRoot() + QLatin1Char('/') + slug
                     + QLatin1String("/matrix-rust-sdk-store")).isDir();
}

// --- identity → store path -------------------------------------------------

void SessionStoreIdentityTest::sameAccountResolvesToOneStoreAcrossRestarts()
{
    // A login persists the server-canonical id, and every later start
    // re-derives the store path from it, landing on the store the login
    // created.
    const auto atLogin = identityFor(QStringLiteral("@alice:matrix.example"));
    seedStore(atLogin.slug, QStringLiteral("real-store"));

    m_settings->saveSession(QLatin1String(kServer),
                            QStringLiteral("@alice:matrix.example"),
                            QStringLiteral("DEVICE1"),
                            QStringLiteral("token"));

    // "Restart": a fresh SettingsManager reading the persisted registry.
    m_settings.reset();
    m_settings = std::make_unique<SettingsManager>();
    m_settings->setSecretStore(m_secrets.get());

    const auto atRestore = identityFor(m_settings->userId(),
                                       m_settings->homeserverUrl());
    QCOMPARE(atRestore.rustStorePath, atLogin.rustStorePath);
    QCOMPARE(markerIn(atRestore.slug), QStringLiteral("real-store"));
}

void SessionStoreIdentityTest::typedLocalpartCaseIsPreservedByResolution()
{
    // Localparts are not lowercased: uppercase localparts are legal Matrix ids,
    // and folding them would alias two accounts onto one store. Divergence is
    // repaired by canonicalizing against saved records.
    const auto upper = identityFor(QStringLiteral("@Mizerd:matrix.example"));
    const auto lower = identityFor(QStringLiteral("@mizerd:matrix.example"));
    QCOMPARE(upper.userId, QStringLiteral("@Mizerd:matrix.example"));
    QCOMPARE(lower.userId, QStringLiteral("@mizerd:matrix.example"));
    QVERIFY(upper.rustStorePath != lower.rustStorePath);
    // The server name IS lowercased, so only the localpart can diverge.
    QCOMPARE(identityFor(QStringLiteral("@mizerd:MATRIX.EXAMPLE")).userId,
             lower.userId);
}

void SessionStoreIdentityTest::typedCaseVariantAdoptsTheSavedCanonicalAccount()
{
    // The user types "Mizerd" and the homeserver knows "@mizerd:...": the
    // login lands on the saved account's store instead of creating a second.
    m_settings->saveSession(QLatin1String(kServer),
                            QStringLiteral("@mizerd:matrix.example"),
                            QStringLiteral("DEVICE1"),
                            QStringLiteral("token"));

    bool ambiguous = true;
    const QString canonical = m_settings->canonicalUserIdForTypedIdentity(
        QStringLiteral("@Mizerd:matrix.example"), &ambiguous);
    QVERIFY(!ambiguous);
    QCOMPARE(canonical, QStringLiteral("@mizerd:matrix.example"));

    const auto typed = identityFor(QStringLiteral("@Mizerd:matrix.example"));
    const auto adopted = identityFor(canonical);
    QVERIFY(typed.rustStorePath != adopted.rustStorePath);
    QCOMPARE(adopted.rustStorePath,
             identityFor(m_settings->userId()).rustStorePath);
}

void SessionStoreIdentityTest::exactMatchWinsOverCaseInsensitiveSibling()
{
    // Both casings are saved, real accounts: an exact hit is never redirected
    // to its sibling.
    m_settings->saveSession(QLatin1String(kServer),
                            QStringLiteral("@mizerd:matrix.example"),
                            QStringLiteral("D1"), QStringLiteral("t1"));
    m_settings->saveSession(QLatin1String(kServer),
                            QStringLiteral("@Mizerd:matrix.example"),
                            QStringLiteral("D2"), QStringLiteral("t2"));

    bool ambiguous = true;
    QCOMPARE(m_settings->canonicalUserIdForTypedIdentity(
                 QStringLiteral("@Mizerd:matrix.example"), &ambiguous),
             QStringLiteral("@Mizerd:matrix.example"));
    QVERIFY(!ambiguous);
    QCOMPARE(m_settings->canonicalUserIdForTypedIdentity(
                 QStringLiteral("@mizerd:matrix.example")),
             QStringLiteral("@mizerd:matrix.example"));
}

void SessionStoreIdentityTest::ambiguousSavedCasingsRefuseCanonicalization()
{
    // Two saved accounts differ only by case and the typed id matches neither
    // exactly: no guess.
    m_settings->saveSession(QLatin1String(kServer),
                            QStringLiteral("@mizerd:matrix.example"),
                            QStringLiteral("D1"), QStringLiteral("t1"));
    m_settings->saveSession(QLatin1String(kServer),
                            QStringLiteral("@Mizerd:matrix.example"),
                            QStringLiteral("D2"), QStringLiteral("t2"));

    bool ambiguous = false;
    QCOMPARE(m_settings->canonicalUserIdForTypedIdentity(
                 QStringLiteral("@MIZERD:matrix.example"), &ambiguous),
             QString());
    QVERIFY(ambiguous);
}

void SessionStoreIdentityTest::distinctUsersOnOneHomeserverStayIsolated()
{
    const auto alice = identityFor(QStringLiteral("@alice:matrix.example"));
    const auto bob = identityFor(QStringLiteral("@bob:matrix.example"));
    QVERIFY(alice.accountRoot != bob.accountRoot);
    QVERIFY(alice.rustStorePath != bob.rustStorePath);

    m_settings->saveSession(QLatin1String(kServer),
                            QStringLiteral("@alice:matrix.example"),
                            QStringLiteral("D1"), QStringLiteral("t1"));
    // A different localpart is never a case variant of ours.
    QCOMPARE(m_settings->canonicalUserIdForTypedIdentity(
                 QStringLiteral("@bob:matrix.example")),
             QString());
    seedStore(bob.slug, QStringLiteral("bob"));
    QVERIFY(matrix::app_data::findCaseVariantStoreSlugs(alice).isEmpty());
}

void SessionStoreIdentityTest::sameLocalpartOnDifferentHomeserversStaysIsolated()
{
    const auto here = identityFor(QStringLiteral("@alice:matrix.example"));
    const auto there = identityFor(QStringLiteral("@alice:other.example"),
                                   QStringLiteral("https://other.example"));
    QVERIFY(here.rustStorePath != there.rustStorePath);

    m_settings->saveSession(QLatin1String(kServer),
                            QStringLiteral("@alice:matrix.example"),
                            QStringLiteral("D1"), QStringLiteral("t1"));
    // Same localpart, different server: not a candidate for canonicalization.
    QCOMPARE(m_settings->canonicalUserIdForTypedIdentity(
                 QStringLiteral("@Alice:other.example")),
             QString());
    seedStore(there.slug, QStringLiteral("other-server"));
    QVERIFY(matrix::app_data::findCaseVariantStoreSlugs(here).isEmpty());
}

void SessionStoreIdentityTest::equivalentHomeserverUrlsShareOneSlot()
{
    // URL spellings of the same homeserver do not multiply the account's
    // storage slots.
    const auto plain = identityFor(QStringLiteral("alice"),
                                   QStringLiteral("https://matrix.example"));
    for (const QString &variant : {QStringLiteral("https://matrix.example/"),
                                   QStringLiteral("https://matrix.example///"),
                                   QStringLiteral("HTTPS://MATRIX.EXAMPLE"),
                                   QStringLiteral("  https://Matrix.Example/  ")}) {
        const auto other = identityFor(QStringLiteral("alice"), variant);
        QCOMPARE(other.userId, plain.userId);
        QCOMPARE(other.homeserver, plain.homeserver);
        QCOMPARE(other.rustStorePath, plain.rustStorePath);
    }

    // An explicit port is part of the server name, so a different identity,
    // but still stable and slash-insensitive.
    const auto ported = identityFor(QStringLiteral("alice"),
                                    QStringLiteral("https://matrix.example:8448"));
    const auto portedSlash = identityFor(QStringLiteral("alice"),
                                         QStringLiteral("https://matrix.example:8448/"));
    QCOMPARE(ported.rustStorePath, portedSlash.rustStorePath);
    QVERIFY(ported.rustStorePath != plain.rustStorePath);
}

// --- adoption / migration --------------------------------------------------

void SessionStoreIdentityTest::adoptionRecordsTheStoreAndIsIdempotent()
{
    // Adoption points the account at the divergent directory and never moves
    // it: the store holds the only copy of its Megolm keys, and a recording is
    // reversible where a rename is not.
    const auto canonical = identityFor(QStringLiteral("@mizerd:matrix.example"));
    const QString typedSlug = QStringLiteral("Mizerd_matrix.example");
    seedStore(typedSlug, QStringLiteral("the-only-real-store"));
    m_settings->saveSession(QLatin1String(kServer),
                            QStringLiteral("@mizerd:matrix.example"),
                            QStringLiteral("DEVICE1"), QStringLiteral("token"));

    QCOMPARE(matrix::app_data::findCaseVariantStoreSlugs(canonical),
             QStringList{typedSlug});

    m_settings->setStoreSlugFor(QStringLiteral("@mizerd:matrix.example"),
                                typedSlug);

    matrix::app_data::AccountIdentity bound;
    QVERIFY(m_settings->resolveSavedIdentity(
        QStringLiteral("@mizerd:matrix.example"), &bound));
    QCOMPARE(bound.storeSlug, typedSlug);
    QCOMPARE(bound.slug, canonical.slug);          // identity unchanged
    QVERIFY(bound.isValid());
    QVERIFY(QFileInfo(bound.rustStorePath).isDir());
    QCOMPARE(markerIn(typedSlug), QStringLiteral("the-only-real-store"));

    // Nothing moved, nothing created at the canonical path.
    QVERIFY(!storeExists(canonical.slug));

    // Recording the same slug again changes nothing.
    m_settings->setStoreSlugFor(QStringLiteral("@mizerd:matrix.example"),
                                typedSlug);
    matrix::app_data::AccountIdentity again;
    QVERIFY(m_settings->resolveSavedIdentity(
        QStringLiteral("@mizerd:matrix.example"), &again));
    QCOMPARE(again.rustStorePath, bound.rustStorePath);
    QCOMPARE(markerIn(typedSlug), QStringLiteral("the-only-real-store"));
}

void SessionStoreIdentityTest::recordingSurvivesRestartAndIsReversible()
{
    const QString typedSlug = QStringLiteral("Mizerd_matrix.example");
    seedStore(typedSlug, QStringLiteral("keys"));
    m_settings->saveSession(QLatin1String(kServer),
                            QStringLiteral("@mizerd:matrix.example"),
                            QStringLiteral("DEVICE1"), QStringLiteral("token"));
    m_settings->setStoreSlugFor(QStringLiteral("@mizerd:matrix.example"),
                                typedSlug);

    // "Restart".
    m_settings.reset();
    m_settings = std::make_unique<SettingsManager>();
    m_settings->setSecretStore(m_secrets.get());

    matrix::app_data::AccountIdentity bound;
    QVERIFY(m_settings->resolveSavedIdentity(
        QStringLiteral("@mizerd:matrix.example"), &bound));
    QCOMPARE(bound.storeSlug, typedSlug);
    QVERIFY(QFileInfo(bound.rustStorePath).isDir());

    // When the SDK rejects an adopted store, clearing the recording returns
    // the account to the canonical layout without touching any store.
    m_settings->setStoreSlugFor(QStringLiteral("@mizerd:matrix.example"),
                                QString{});
    matrix::app_data::AccountIdentity reverted;
    QVERIFY(m_settings->resolveSavedIdentity(
        QStringLiteral("@mizerd:matrix.example"), &reverted));
    QCOMPARE(reverted.effectiveStoreSlug(), reverted.slug);
    QCOMPARE(markerIn(typedSlug), QStringLiteral("keys"));
}

void SessionStoreIdentityTest::ambiguousOwnershipRefusesAdoption()
{
    // Two stores whose slugs both differ from the canonical one only by case:
    // adopting either would be a guess that could hand over another account's
    // crypto store.
    const auto canonical = identityFor(QStringLiteral("@mizerd:matrix.example"));
    seedStore(QStringLiteral("Mizerd_matrix.example"), QStringLiteral("one"));
    seedStore(QStringLiteral("MIZERD_matrix.example"), QStringLiteral("two"));

    QCOMPARE(matrix::app_data::findCaseVariantStoreSlugs(canonical).size(), 2);

    // Nothing was destroyed or claimed.
    QCOMPARE(markerIn(QStringLiteral("Mizerd_matrix.example")),
             QStringLiteral("one"));
    QCOMPARE(markerIn(QStringLiteral("MIZERD_matrix.example")),
             QStringLiteral("two"));
    QVERIFY(!storeExists(canonical.slug));
}

void SessionStoreIdentityTest::bindingRefusesUnsafeOrUnscopedSlugs()
{
    auto canonical = identityFor(QStringLiteral("@mizerd:matrix.example"));
    const QString original = canonical.rustStorePath;

    for (const QString &bad : {QStringLiteral(".."),
                               QStringLiteral("."),
                               QStringLiteral("../escape"),
                               QStringLiteral("sub/dir"),
                               QStringLiteral("back\\slash")}) {
        auto probe = canonical;
        QVERIFY2(!matrix::app_data::bindStoreSlug(&probe, bad),
                 qPrintable(bad));
        // Refused, not half-applied.
        QCOMPARE(probe.rustStorePath, original);
    }

    // An empty slug means "drop the recording" and returns to canonical.
    auto probe = canonical;
    QVERIFY(matrix::app_data::bindStoreSlug(&probe, QStringLiteral("Other_x")));
    QVERIFY(probe.rustStorePath != original);
    QVERIFY(matrix::app_data::bindStoreSlug(&probe, QString{}));
    QCOMPARE(probe.rustStorePath, original);
    QCOMPARE(probe.effectiveStoreSlug(), canonical.slug);
}

void SessionStoreIdentityTest::adoptionLeavesOtherAccountsUntouched()
{
    const auto canonical = identityFor(QStringLiteral("@mizerd:matrix.example"));
    const auto other = identityFor(QStringLiteral("@someone:matrix.example"));
    seedStore(QStringLiteral("Mizerd_matrix.example"), QStringLiteral("mine"));
    seedStore(other.slug, QStringLiteral("not-mine"));

    // A different account is not a case variant, so it is never a candidate.
    QCOMPARE(matrix::app_data::findCaseVariantStoreSlugs(canonical),
             QStringList{QStringLiteral("Mizerd_matrix.example")});
    QCOMPARE(markerIn(other.slug), QStringLiteral("not-mine"));
}

void SessionStoreIdentityTest::delegatedHomeserverSlugIsReconstructedExactly()
{
    // .well-known delegation: https://matrix.example.com serves
    // @alice:example.com. Older builds paired the typed bare localpart with the
    // URL host (alice_matrix.example.com) while the record said
    // alice_example.com; the case scan cannot see that.
    matrix::app_data::AccountIdentity delegated;
    QVERIFY(matrix::app_data::resolveAccountIdentity(
        QStringLiteral("https://matrix.example.com"),
        QStringLiteral("@alice:example.com"), &delegated));
    QCOMPARE(delegated.slug, QStringLiteral("alice_example.com"));
    QCOMPARE(matrix::app_data::delegatedHomeserverStoreSlug(delegated),
             QStringLiteral("alice_matrix.example.com"));

    // The old computation: resolving the bare localpart against the same URL.
    const auto legacy = identityFor(QStringLiteral("alice"),
                                    QStringLiteral("https://matrix.example.com"));
    QCOMPARE(legacy.slug, QStringLiteral("alice_matrix.example.com"));

    // No delegation: nothing to reconstruct.
    const auto plain = identityFor(QStringLiteral("@alice:matrix.example"));
    QCOMPARE(matrix::app_data::delegatedHomeserverStoreSlug(plain), QString());
}

void SessionStoreIdentityTest::delegationDivergenceIsAdoptableAndCaseScanIsNot()
{
    matrix::app_data::AccountIdentity delegated;
    QVERIFY(matrix::app_data::resolveAccountIdentity(
        QStringLiteral("https://matrix.example.com"),
        QStringLiteral("@alice:example.com"), &delegated));
    seedStore(QStringLiteral("alice_matrix.example.com"),
              QStringLiteral("delegated-store"));

    // The case scan is blind to it; the reconstruction fills that gap.
    QVERIFY(matrix::app_data::findCaseVariantStoreSlugs(delegated).isEmpty());

    // Recording the reconstructed slug binds the real store; nothing moves.
    m_settings->saveSession(QStringLiteral("https://matrix.example.com"),
                            QStringLiteral("@alice:example.com"),
                            QStringLiteral("D1"), QStringLiteral("t1"));
    m_settings->setStoreSlugFor(QStringLiteral("@alice:example.com"),
                                QStringLiteral("alice_matrix.example.com"));

    matrix::app_data::AccountIdentity bound;
    QVERIFY(m_settings->resolveSavedIdentity(
        QStringLiteral("@alice:example.com"), &bound));
    QCOMPARE(bound.slug, QStringLiteral("alice_example.com"));
    QCOMPARE(bound.storeSlug, QStringLiteral("alice_matrix.example.com"));
    QVERIFY(bound.isValid());
    QVERIFY(QFileInfo(bound.rustStorePath).isDir());
    QCOMPARE(markerIn(QStringLiteral("alice_matrix.example.com")),
             QStringLiteral("delegated-store"));
    QCOMPARE(matrix::rust_session::restoreBlockReason(
                 bound, true, QStringLiteral("D1")),
             matrix::rust_session::StoreBlockReason::None);
}

void SessionStoreIdentityTest::savedSessionWithoutStoreEndsInASignInableState()
{
    // The repair path when the store is gone: restore reports
    // saved_session_without_store (sign in again as a new device), and signing
    // in must not write the store under the typed slug and the record under
    // the canonical one, which would fail the same way on the next start.
    using R = matrix::rust_session::StoreBlockReason;
    const QString typed = QStringLiteral("@Mizerd:matrix.example");
    const QString canonicalId = QStringLiteral("@mizerd:matrix.example");

    // Saved record + token, a device id, and no store anywhere.
    m_settings->saveSession(QLatin1String(kServer), canonicalId,
                            QStringLiteral("DCRVACHEGL"), QStringLiteral("token"));
    matrix::app_data::AccountIdentity saved;
    QVERIFY(m_settings->resolveSavedIdentity(canonicalId, &saved));
    QVERIFY(!QFileInfo(saved.rustStorePath).exists());
    QVERIFY(matrix::app_data::findCaseVariantStoreSlugs(saved).isEmpty());

    // 1. The verdict is specific and does not offer to delete a missing store.
    const auto block = matrix::rust_session::restoreBlockReason(
        saved, false, QStringLiteral("DCRVACHEGL"));
    QCOMPARE(block, R::MissingStoreForSavedSession);
    QCOMPARE(matrix::rust_session::diagnosticName(block),
             QStringLiteral("saved_session_without_store"));
    QVERIFY(!matrix::rust_session::suggestsLocalReset(block));
    QVERIFY(!matrix::rust_session::userMessage(block)
                 .contains(QStringLiteral("different Matrix session or device")));

    // 2. Signing in with the typed casing canonicalizes onto the saved
    //    account.
    bool ambiguous = true;
    const QString resolved =
        m_settings->canonicalUserIdForTypedIdentity(typed, &ambiguous);
    QVERIFY(!ambiguous);
    QCOMPARE(resolved, canonicalId);

    // 3. A fresh password login is not blocked: no store, no ownership
    //    conflict.
    auto loginIdentity = identityFor(resolved);
    QCOMPARE(matrix::rust_session::passwordLoginBlockReason(
                 loginIdentity, false, true, QStringLiteral("DCRVACHEGL")),
             R::None);

    // 4. The login opens a store and the server answers with the canonical
    //    id; the directory actually opened is what gets recorded (here, one
    //    that diverges from the record).
    const QString openedSlug = QStringLiteral("Mizerd_matrix.example");
    seedStore(openedSlug, QStringLiteral("new-device-store"));
    m_settings->saveSession(QLatin1String(kServer), canonicalId,
                            QStringLiteral("NEWDEVICE"), QStringLiteral("token2"));
    m_settings->setStoreSlugFor(canonicalId, openedSlug);

    // 5. Restart: the account resolves to the store that exists, so restore
    //    is not blocked.
    m_settings.reset();
    m_settings = std::make_unique<SettingsManager>();
    m_settings->setSecretStore(m_secrets.get());

    matrix::app_data::AccountIdentity afterRestart;
    QVERIFY(m_settings->resolveSavedIdentity(canonicalId, &afterRestart));
    QVERIFY(QFileInfo(afterRestart.rustStorePath).isDir());
    QCOMPARE(markerIn(afterRestart.effectiveStoreSlug()),
             QStringLiteral("new-device-store"));
    QCOMPARE(matrix::rust_session::restoreBlockReason(
                 afterRestart, true, m_settings->deviceId()),
             R::None);
    QCOMPARE(m_settings->deviceId(), QStringLiteral("NEWDEVICE"));

    // 6. Still true on the next restart: stable, not a one-shot repair.
    m_settings.reset();
    m_settings = std::make_unique<SettingsManager>();
    m_settings->setSecretStore(m_secrets.get());
    matrix::app_data::AccountIdentity third;
    QVERIFY(m_settings->resolveSavedIdentity(canonicalId, &third));
    QCOMPARE(third.rustStorePath, afterRestart.rustStorePath);
    QCOMPARE(matrix::rust_session::restoreBlockReason(
                 third, QFileInfo(third.rustStorePath).isDir(),
                 m_settings->deviceId()),
             R::None);

    // 7. Sign-out deletes the store really in use.
    const auto removed = matrix::app_data::removeAccountRustState(third);
    QVERIFY(removed.ok());
    QVERIFY(removed.removedAnything());
    QVERIFY(!storeExists(openedSlug));
}

// --- login() orphan-cleanup safety ----------------------------------------

void SessionStoreIdentityTest::delegatedStoreIsOwnedAndSurvivesLoginOrphanCleanup()
{
    // Driven through login()'s decision inputs (RustSdkMatrixClient needs the
    // FFI). https://matrix.example.com serves @alice:example.com and the user
    // types "alice": the derived @alice:matrix.example.com has no record, but
    // its directory is the account's real crypto store and must not be
    // deleted.
    m_settings->saveSession(QStringLiteral("https://matrix.example.com"),
                            QStringLiteral("@alice:example.com"),
                            QStringLiteral("D1"), QStringLiteral("t1"));
    seedStore(QStringLiteral("alice_matrix.example.com"),
              QStringLiteral("real-keys"));

    // What login() derives from the typed input.
    const auto typed = identityFor(QStringLiteral("alice"),
                                   QStringLiteral("https://matrix.example.com"));
    QCOMPARE(typed.userId, QStringLiteral("@alice:matrix.example.com"));
    QCOMPARE(typed.effectiveStoreSlug(),
             QStringLiteral("alice_matrix.example.com"));

    // Every older signal says "unclaimed".
    QVERIFY(!m_settings->hasSavedAccount(typed.userId));
    QCOMPARE(m_settings->canonicalUserIdForTypedIdentity(typed.userId),
             QString());
    QVERIFY(matrix::app_data::findCaseVariantStoreSlugs(typed).isEmpty());

    // The ownership check sees it.
    QCOMPARE(m_settings->accountOwningStoreSlug(typed.effectiveStoreSlug()),
             QStringLiteral("@alice:example.com"));

    // So the store is untouched: the owner has a record and a readable token,
    // which is the "switch to that account" state, not an orphan.
    QVERIFY(m_settings->hasSavedAccount(QStringLiteral("@alice:example.com")));
    QVERIFY(!m_settings->accessTokenFor(QStringLiteral("@alice:example.com"))
                 .isEmpty());
    QCOMPARE(markerIn(QStringLiteral("alice_matrix.example.com")),
             QStringLiteral("real-keys"));
    QVERIFY(storeExists(QStringLiteral("alice_matrix.example.com")));
}

void SessionStoreIdentityTest::ownershipCheckCoversAllThreeBindings()
{
    // An account can be bound to a directory three ways; missing any one makes
    // a real store look unclaimed.
    m_settings->saveSession(QLatin1String(kServer),
                            QStringLiteral("@canonical:matrix.example"),
                            QStringLiteral("D1"), QStringLiteral("t1"));
    m_settings->saveSession(QLatin1String(kServer),
                            QStringLiteral("@recorded:matrix.example"),
                            QStringLiteral("D2"), QStringLiteral("t2"));
    m_settings->setStoreSlugFor(QStringLiteral("@recorded:matrix.example"),
                                QStringLiteral("Recorded_matrix.example"));
    m_settings->saveSession(QStringLiteral("https://matrix.example.com"),
                            QStringLiteral("@delegated:example.com"),
                            QStringLiteral("D3"), QStringLiteral("t3"));

    QCOMPARE(m_settings->accountOwningStoreSlug(
                 QStringLiteral("canonical_matrix.example")),
             QStringLiteral("@canonical:matrix.example"));
    QCOMPARE(m_settings->accountOwningStoreSlug(
                 QStringLiteral("Recorded_matrix.example")),
             QStringLiteral("@recorded:matrix.example"));
    QCOMPARE(m_settings->accountOwningStoreSlug(
                 QStringLiteral("delegated_matrix.example.com")),
             QStringLiteral("@delegated:example.com"));

    // An unbound directory stays unowned, so something is still cleanable.
    QCOMPARE(m_settings->accountOwningStoreSlug(
                 QStringLiteral("stranger_matrix.example")),
             QString());
    QCOMPARE(m_settings->accountOwningStoreSlug(QString{}), QString());
}

void SessionStoreIdentityTest::unclaimedStoreIsQuarantinedNotDeleted()
{
    // A genuinely unclaimed store is moved aside, not destroyed, so a wrong
    // verdict stays recoverable.
    const auto stray = identityFor(QStringLiteral("@stray:matrix.example"));
    seedStore(stray.slug, QStringLiteral("might-matter"));
    QCOMPARE(m_settings->accountOwningStoreSlug(stray.slug), QString());

    const QString moved = matrix::app_data::quarantineRustStore(stray);
    QVERIFY(!moved.isEmpty());
    QVERIFY(!QFileInfo(stray.rustStorePath).exists());   // path is free again
    QVERIFY(QFileInfo(moved).isDir());
    QVERIFY(moved.startsWith(stray.rustStorePath + QLatin1String(".orphaned-")));

    // The bytes survived.
    QFile f(moved + QLatin1String("/marker"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(f.readAll()), QStringLiteral("might-matter"));
    f.close();

    // Nothing to move a second time, and no store is invented.
    QCOMPARE(matrix::app_data::quarantineRustStore(stray), QString());
}

void SessionStoreIdentityTest::asciiOnlyCaseFoldingForAdoptionCandidates()
{
    // Adoption recognises only a-z/A-Z divergence; full Unicode folding would
    // equate slugs from genuinely distinct localparts.
    const auto canonical = identityFor(QStringLiteral("@mizerd:matrix.example"));
    seedStore(QStringLiteral("Mizerd_matrix.example"), QStringLiteral("ascii"));
    QCOMPARE(matrix::app_data::findCaseVariantStoreSlugs(canonical),
             QStringList{QStringLiteral("Mizerd_matrix.example")});

    // Turkish dotless i folds to "i" under Unicode rules but is a different
    // localpart.
    const auto turkish = identityFor(QStringLiteral("@ismail:matrix.example"));
    seedStore(QString::fromUtf8("\xc4\xb1smail_matrix.example"),
              QStringLiteral("different-person"));
    QVERIFY(matrix::app_data::findCaseVariantStoreSlugs(turkish).isEmpty());
}

void SessionStoreIdentityTest::repairQuarantinesTheStoreInsteadOfDeletingIt()
{
    // "Quarantine and rebuild" quarantines rather than deletes: several reason
    // codes route to it where the store is believed to belong to someone
    // else, and that belief can be wrong.
    const auto identity = identityFor(QStringLiteral("@mizerd:matrix.example"));
    seedStore(identity.slug, QStringLiteral("possibly-the-only-copy"));

    const auto files = matrix::app_data::quarantineAccountRustState(identity);
    QVERIFY(files.ok());
    QVERIFY(files.removedAnything());          // still real work, not a no-op
    QVERIFY(!storeExists(identity.slug));      // out of service

    // ...but recoverable: the quarantined copy exists with its bytes.
    QDir account(identity.accountRoot);
    const auto kept = account.entryList(
        {QStringLiteral("matrix-rust-sdk-store.orphaned-*")},
        QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(kept.size(), 1);
    QFile f(identity.accountRoot + QLatin1Char('/') + kept.first()
            + QLatin1String("/marker"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(f.readAll()),
             QStringLiteral("possibly-the-only-copy"));
    f.close();

    // A repair with nothing to act on is still an honest no-op.
    const auto again = matrix::app_data::quarantineAccountRustState(identity);
    QVERIFY(again.ok());
    QVERIFY(!again.removedAnything());

    // Explicit sign-out still deletes, including quarantined copies: the user
    // asked for the account to be gone, and a quarantine is a complete crypto
    // store. This also bounds the copies.
    seedStore(identity.slug, QStringLiteral("signing-out"));
    const auto removed = matrix::app_data::removeAccountRustState(identity);
    QVERIFY(removed.removedAnything());
    QVERIFY(!storeExists(identity.slug));
    QCOMPARE(QDir(identity.accountRoot)
                 .entryList({QStringLiteral("matrix-rust-sdk-store.orphaned-*")},
                            QDir::Dirs | QDir::NoDotAndDotDot)
                 .size(),
             0);   // sign-out takes the quarantines with it
}

void SessionStoreIdentityTest::unreadableSecretBackendIsNeverADestructiveVerdict()
{
    // A locked keyring makes token lookups empty while record and store are
    // intact, so it must not route to a destructive repair.
    using R = matrix::rust_session::StoreBlockReason;
    QVERIFY(!matrix::rust_session::suggestsLocalReset(R::SecretBackendUnavailable));
    QCOMPARE(matrix::rust_session::diagnosticName(R::SecretBackendUnavailable),
             QStringLiteral("secret_backend_unavailable"));

    const QString message =
        matrix::rust_session::userMessage(R::SecretBackendUnavailable);
    QVERIFY(!message.isEmpty());
    QVERIFY(!message.contains(QStringLiteral("different Matrix session or device")));
    // The message says the data is safe.
    QVERIFY(message.contains(QStringLiteral("Nothing has been deleted")));

    // With no secret store wired the backend cannot answer, which login()
    // checks before MissingSessionMetadata (destructive) may claim the case.
    SettingsManager bare;
    QVERIFY(bare.secretBackendUnavailable());
    QVERIFY(!m_settings->secretBackendUnavailable());   // fake store answers
}

void SessionStoreIdentityTest::
    anUnreadableSecretBlocksTheLoginThatWouldDeleteTheStore()
{
    using matrix::rust_session::unreadableSecretBlocksLogin;

    // unreadableSecretBlocksLogin guards a repair that deletes a crypto store:
    // a record + a store + a token we could not read is not evidence of an
    // orphan. Destructive cleanup keys on the record being absent, never on an
    // unreadable secret.

    // Both "could not ask" routes block: a keyring that locked after startup,
    // and a fallback store standing in for a native backend that would not
    // open.
    QVERIFY2(unreadableSecretBlocksLogin(true, true, false, true, false),
             "a locked keyring let a store-deleting repair through");
    QVERIFY2(unreadableSecretBlocksLogin(true, true, false, false, true),
             "an inconclusive fallback store let a store-deleting repair "
             "through — this is the one the 2026-09-10 correction exposed");
    QVERIFY(unreadableSecretBlocksLogin(true, true, false, true, true));

    // A token we did read answers the question, so ordinary sign-ins are not
    // blocked.
    QVERIFY(!unreadableSecretBlocksLogin(true, true, true, true, true));

    // Nothing is blocked when nothing is at risk: no store, or no record.
    QVERIFY(!unreadableSecretBlocksLogin(false, true, false, true, true));
    QVERIFY(!unreadableSecretBlocksLogin(true, false, false, true, true));

    // A readable-but-absent secret with a record is an ordinary expired
    // sign-in and stays reachable.
    QVERIFY(!unreadableSecretBlocksLogin(true, true, false, false, false));
}

void SessionStoreIdentityTest::codeKeyedResetPolicyMatchesTheEnum()
{
    // "No destructive action for a reason a reset cannot repair" is enforced
    // in C++ from the reason code, not by a QML label.
    using R = matrix::rust_session::StoreBlockReason;
    for (R r : {R::None, R::MissingSessionMetadata, R::MissingDeviceId,
                R::DifferentAccount, R::ExistingStoreNeedsRestore,
                R::MissingStoreForSavedSession, R::AccessTokenRevoked,
                R::AmbiguousStoreCandidates, R::SecretBackendUnavailable,
                R::InvalidSavedIdentity}) {
        QVERIFY2(matrix::rust_session::suggestsLocalResetForCode(
                     matrix::rust_session::diagnosticName(r))
                     == matrix::rust_session::suggestsLocalReset(r),
                 qPrintable(matrix::rust_session::diagnosticName(r)));
    }

    // Codes emitted outside the enum.
    QVERIFY(matrix::rust_session::suggestsLocalResetForCode(
        QStringLiteral("cleanup_incomplete")));
    QVERIFY(matrix::rust_session::suggestsLocalResetForCode(
        QStringLiteral("sdk_store_ownership_mismatch")));

    // Unknown and empty codes default to not destructive.
    QVERIFY(!matrix::rust_session::suggestsLocalResetForCode(QString{}));
    QVERIFY(!matrix::rust_session::suggestsLocalResetForCode(
        QStringLiteral("something_a_future_build_emits")));
}

// --- reset honesty ---------------------------------------------------------

void SessionStoreIdentityTest::resetOfUnknownAccountReportsNoMatch()
{
    m_settings->saveSession(QLatin1String(kServer),
                            QStringLiteral("@mizerd:matrix.example"),
                            QStringLiteral("DEVICE1"),
                            QStringLiteral("token"));

    // Resetting an unknown account reports no match: SecretStore clears are
    // successful no-ops, so success alone would claim a reset that did
    // nothing.
    bool matched = true;
    const bool ok = m_settings->clearSessionForAccount(
        QStringLiteral("@nobody:matrix.example"), &matched);
    QVERIFY(!matched);
    Q_UNUSED(ok);

    QVERIFY(m_settings->hasSavedAccount(QStringLiteral("@mizerd:matrix.example")));
    QCOMPARE(m_settings->activeAccountUserId(),
             QStringLiteral("@mizerd:matrix.example"));
    QVERIFY(m_secrets->hasSecret(QStringLiteral("@mizerd:matrix.example"),
                                 QStringLiteral("accessToken")));
}

void SessionStoreIdentityTest::resetOfCaseVariantMatchesTheSavedRecord()
{
    m_settings->saveSession(QLatin1String(kServer),
                            QStringLiteral("@mizerd:matrix.example"),
                            QStringLiteral("DEVICE1"),
                            QStringLiteral("token"));

    // Typed with the wrong casing, the reset still finds the account that
    // failed.
    bool matched = false;
    QVERIFY(m_settings->clearSessionForAccount(
        QStringLiteral("@Mizerd:matrix.example"), &matched));
    QVERIFY(matched);
    QVERIFY(!m_settings->hasSavedAccount(QStringLiteral("@mizerd:matrix.example")));
    QVERIFY(!m_secrets->hasSecret(QStringLiteral("@mizerd:matrix.example"),
                                  QStringLiteral("accessToken")));
    QCOMPARE(m_settings->activeAccountUserId(), QString());
}

void SessionStoreIdentityTest::removalSummaryDistinguishesMissingFromDeleted()
{
    // ok() is true for an idempotent no-op; removedAnything() is the signal
    // that a reset did something.
    const auto canonical = identityFor(QStringLiteral("@mizerd:matrix.example"));
    const auto nothing = matrix::app_data::removeAccountRustState(canonical);
    QVERIFY(nothing.ok());
    QVERIFY(!nothing.removedAnything());

    seedStore(canonical.slug, QStringLiteral("x"));
    const auto real = matrix::app_data::removeAccountRustState(canonical);
    QVERIFY(real.ok());
    QVERIFY(real.removedAnything());
    QVERIFY(!storeExists(canonical.slug));
}

// --- classification --------------------------------------------------------

void SessionStoreIdentityTest::everyBlockReasonHasItsOwnCode()
{
    using R = matrix::rust_session::StoreBlockReason;
    const QList<R> all = {R::None, R::MissingSessionMetadata, R::MissingDeviceId,
                          R::DifferentAccount, R::ExistingStoreNeedsRestore,
                          R::MissingStoreForSavedSession, R::AccessTokenRevoked,
                          R::AmbiguousStoreCandidates, R::InvalidSavedIdentity,
                          R::SecretBackendUnavailable};
    QSet<QString> codes;
    for (R r : all) {
        const QString code = matrix::rust_session::diagnosticName(r);
        QVERIFY2(!code.isEmpty(), qPrintable(code));
        QVERIFY2(code != QLatin1String("unknown"), qPrintable(code));
        QVERIFY2(!codes.contains(code), qPrintable(code));
        codes.insert(code);
    }
    QCOMPARE(codes.size(), all.size());
    // The tokens AppController and the logs use must not drift.
    QCOMPARE(matrix::rust_session::diagnosticName(R::MissingStoreForSavedSession),
             QStringLiteral("saved_session_without_store"));
    QCOMPARE(matrix::rust_session::diagnosticName(R::AccessTokenRevoked),
             QStringLiteral("access_token_revoked"));
    QCOMPARE(matrix::rust_session::diagnosticName(R::AmbiguousStoreCandidates),
             QStringLiteral("ambiguous_store_candidates"));
}

void SessionStoreIdentityTest::missingStoreIsNotReportedAsAForeignStore()
{
    using R = matrix::rust_session::StoreBlockReason;
    const QString foreign =
        QStringLiteral("belongs to a different Matrix session or device");

    // Only a real SDK ownership mismatch may use this message.
    QVERIFY(matrix::rust_session::userMessage(R::DifferentAccount)
                .contains(foreign));
    for (R r : {R::MissingStoreForSavedSession, R::AccessTokenRevoked,
                R::AmbiguousStoreCandidates, R::ExistingStoreNeedsRestore,
                R::MissingSessionMetadata, R::InvalidSavedIdentity,
                R::SecretBackendUnavailable}) {
        const QString message = matrix::rust_session::userMessage(r);
        QVERIFY2(!message.isEmpty(),
                 qPrintable(matrix::rust_session::diagnosticName(r)));
        QVERIFY2(!message.contains(foreign),
                 qPrintable(matrix::rust_session::diagnosticName(r)));
    }
    QCOMPARE(matrix::rust_session::userMessage(R::None), QString());

    // Distinct conditions read differently.
    QSet<QString> seen;
    for (R r : {R::MissingStoreForSavedSession, R::AccessTokenRevoked,
                R::AmbiguousStoreCandidates, R::ExistingStoreNeedsRestore,
                R::MissingSessionMetadata, R::MissingDeviceId,
                R::DifferentAccount, R::InvalidSavedIdentity,
                R::SecretBackendUnavailable}) {
        seen.insert(matrix::rust_session::userMessage(r));
    }
    QCOMPARE(seen.size(), 9);
}

void SessionStoreIdentityTest::onlyRepairableReasonsOfferALocalReset()
{
    using R = matrix::rust_session::StoreBlockReason;
    // Deleting local data cannot conjure a missing store, renew a revoked
    // token, or settle contested ownership, and would destroy the only copy of
    // the room keys.
    QVERIFY(!matrix::rust_session::suggestsLocalReset(R::MissingStoreForSavedSession));
    QVERIFY(!matrix::rust_session::suggestsLocalReset(R::AccessTokenRevoked));
    QVERIFY(!matrix::rust_session::suggestsLocalReset(R::AmbiguousStoreCandidates));
    QVERIFY(!matrix::rust_session::suggestsLocalReset(R::ExistingStoreNeedsRestore));
    QVERIFY(!matrix::rust_session::suggestsLocalReset(R::None));

    QVERIFY(matrix::rust_session::suggestsLocalReset(R::DifferentAccount));
    QVERIFY(matrix::rust_session::suggestsLocalReset(R::MissingDeviceId));
    QVERIFY(matrix::rust_session::suggestsLocalReset(R::MissingSessionMetadata));
    // A corrupt saved record is repairable by clearing it, with a message that
    // says so.
    QVERIFY(matrix::rust_session::suggestsLocalReset(R::InvalidSavedIdentity));
    QCOMPARE(matrix::rust_session::diagnosticName(R::InvalidSavedIdentity),
             QStringLiteral("invalid_saved_account_identity"));
}

QTEST_MAIN(SessionStoreIdentityTest)
#include "SessionStoreIdentityTest.moc"
