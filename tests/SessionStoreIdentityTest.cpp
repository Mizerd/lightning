// Regression suite for the account-identity ↔ SDK-store binding.
//
// The defect this exists to prevent: Matrix localparts are case-sensitive, so
// resolveAccountIdentity() preserves whatever the user typed, while the
// homeserver answers a login with ITS canonical user id — and that is what
// gets persisted. Signing in as "Mizerd" therefore created the SDK store
// under `Mizerd_<server>/` and the account record under `mizerd_<server>/`.
// Every later restore re-derived the store path from the saved (canonical)
// record, found nothing there, and dead-ended the user on a reset prompt
// whose button targeted the typed text rather than the saved account — so the
// reset deleted the wrong slug, reported success, and the next start failed
// exactly the same way.
//
// The invariant nothing in the suite asserted before: THE STORE PATH USED TO
// CREATE A SESSION MUST EQUAL THE STORE PATH USED TO RESTORE IT.

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
    // Mirrors the real backends: clearing an account that has no secrets is a
    // successful no-op, which is precisely why "did the reset do anything?"
    // cannot be inferred from this return value alone.
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

    // login() orphan-cleanup safety (C1).
    void delegatedStoreIsOwnedAndSurvivesLoginOrphanCleanup();
    void ownershipCheckCoversAllThreeBindings();
    void unclaimedStoreIsQuarantinedNotDeleted();
    void asciiOnlyCaseFoldingForAdoptionCandidates();
    void repairQuarantinesTheStoreInsteadOfDeletingIt();
    void unreadableSecretBackendIsNeverADestructiveVerdict();
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
    // Create <primaryRoot>/<slug>/matrix-rust-sdk-store with one marker file
    // so a migration can be proven to have moved the SAME store rather than
    // silently created an empty one.
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
    // THE invariant. A login persists the server-canonical id; every later
    // start re-derives the store path from that same saved id, and it must
    // land on the store the login created.
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
    // Documents the deliberate behaviour the rest of the fix is built on:
    // localparts are NOT lowercased, because uppercase localparts are legal
    // Matrix identities and folding them would alias two real accounts onto
    // one store. The divergence is therefore repaired by canonicalizing
    // against saved records, never by mangling the id.
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
    // The reported failure, in one assertion: the user types "Mizerd", the
    // homeserver knows them as "@mizerd:…". The login must land on the saved
    // account's store instead of minting a second one.
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
    // Both casings are saved and both are real accounts. An exact hit must
    // never be redirected to its sibling.
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
    // exactly. Guessing would hand the login another account's store.
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
    // URL spellings that denote the same homeserver must not multiply the
    // account's storage slots.
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

    // An explicit port is part of the Matrix server name, so it is a
    // genuinely different identity — but it must still be stable and
    // slash-insensitive rather than producing a fresh slot per spelling.
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
    // Adoption points the account at the divergent directory; it never moves
    // it. The store holds the only copy of this account's Megolm keys, and a
    // recording is reversible where a rename is not.
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

    // Nothing was moved, nothing created at the canonical path.
    QVERIFY(!storeExists(canonical.slug));

    // Idempotent: recording the same slug again changes nothing.
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

    // The SDK is the authority on ownership: when it rejects an adopted
    // store, clearing the recording must return the account to the canonical
    // layout without touching any store.
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
    // Two on-disk stores whose slugs both differ from the canonical one only
    // by case. Adopting either would be a guess, and a wrong guess hands this
    // account someone else's crypto store.
    const auto canonical = identityFor(QStringLiteral("@mizerd:matrix.example"));
    seedStore(QStringLiteral("Mizerd_matrix.example"), QStringLiteral("one"));
    seedStore(QStringLiteral("MIZERD_matrix.example"), QStringLiteral("two"));

    QCOMPARE(matrix::app_data::findCaseVariantStoreSlugs(canonical).size(), 2);

    // Nothing was destroyed or claimed while establishing that.
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
    // .well-known delegation: https://matrix.example.com serves @alice:example.com.
    // An older build paired the typed bare localpart with the URL host, so the
    // store went to alice_matrix.example.com while the record said
    // alice_example.com. No casing is involved, which is why the case scan
    // cannot see it.
    matrix::app_data::AccountIdentity delegated;
    QVERIFY(matrix::app_data::resolveAccountIdentity(
        QStringLiteral("https://matrix.example.com"),
        QStringLiteral("@alice:example.com"), &delegated));
    QCOMPARE(delegated.slug, QStringLiteral("alice_example.com"));
    QCOMPARE(matrix::app_data::delegatedHomeserverStoreSlug(delegated),
             QStringLiteral("alice_matrix.example.com"));

    // Exactly what the old code computed: resolving the bare localpart against
    // the same URL must produce that very slug.
    const auto legacy = identityFor(QStringLiteral("alice"),
                                    QStringLiteral("https://matrix.example.com"));
    QCOMPARE(legacy.slug, QStringLiteral("alice_matrix.example.com"));

    // No delegation in play -> nothing to reconstruct.
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

    // The case scan is blind to it — this is the gap the reconstruction fills.
    QVERIFY(matrix::app_data::findCaseVariantStoreSlugs(delegated).isEmpty());

    // Recording the reconstructed slug binds the account to the real store,
    // and nothing is moved.
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
    // THIS IS THE USER'S ACTUAL REPAIR PATH on the reporting machine. Their
    // real store was destroyed on 2026-07-29 08:04:22 by the old orphan
    // cleanup, so both account roots are empty: there is nothing to adopt,
    // and the honest verdict is saved_session_without_store → sign in again
    // as a new device. What must NOT happen is the old loop, where signing in
    // wrote the store under the typed slug and the record under the canonical
    // one, so the very next start failed identically.
    using R = matrix::rust_session::StoreBlockReason;
    const QString typed = QStringLiteral("@Mizerd:matrix.example");
    const QString canonicalId = QStringLiteral("@mizerd:matrix.example");

    // Saved record + token, non-empty device id, and NO store anywhere.
    m_settings->saveSession(QLatin1String(kServer), canonicalId,
                            QStringLiteral("DCRVACHEGL"), QStringLiteral("token"));
    matrix::app_data::AccountIdentity saved;
    QVERIFY(m_settings->resolveSavedIdentity(canonicalId, &saved));
    QVERIFY(!QFileInfo(saved.rustStorePath).exists());
    QVERIFY(matrix::app_data::findCaseVariantStoreSlugs(saved).isEmpty());

    // 1. The restore verdict is specific, and it does NOT offer to delete a
    //    store that is not there.
    const auto block = matrix::rust_session::restoreBlockReason(
        saved, false, QStringLiteral("DCRVACHEGL"));
    QCOMPARE(block, R::MissingStoreForSavedSession);
    QCOMPARE(matrix::rust_session::diagnosticName(block),
             QStringLiteral("saved_session_without_store"));
    QVERIFY(!matrix::rust_session::suggestsLocalReset(block));
    QVERIFY(!matrix::rust_session::userMessage(block)
                 .contains(QStringLiteral("different Matrix session or device")));

    // 2. The user signs in again, typing the casing they always type. The
    //    login canonicalizes onto the saved account instead of minting a
    //    second one.
    bool ambiguous = true;
    const QString resolved =
        m_settings->canonicalUserIdForTypedIdentity(typed, &ambiguous);
    QVERIFY(!ambiguous);
    QCOMPARE(resolved, canonicalId);

    // 3. A fresh password login is NOT blocked — no store exists, so there is
    //    no ownership conflict to protect against.
    auto loginIdentity = identityFor(resolved);
    QCOMPARE(matrix::rust_session::passwordLoginBlockReason(
                 loginIdentity, false, true, QStringLiteral("DCRVACHEGL")),
             R::None);

    // 4. The login opens a store and the server answers with the canonical
    //    id. Whatever directory was really opened is what gets recorded.
    //    (Simulating the worst case: the directory diverges from the record.)
    const QString openedSlug = QStringLiteral("Mizerd_matrix.example");
    seedStore(openedSlug, QStringLiteral("new-device-store"));
    m_settings->saveSession(QLatin1String(kServer), canonicalId,
                            QStringLiteral("NEWDEVICE"), QStringLiteral("token2"));
    m_settings->setStoreSlugFor(canonicalId, openedSlug);

    // 5. Restart. The account resolves to the store that actually exists, so
    //    restore is no longer blocked. This is the assertion that proves the
    //    loop is broken.
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

    // 6. Still true on the restart after that — the state is stable, not a
    //    one-shot repair.
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

    // 7. And a sign-out now deletes the store that was really in use rather
    //    than reporting success over an untouched directory.
    const auto removed = matrix::app_data::removeAccountRustState(third);
    QVERIFY(removed.ok());
    QVERIFY(removed.removedAnything());
    QVERIFY(!storeExists(openedSlug));
}

// --- login() orphan-cleanup safety ----------------------------------------

void SessionStoreIdentityTest::delegatedStoreIsOwnedAndSurvivesLoginOrphanCleanup()
{
    // The critical regression, driven through login()'s DECISION INPUTS.
    // (RustSdkMatrixClient itself needs the Rust FFI and cannot be built in a
    // pure unit test, so this exercises the three values login() branches on
    // rather than the method; the branch is a direct function of them.)
    //
    // Homeserver https://matrix.example.com serves @alice:example.com. The
    // user types a bare "alice". The old code derived @alice:matrix.example.com,
    // found no record under that slug, and recursively deleted the directory —
    // which was the account's real crypto store.
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

    // Every pre-existing signal says "unclaimed" — this is exactly why the
    // delete fired, and why none of the earlier fixes caught it.
    QVERIFY(!m_settings->hasSavedAccount(typed.userId));
    QCOMPARE(m_settings->canonicalUserIdForTypedIdentity(typed.userId),
             QString());
    QVERIFY(matrix::app_data::findCaseVariantStoreSlugs(typed).isEmpty());

    // The ownership check is the one that sees it.
    QCOMPARE(m_settings->accountOwningStoreSlug(typed.effectiveStoreSlug()),
             QStringLiteral("@alice:example.com"));

    // So the store is never touched, and the branch that would have removed
    // it is not taken: the owner has a record and a readable token, which is
    // the "switch to that account" state, not the orphan state.
    QVERIFY(m_settings->hasSavedAccount(QStringLiteral("@alice:example.com")));
    QVERIFY(!m_settings->accessTokenFor(QStringLiteral("@alice:example.com"))
                 .isEmpty());
    QCOMPARE(markerIn(QStringLiteral("alice_matrix.example.com")),
             QStringLiteral("real-keys"));
    QVERIFY(storeExists(QStringLiteral("alice_matrix.example.com")));
}

void SessionStoreIdentityTest::ownershipCheckCoversAllThreeBindings()
{
    // An account can be bound to a directory three ways. Missing any one of
    // them means a real store reads as unclaimed.
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

    // A directory nothing is bound to stays unowned — the check must not be
    // so broad that nothing is ever cleanable.
    QCOMPARE(m_settings->accountOwningStoreSlug(
                 QStringLiteral("stranger_matrix.example")),
             QString());
    QCOMPARE(m_settings->accountOwningStoreSlug(QString{}), QString());
}

void SessionStoreIdentityTest::unclaimedStoreIsQuarantinedNotDeleted()
{
    // A genuinely unclaimed store is still moved aside, not destroyed: the
    // "unclaimed" verdict has been wrong before and must stay recoverable.
    const auto stray = identityFor(QStringLiteral("@stray:matrix.example"));
    seedStore(stray.slug, QStringLiteral("might-matter"));
    QCOMPARE(m_settings->accountOwningStoreSlug(stray.slug), QString());

    const QString moved = matrix::app_data::quarantineRustStore(stray);
    QVERIFY(!moved.isEmpty());
    QVERIFY(!QFileInfo(stray.rustStorePath).exists());   // path is free again
    QVERIFY(QFileInfo(moved).isDir());
    QVERIFY(moved.startsWith(stray.rustStorePath + QLatin1String(".orphaned-")));

    // The bytes survived — this is the whole point.
    QFile f(moved + QLatin1String("/marker"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(f.readAll()), QStringLiteral("might-matter"));
    f.close();

    // Nothing to move a second time, and no store is invented.
    QCOMPARE(matrix::app_data::quarantineRustStore(stray), QString());
}

void SessionStoreIdentityTest::asciiOnlyCaseFoldingForAdoptionCandidates()
{
    // Adoption recognises the a-z/A-Z divergence the old code produced and
    // nothing else. Full Unicode folding would equate slugs built from
    // genuinely distinct Matrix localparts.
    const auto canonical = identityFor(QStringLiteral("@mizerd:matrix.example"));
    seedStore(QStringLiteral("Mizerd_matrix.example"), QStringLiteral("ascii"));
    QCOMPARE(matrix::app_data::findCaseVariantStoreSlugs(canonical),
             QStringList{QStringLiteral("Mizerd_matrix.example")});

    // Turkish dotless i folds to "i" under Unicode rules but is a different
    // localpart, so it must not be offered as this account's store.
    const auto turkish = identityFor(QStringLiteral("@ismail:matrix.example"));
    seedStore(QString::fromUtf8("\xc4\xb1smail_matrix.example"),
              QStringLiteral("different-person"));
    QVERIFY(matrix::app_data::findCaseVariantStoreSlugs(turkish).isEmpty());
}

void SessionStoreIdentityTest::repairQuarantinesTheStoreInsteadOfDeletingIt()
{
    // The repair card is captioned "Quarantine and rebuild" and four reason
    // codes route to it — including session_account_mismatch and
    // sdk_store_ownership_mismatch, where the store being acted on is BY
    // DEFINITION one the app believes belongs to someone else. That belief
    // has been wrong. The operation must match the label.
    const auto identity = identityFor(QStringLiteral("@mizerd:matrix.example"));
    seedStore(identity.slug, QStringLiteral("possibly-the-only-copy"));

    const auto files = matrix::app_data::quarantineAccountRustState(identity);
    QVERIFY(files.ok());
    QVERIFY(files.removedAnything());          // still real work, not a no-op
    QVERIFY(!storeExists(identity.slug));      // out of service

    // ...but recoverable. Find the quarantined copy and prove the bytes live.
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

    // The explicit sign-out path deliberately still DELETES: there the user
    // asked for the account to be gone, and leaving Megolm keys behind would
    // be a data-at-rest defect. The two must not be conflated.
    //
    // And it must delete the QUARANTINES TOO. A quarantined store is a
    // complete crypto store — Megolm sessions plus the device's Olm identity
    // — so a sign-out that removed only the live one would report success
    // while leaving exactly the key material the user asked to be rid of.
    // Repair keeps a copy precisely because the verdict might be wrong; a
    // sign-out is not a verdict, it is an instruction. This also bounds the
    // copies, which would otherwise accumulate one per repair forever.
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
    // A locked keyring makes every token lookup come back empty. The record
    // and the store are both intact and the sign-in may be too — we simply
    // cannot ask. Routing that to a destructive repair captioned "rebuilding
    // it is safe" destroys room keys to fix nothing.
    using R = matrix::rust_session::StoreBlockReason;
    QVERIFY(!matrix::rust_session::suggestsLocalReset(R::SecretBackendUnavailable));
    QCOMPARE(matrix::rust_session::diagnosticName(R::SecretBackendUnavailable),
             QStringLiteral("secret_backend_unavailable"));

    const QString message =
        matrix::rust_session::userMessage(R::SecretBackendUnavailable);
    QVERIFY(!message.isEmpty());
    QVERIFY(!message.contains(QStringLiteral("different Matrix session or device")));
    // It must say the data is safe, since that is the whole distinction from
    // the destructive sibling it used to be classified as.
    QVERIFY(message.contains(QStringLiteral("Nothing has been deleted")));

    // The reachable-state check: with no secret store wired, the backend
    // cannot answer, which is exactly what login() consults before it lets
    // MissingSessionMetadata (destructive) claim the case.
    SettingsManager bare;
    QVERIFY(bare.secretBackendUnavailable());
    QVERIFY(!m_settings->secretBackendUnavailable());   // fake store answers
}

void SessionStoreIdentityTest::codeKeyedResetPolicyMatchesTheEnum()
{
    // The invariant "no destructive action for a reason a reset cannot
    // repair" is enforced in C++ off the reason code, not by a QML label.
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

    // Unknown and empty codes must default to NOT destructive.
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

    // The regression: this returned true (SecretStore clears are successful
    // no-ops), so the UI announced "Local Lightning session reset. You can
    // sign in again." while the real record, token and active pointer were
    // all still there — and the next start failed identically.
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

    // Typed with the wrong casing — the reset must still find and clear the
    // account that actually failed, instead of silently matching nothing.
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
    // ok() stays true for an idempotent no-op, so it can never be the signal
    // that a reset accomplished anything. removedAnything() is.
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
    // The tokens AppController and the logs key off must not drift.
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

    // Six unrelated conditions used to share this one sentence. Only a real
    // SDK ownership mismatch may claim it.
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

    // Distinct conditions must read differently, or the split is cosmetic.
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
    // Deleting local data cannot conjure a store that is not there, cannot
    // renew a revoked token, and must never be the answer to contested
    // ownership — that is how the user's only copy of their room keys gets
    // destroyed.
    QVERIFY(!matrix::rust_session::suggestsLocalReset(R::MissingStoreForSavedSession));
    QVERIFY(!matrix::rust_session::suggestsLocalReset(R::AccessTokenRevoked));
    QVERIFY(!matrix::rust_session::suggestsLocalReset(R::AmbiguousStoreCandidates));
    QVERIFY(!matrix::rust_session::suggestsLocalReset(R::ExistingStoreNeedsRestore));
    QVERIFY(!matrix::rust_session::suggestsLocalReset(R::None));

    QVERIFY(matrix::rust_session::suggestsLocalReset(R::DifferentAccount));
    QVERIFY(matrix::rust_session::suggestsLocalReset(R::MissingDeviceId));
    QVERIFY(matrix::rust_session::suggestsLocalReset(R::MissingSessionMetadata));
    // A corrupt saved record IS repairable by clearing it — but its message
    // must describe that, not claim the store belongs to someone else.
    QVERIFY(matrix::rust_session::suggestsLocalReset(R::InvalidSavedIdentity));
    QCOMPARE(matrix::rust_session::diagnosticName(R::InvalidSavedIdentity),
             QStringLiteral("invalid_saved_account_identity"));
}

QTEST_MAIN(SessionStoreIdentityTest)
#include "SessionStoreIdentityTest.moc"
