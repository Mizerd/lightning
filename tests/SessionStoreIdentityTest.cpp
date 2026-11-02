
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

}

class SessionStoreIdentityTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();

    void sameAccountResolvesToOneStoreAcrossRestarts();
    void typedLocalpartCaseIsPreservedByResolution();
    void typedCaseVariantAdoptsTheSavedCanonicalAccount();
    void exactMatchWinsOverCaseInsensitiveSibling();
    void ambiguousSavedCasingsRefuseCanonicalization();
    void distinctUsersOnOneHomeserverStayIsolated();
    void sameLocalpartOnDifferentHomeserversStaysIsolated();
    void equivalentHomeserverUrlsShareOneSlot();

    void aBareUsernameIsSentAsTypedNeverCompletedWithTheAddress();
    void aBareUsernameFindsItsAccountFromTheRecordOnThatAddressOnly();
    void aSignInAnswerNeverTakesAnotherAccountsStore();
    void aCaseFoldedBareUsernameNeverResumesTheSavedDevice();

    void adoptionRecordsTheStoreAndIsIdempotent();
    void recordingSurvivesRestartAndIsReversible();
    void ambiguousOwnershipRefusesAdoption();
    void bindingRefusesUnsafeOrUnscopedSlugs();
    void adoptionLeavesOtherAccountsUntouched();
    void delegatedHomeserverSlugIsReconstructedExactly();
    void delegationDivergenceIsAdoptableAndCaseScanIsNot();
    void savedSessionWithoutStoreEndsInASignInableState();

    void delegatedStoreIsOwnedAndSurvivesLoginOrphanCleanup();
    void ownershipCheckCoversAllThreeBindings();
    void unclaimedStoreIsQuarantinedNotDeleted();
    void quarantineRetriesASharingViolationAndNeverSplitsTheStore();
    void quarantineNamesAStoreInUse();
    void aFailedEntryMoveIsRolledBackAndTheStoreStaysWhole();
    void asciiOnlyCaseFoldingForAdoptionCandidates();
    void repairQuarantinesTheStoreInsteadOfDeletingIt();
    void unreadableSecretBackendIsNeverADestructiveVerdict();
    void anUnreadableSecretBlocksTheLoginThatWouldDeleteTheStore();
    void codeKeyedResetPolicyMatchesTheEnum();

    void resetOfUnknownAccountReportsNoMatch();
    void resetOfCaseVariantMatchesTheSavedRecord();
    void removalSummaryDistinguishesMissingFromDeleted();

    void everyBlockReasonHasItsOwnCode();
    void missingStoreIsNotReportedAsAForeignStore();
    void onlyRepairableReasonsOfferALocalReset();

private:
    matrix::app_data::AccountIdentity identityFor(
        const QString &user, const QString &homeserver = QLatin1String(kServer)) const;
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

void SessionStoreIdentityTest::sameAccountResolvesToOneStoreAcrossRestarts()
{
    const auto atLogin = identityFor(QStringLiteral("@alice:matrix.example"));
    seedStore(atLogin.slug, QStringLiteral("real-store"));

    m_settings->saveSession(QLatin1String(kServer),
                            QStringLiteral("@alice:matrix.example"),
                            QStringLiteral("DEVICE1"),
                            QStringLiteral("token"));

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
    const auto upper = identityFor(QStringLiteral("@Mizerd:matrix.example"));
    const auto lower = identityFor(QStringLiteral("@mizerd:matrix.example"));
    QCOMPARE(upper.userId, QStringLiteral("@Mizerd:matrix.example"));
    QCOMPARE(lower.userId, QStringLiteral("@mizerd:matrix.example"));
    QVERIFY(upper.rustStorePath != lower.rustStorePath);
    QCOMPARE(identityFor(QStringLiteral("@mizerd:MATRIX.EXAMPLE")).userId,
             lower.userId);
}

void SessionStoreIdentityTest::typedCaseVariantAdoptsTheSavedCanonicalAccount()
{
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

void SessionStoreIdentityTest::aBareUsernameIsSentAsTypedNeverCompletedWithTheAddress()
{
    const QString address = QStringLiteral("http://127.0.0.1:18008");
    const auto typed = identityFor(QStringLiteral("bk010"), address);
    QCOMPARE(typed.userId, QStringLiteral("@bk010:127.0.0.1:18008"));
    QCOMPARE(matrix::rust_session::passwordLoginIdentifier(QStringLiteral("bk010"), typed),
             QStringLiteral("bk010"));
    QCOMPARE(matrix::rust_session::passwordLoginIdentifier(QStringLiteral("  bk010 "), typed),
             QStringLiteral("bk010"));

    const auto full = identityFor(QStringLiteral("@bk010:LocalHS.test"), address);
    QCOMPARE(matrix::rust_session::passwordLoginIdentifier(
                 QStringLiteral("@bk010:LocalHS.test"), full),
             QStringLiteral("@bk010:localhs.test"));
}

void SessionStoreIdentityTest::aBareUsernameFindsItsAccountFromTheRecordOnThatAddressOnly()
{
    using matrix::rust_session::savedAccountForBareUsername;
    const QString address = QStringLiteral("http://127.0.0.1:18008");
    m_settings->saveSession(address + QLatin1Char('/'), QStringLiteral("@bk010:localhs.test"),
                            QStringLiteral("D1"), QStringLiteral("t1"));
    m_settings->saveSession(QLatin1String(kServer), QStringLiteral("@bk010:matrix.example"),
                            QStringLiteral("D2"), QStringLiteral("t2"));
    QList<matrix::app_data::AccountIdentity> saved;
    for (const QString &userId : m_settings->savedAccountUserIds()) {
        matrix::app_data::AccountIdentity record;
        QVERIFY(m_settings->resolveSavedIdentity(userId, &record));
        saved.append(record);
    }
    QCOMPARE(saved.size(), 2);
    const QString onAddress = identityFor(QStringLiteral("bk010"), address).homeserver;

    bool ambiguous = true;
    QCOMPARE(savedAccountForBareUsername(QStringLiteral("bk010"), onAddress, saved, &ambiguous),
             QStringLiteral("@bk010:localhs.test"));
    QVERIFY(!ambiguous);
    QCOMPARE(savedAccountForBareUsername(QStringLiteral("bk010"),
                                         QStringLiteral("https://elsewhere.example"), saved),
             QString());
    QCOMPARE(savedAccountForBareUsername(QStringLiteral("bk011"), onAddress, saved), QString());
    QCOMPARE(savedAccountForBareUsername(QStringLiteral("@bk010:localhs.test"), onAddress,
                                         saved),
             QString());
    QCOMPARE(savedAccountForBareUsername(QStringLiteral("BK010"), onAddress, saved),
             QStringLiteral("@bk010:localhs.test"));
    matrix::app_data::AccountIdentity upper = identityFor(
        QStringLiteral("@BK010:localhs.test"), address);
    QList<matrix::app_data::AccountIdentity> both = saved;
    both.append(upper);
    QCOMPARE(savedAccountForBareUsername(QStringLiteral("BK010"), onAddress, both, &ambiguous),
             QStringLiteral("@BK010:localhs.test"));
    QVERIFY(!ambiguous);
    QCOMPARE(savedAccountForBareUsername(QStringLiteral("Bk010"), onAddress, both, &ambiguous),
             QString());
    QVERIFY(ambiguous);
    QList<matrix::app_data::AccountIdentity> twins = saved;
    twins.append(identityFor(QStringLiteral("@bk010:other.test"), address));
    QCOMPARE(savedAccountForBareUsername(QStringLiteral("bk010"), onAddress, twins, &ambiguous),
             QString());
    QVERIFY(ambiguous);
}

void SessionStoreIdentityTest::aSignInAnswerNeverTakesAnotherAccountsStore()
{
    using matrix::rust_session::signInAnswerCollidesWithSavedStore;
    const QString answer = QStringLiteral("@bk010:localhs.test");
    const QString root = matrix::app_data::primaryRoot();
    const QString provisional =
        root + QStringLiteral("/bk010_127.0.0.1_18008/matrix-rust-sdk-store");
    const QString recorded = root + QStringLiteral("/bk010_localhs.test/matrix-rust-sdk-store");

    QVERIFY(!signInAnswerCollidesWithSavedStore(answer, provisional, QString(), false,
                                                QString()));
    QVERIFY(!signInAnswerCollidesWithSavedStore(
        answer, recorded, answer, true,
        root + QStringLiteral("/bk010_localhs.test/./matrix-rust-sdk-store/")));
    QVERIFY(signInAnswerCollidesWithSavedStore(answer, provisional, QString(), true, recorded));
    QVERIFY(signInAnswerCollidesWithSavedStore(answer, recorded,
                                               QStringLiteral("@BK010:localhs.test"), false,
                                               QString()));
    QVERIFY(!signInAnswerCollidesWithSavedStore(answer, QString(), QString(), true, recorded));
}

void SessionStoreIdentityTest::aCaseFoldedBareUsernameNeverResumesTheSavedDevice()
{
    using namespace matrix::rust_session;
    const QString saved = QStringLiteral("@bk010:localhs.test");
    const QString hs = QStringLiteral("http://127.0.0.1:18008");
    const QString device = QStringLiteral("OLDDEVICE");
    QCOMPARE(typedUserIdOnSavedServer(QStringLiteral("bk010"), saved), saved);
    QCOMPARE(typedUserIdOnSavedServer(QStringLiteral(" BK010 "), saved),
             QStringLiteral("@BK010:localhs.test"));
    QVERIFY(typedUserIdOnSavedServer(saved, saved).isEmpty());
    QVERIFY(typedUserIdOnSavedServer(QStringLiteral("bk010:x"), saved).isEmpty());
    QVERIFY(typedUserIdOnSavedServer(QString(), saved).isEmpty());
    QVERIFY(typedUserIdOnSavedServer(QStringLiteral("bk010"), QStringLiteral("bk010"))
                .isEmpty());

    const QString exact = typedUserIdOnSavedServer(QStringLiteral("bk010"), saved);
    QVERIFY(softLoggedOutDeviceMayResume(exact, hs, saved, hs, device, saved, hs, device));
    QVERIFY(lostSignInDeviceMayResume(exact, hs, saved, hs, device, false));
    const QString folded = typedUserIdOnSavedServer(QStringLiteral("BK010"), saved);
    QVERIFY(!softLoggedOutDeviceMayResume(folded, hs, saved, hs, device, saved, hs, device));
    QVERIFY(!lostSignInDeviceMayResume(folded, hs, saved, hs, device, false));
    QVERIFY(!lostSignInDeviceMayResume(
        typedUserIdOnSavedServer(QStringLiteral("bk010"), QStringLiteral("bad")), hs,
        saved, hs, device, false));
}

void SessionStoreIdentityTest::exactMatchWinsOverCaseInsensitiveSibling()
{
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
    QCOMPARE(m_settings->canonicalUserIdForTypedIdentity(
                 QStringLiteral("@Alice:other.example")),
             QString());
    seedStore(there.slug, QStringLiteral("other-server"));
    QVERIFY(matrix::app_data::findCaseVariantStoreSlugs(here).isEmpty());
}

void SessionStoreIdentityTest::equivalentHomeserverUrlsShareOneSlot()
{
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

    const auto ported = identityFor(QStringLiteral("alice"),
                                    QStringLiteral("https://matrix.example:8448"));
    const auto portedSlash = identityFor(QStringLiteral("alice"),
                                         QStringLiteral("https://matrix.example:8448/"));
    QCOMPARE(ported.rustStorePath, portedSlash.rustStorePath);
    QVERIFY(ported.rustStorePath != plain.rustStorePath);
}

void SessionStoreIdentityTest::adoptionRecordsTheStoreAndIsIdempotent()
{
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
    QCOMPARE(bound.slug, canonical.slug);
    QVERIFY(bound.isValid());
    QVERIFY(QFileInfo(bound.rustStorePath).isDir());
    QCOMPARE(markerIn(typedSlug), QStringLiteral("the-only-real-store"));

    QVERIFY(!storeExists(canonical.slug));

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

    m_settings.reset();
    m_settings = std::make_unique<SettingsManager>();
    m_settings->setSecretStore(m_secrets.get());

    matrix::app_data::AccountIdentity bound;
    QVERIFY(m_settings->resolveSavedIdentity(
        QStringLiteral("@mizerd:matrix.example"), &bound));
    QCOMPARE(bound.storeSlug, typedSlug);
    QVERIFY(QFileInfo(bound.rustStorePath).isDir());

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
    const auto canonical = identityFor(QStringLiteral("@mizerd:matrix.example"));
    seedStore(QStringLiteral("Mizerd_matrix.example"), QStringLiteral("one"));
    seedStore(QStringLiteral("MIZERD_matrix.example"), QStringLiteral("two"));

    QCOMPARE(matrix::app_data::findCaseVariantStoreSlugs(canonical).size(), 2);

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
        QCOMPARE(probe.rustStorePath, original);
    }

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

    QCOMPARE(matrix::app_data::findCaseVariantStoreSlugs(canonical),
             QStringList{QStringLiteral("Mizerd_matrix.example")});
    QCOMPARE(markerIn(other.slug), QStringLiteral("not-mine"));
}

void SessionStoreIdentityTest::delegatedHomeserverSlugIsReconstructedExactly()
{
    matrix::app_data::AccountIdentity delegated;
    QVERIFY(matrix::app_data::resolveAccountIdentity(
        QStringLiteral("https://matrix.example.com"),
        QStringLiteral("@alice:example.com"), &delegated));
    QCOMPARE(delegated.slug, QStringLiteral("alice_example.com"));
    QCOMPARE(matrix::app_data::delegatedHomeserverStoreSlug(delegated),
             QStringLiteral("alice_matrix.example.com"));

    const auto legacy = identityFor(QStringLiteral("alice"),
                                    QStringLiteral("https://matrix.example.com"));
    QCOMPARE(legacy.slug, QStringLiteral("alice_matrix.example.com"));

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

    QVERIFY(matrix::app_data::findCaseVariantStoreSlugs(delegated).isEmpty());

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
    using R = matrix::rust_session::StoreBlockReason;
    const QString typed = QStringLiteral("@Mizerd:matrix.example");
    const QString canonicalId = QStringLiteral("@mizerd:matrix.example");

    m_settings->saveSession(QLatin1String(kServer), canonicalId,
                            QStringLiteral("DCRVACHEGL"), QStringLiteral("token"));
    matrix::app_data::AccountIdentity saved;
    QVERIFY(m_settings->resolveSavedIdentity(canonicalId, &saved));
    QVERIFY(!QFileInfo(saved.rustStorePath).exists());
    QVERIFY(matrix::app_data::findCaseVariantStoreSlugs(saved).isEmpty());

    const auto block = matrix::rust_session::restoreBlockReason(
        saved, false, QStringLiteral("DCRVACHEGL"));
    QCOMPARE(block, R::MissingStoreForSavedSession);
    QCOMPARE(matrix::rust_session::diagnosticName(block),
             QStringLiteral("saved_session_without_store"));
    QVERIFY(!matrix::rust_session::suggestsLocalReset(block));
    QVERIFY(!matrix::rust_session::userMessage(block)
                 .contains(QStringLiteral("different Matrix session or device")));

    bool ambiguous = true;
    const QString resolved =
        m_settings->canonicalUserIdForTypedIdentity(typed, &ambiguous);
    QVERIFY(!ambiguous);
    QCOMPARE(resolved, canonicalId);

    auto loginIdentity = identityFor(resolved);
    QCOMPARE(matrix::rust_session::passwordLoginBlockReason(
                 loginIdentity, false, true, QStringLiteral("DCRVACHEGL")),
             R::None);

    const QString openedSlug = QStringLiteral("Mizerd_matrix.example");
    seedStore(openedSlug, QStringLiteral("new-device-store"));
    m_settings->saveSession(QLatin1String(kServer), canonicalId,
                            QStringLiteral("NEWDEVICE"), QStringLiteral("token2"));
    m_settings->setStoreSlugFor(canonicalId, openedSlug);

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

    const auto removed = matrix::app_data::removeAccountRustState(third);
    QVERIFY(removed.ok());
    QVERIFY(removed.removedAnything());
    QVERIFY(!storeExists(openedSlug));
}

void SessionStoreIdentityTest::delegatedStoreIsOwnedAndSurvivesLoginOrphanCleanup()
{
    m_settings->saveSession(QStringLiteral("https://matrix.example.com"),
                            QStringLiteral("@alice:example.com"),
                            QStringLiteral("D1"), QStringLiteral("t1"));
    seedStore(QStringLiteral("alice_matrix.example.com"),
              QStringLiteral("real-keys"));

    const auto typed = identityFor(QStringLiteral("alice"),
                                   QStringLiteral("https://matrix.example.com"));
    QCOMPARE(typed.userId, QStringLiteral("@alice:matrix.example.com"));
    QCOMPARE(typed.effectiveStoreSlug(),
             QStringLiteral("alice_matrix.example.com"));

    QVERIFY(!m_settings->hasSavedAccount(typed.userId));
    QCOMPARE(m_settings->canonicalUserIdForTypedIdentity(typed.userId),
             QString());
    QVERIFY(matrix::app_data::findCaseVariantStoreSlugs(typed).isEmpty());

    QCOMPARE(m_settings->accountOwningStoreSlug(typed.effectiveStoreSlug()),
             QStringLiteral("@alice:example.com"));

    QVERIFY(m_settings->hasSavedAccount(QStringLiteral("@alice:example.com")));
    QVERIFY(!m_settings->accessTokenFor(QStringLiteral("@alice:example.com"))
                 .isEmpty());
    QCOMPARE(markerIn(QStringLiteral("alice_matrix.example.com")),
             QStringLiteral("real-keys"));
    QVERIFY(storeExists(QStringLiteral("alice_matrix.example.com")));
}

void SessionStoreIdentityTest::ownershipCheckCoversAllThreeBindings()
{
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

    QCOMPARE(m_settings->accountOwningStoreSlug(
                 QStringLiteral("stranger_matrix.example")),
             QString());
    QCOMPARE(m_settings->accountOwningStoreSlug(QString{}), QString());
}

void SessionStoreIdentityTest::unclaimedStoreIsQuarantinedNotDeleted()
{
    const auto stray = identityFor(QStringLiteral("@stray:matrix.example"));
    seedStore(stray.slug, QStringLiteral("might-matter"));
    QCOMPARE(m_settings->accountOwningStoreSlug(stray.slug), QString());

    const QString moved = matrix::app_data::quarantineRustStore(stray);
    QVERIFY(!moved.isEmpty());
    QVERIFY(!QFileInfo(stray.rustStorePath).exists());
    QVERIFY(QFileInfo(moved).isDir());
    QVERIFY(moved.startsWith(stray.rustStorePath + QLatin1String(".orphaned-")));

    QFile f(moved + QLatin1String("/marker"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(f.readAll()), QStringLiteral("might-matter"));
    f.close();

    QCOMPARE(matrix::app_data::quarantineRustStore(stray), QString());
}

namespace {
struct FlakyRename {
    int failFor = 0;
    int calls = 0;
    bool always = false;
};
bool flakyRename(const QString &from, const QString &to, void *ctx)
{
    auto *f = static_cast<FlakyRename *>(ctx);
    ++f->calls;
    if (f->always || f->calls <= f->failFor)
        return false;
    return QDir().rename(from, to);
}
}

void SessionStoreIdentityTest::quarantineRetriesASharingViolationAndNeverSplitsTheStore()
{
    const auto stray = identityFor(QStringLiteral("@busy:matrix.example"));
    seedStore(stray.slug, QStringLiteral("held-open"));
    QFile second(stray.rustStorePath + QLatin1String("/second"));
    QVERIFY(second.open(QIODevice::WriteOnly));
    second.write("2");
    second.close();

    struct Ctx { FlakyRename f; int n = 0; } ctx;
    auto rename = [](const QString &from, const QString &to, void *c) {
        auto *x = static_cast<Ctx *>(c);
        ++x->n;
        if (x->n == 1 || x->n == 3)
            return false;
        return QDir().rename(from, to);
    };
    matrix::app_data::QuarantineFailure why;
    const QString moved = matrix::app_data::quarantineRustStoreWith(
        stray, rename, &ctx, 3, 0, &why);
    QVERIFY2(!moved.isEmpty(), "a transient sharing violation was not retried");
    QVERIFY(!QFileInfo(stray.rustStorePath).exists());
    QVERIFY(QFileInfo(moved + QLatin1String("/marker")).exists());
    QVERIFY(QFileInfo(moved + QLatin1String("/second")).exists());
}

void SessionStoreIdentityTest::aFailedEntryMoveIsRolledBackAndTheStoreStaysWhole()
{
    const auto stray = identityFor(QStringLiteral("@split:matrix.example"));
    seedStore(stray.slug, QStringLiteral("whole"));
    QFile second(stray.rustStorePath + QLatin1String("/second"));
    QVERIFY(second.open(QIODevice::WriteOnly));
    second.write("2");
    second.close();

    struct Ctx { int n = 0; } ctx;
    auto rename = [](const QString &from, const QString &to, void *c) {
        auto *x = static_cast<Ctx *>(c);
        ++x->n;
        if (x->n == 1 || x->n == 3)
            return false;
        return QDir().rename(from, to);
    };
    matrix::app_data::QuarantineFailure why
        = matrix::app_data::QuarantineFailure::None;
    const QString moved = matrix::app_data::quarantineRustStoreWith(
        stray, rename, &ctx, 1, 0, &why);
    QVERIFY(moved.isEmpty());
    QCOMPARE(why, matrix::app_data::QuarantineFailure::InUse);
    QVERIFY2(QFileInfo(stray.rustStorePath + QLatin1String("/marker")).exists(),
             "the first entry was left in the aside directory");
    QVERIFY(QFileInfo(stray.rustStorePath + QLatin1String("/second")).exists());
    const QDir account(QFileInfo(stray.rustStorePath).absolutePath());
    QVERIFY(account.entryList({QStringLiteral("*.orphaned-*")},
                              QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
}

void SessionStoreIdentityTest::quarantineNamesAStoreInUse()
{
    const auto stray = identityFor(QStringLiteral("@held:matrix.example"));
    seedStore(stray.slug, QStringLiteral("held-open"));
    FlakyRename always;
    always.always = true;
    matrix::app_data::QuarantineFailure why
        = matrix::app_data::QuarantineFailure::None;
    const QString moved = matrix::app_data::quarantineRustStoreWith(
        stray, &flakyRename, &always, 2, 0, &why);
    QVERIFY(moved.isEmpty());
    QCOMPARE(why, matrix::app_data::QuarantineFailure::InUse);
    QVERIFY2(always.calls >= 2, "no retry was made");
    QCOMPARE(markerIn(stray.slug), QStringLiteral("held-open"));
    const QDir account(QFileInfo(stray.rustStorePath).absolutePath());
    QVERIFY(account.entryList({QStringLiteral("*.orphaned-*")},
                              QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
}

void SessionStoreIdentityTest::asciiOnlyCaseFoldingForAdoptionCandidates()
{
    const auto canonical = identityFor(QStringLiteral("@mizerd:matrix.example"));
    seedStore(QStringLiteral("Mizerd_matrix.example"), QStringLiteral("ascii"));
    QCOMPARE(matrix::app_data::findCaseVariantStoreSlugs(canonical),
             QStringList{QStringLiteral("Mizerd_matrix.example")});

    const auto turkish = identityFor(QStringLiteral("@ismail:matrix.example"));
    seedStore(QString::fromUtf8("\xc4\xb1smail_matrix.example"),
              QStringLiteral("different-person"));
    QVERIFY(matrix::app_data::findCaseVariantStoreSlugs(turkish).isEmpty());
}

void SessionStoreIdentityTest::repairQuarantinesTheStoreInsteadOfDeletingIt()
{
    const auto identity = identityFor(QStringLiteral("@mizerd:matrix.example"));
    seedStore(identity.slug, QStringLiteral("possibly-the-only-copy"));

    const auto files = matrix::app_data::quarantineAccountRustState(identity);
    QVERIFY(files.ok());
    QVERIFY(files.removedAnything());
    QVERIFY(!storeExists(identity.slug));

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

    const auto again = matrix::app_data::quarantineAccountRustState(identity);
    QVERIFY(again.ok());
    QVERIFY(!again.removedAnything());

    seedStore(identity.slug, QStringLiteral("signing-out"));
    const auto removed = matrix::app_data::removeAccountRustState(identity);
    QVERIFY(removed.removedAnything());
    QVERIFY(!storeExists(identity.slug));
    QCOMPARE(QDir(identity.accountRoot)
                 .entryList({QStringLiteral("matrix-rust-sdk-store.orphaned-*")},
                            QDir::Dirs | QDir::NoDotAndDotDot)
                 .size(),
             0);
}

void SessionStoreIdentityTest::unreadableSecretBackendIsNeverADestructiveVerdict()
{
    using R = matrix::rust_session::StoreBlockReason;
    QVERIFY(!matrix::rust_session::suggestsLocalReset(R::SecretBackendUnavailable));
    QCOMPARE(matrix::rust_session::diagnosticName(R::SecretBackendUnavailable),
             QStringLiteral("secret_backend_unavailable"));

    const QString message =
        matrix::rust_session::userMessage(R::SecretBackendUnavailable);
    QVERIFY(!message.isEmpty());
    QVERIFY(!message.contains(QStringLiteral("different Matrix session or device")));
    QVERIFY(message.contains(QStringLiteral("Nothing has been deleted")));

    SettingsManager bare;
    QVERIFY(bare.secretBackendUnavailable());
    QVERIFY(!m_settings->secretBackendUnavailable());
}

void SessionStoreIdentityTest::
    anUnreadableSecretBlocksTheLoginThatWouldDeleteTheStore()
{
    using matrix::rust_session::unreadableSecretBlocksLogin;

    QVERIFY2(unreadableSecretBlocksLogin(true, true, false, true, false),
             "a locked keyring let a store-deleting repair through");
    QVERIFY2(unreadableSecretBlocksLogin(true, true, false, false, true),
             "an inconclusive fallback store let a store-deleting repair "
             "through — this is the one the 2026-09-10 correction exposed");
    QVERIFY(unreadableSecretBlocksLogin(true, true, false, true, true));

    QVERIFY(!unreadableSecretBlocksLogin(true, true, true, true, true));

    QVERIFY(!unreadableSecretBlocksLogin(false, true, false, true, true));
    QVERIFY(!unreadableSecretBlocksLogin(true, false, false, true, true));

    QVERIFY(!unreadableSecretBlocksLogin(true, true, false, false, false));
}

void SessionStoreIdentityTest::codeKeyedResetPolicyMatchesTheEnum()
{
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

    QVERIFY(matrix::rust_session::suggestsLocalResetForCode(
        QStringLiteral("cleanup_incomplete")));
    QVERIFY(matrix::rust_session::suggestsLocalResetForCode(
        QStringLiteral("sdk_store_ownership_mismatch")));

    QVERIFY(!matrix::rust_session::suggestsLocalResetForCode(QString{}));
    QVERIFY(!matrix::rust_session::suggestsLocalResetForCode(
        QStringLiteral("something_a_future_build_emits")));
}

void SessionStoreIdentityTest::resetOfUnknownAccountReportsNoMatch()
{
    m_settings->saveSession(QLatin1String(kServer),
                            QStringLiteral("@mizerd:matrix.example"),
                            QStringLiteral("DEVICE1"),
                            QStringLiteral("token"));

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

void SessionStoreIdentityTest::everyBlockReasonHasItsOwnCode()
{
    using R = matrix::rust_session::StoreBlockReason;
    const QList<R> all = {R::None, R::MissingSessionMetadata, R::MissingDeviceId,
                          R::DifferentAccount, R::ExistingStoreNeedsRestore,
                          R::MissingStoreForSavedSession, R::AccessTokenRevoked,
                          R::AccessTokenExpired,
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
    QCOMPARE(matrix::rust_session::diagnosticName(R::MissingStoreForSavedSession),
             QStringLiteral("saved_session_without_store"));
    QCOMPARE(matrix::rust_session::diagnosticName(R::AccessTokenRevoked),
             QStringLiteral("access_token_revoked"));
    QCOMPARE(matrix::rust_session::diagnosticName(R::AccessTokenExpired),
             QStringLiteral("access_token_expired"));
    QVERIFY(!matrix::rust_session::suggestsLocalReset(R::AccessTokenExpired));
    QVERIFY(!matrix::rust_session::suggestsLocalResetForCode(
        QStringLiteral("access_token_expired")));
    QCOMPARE(matrix::rust_session::diagnosticName(R::AmbiguousStoreCandidates),
             QStringLiteral("ambiguous_store_candidates"));
}

void SessionStoreIdentityTest::missingStoreIsNotReportedAsAForeignStore()
{
    using R = matrix::rust_session::StoreBlockReason;
    const QString foreign =
        QStringLiteral("belongs to a different Matrix session or device");

    QVERIFY(matrix::rust_session::userMessage(R::DifferentAccount)
                .contains(foreign));
    for (R r : {R::MissingStoreForSavedSession, R::AccessTokenRevoked,
                R::AccessTokenExpired,
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

    QSet<QString> seen;
    for (R r : {R::MissingStoreForSavedSession, R::AccessTokenRevoked,
                R::AccessTokenExpired,
                R::AmbiguousStoreCandidates, R::ExistingStoreNeedsRestore,
                R::MissingSessionMetadata, R::MissingDeviceId,
                R::DifferentAccount, R::InvalidSavedIdentity,
                R::SecretBackendUnavailable}) {
        seen.insert(matrix::rust_session::userMessage(r));
    }
    QCOMPARE(seen.size(), 10);
}

void SessionStoreIdentityTest::onlyRepairableReasonsOfferALocalReset()
{
    using R = matrix::rust_session::StoreBlockReason;
    QVERIFY(!matrix::rust_session::suggestsLocalReset(R::MissingStoreForSavedSession));
    QVERIFY(!matrix::rust_session::suggestsLocalReset(R::AccessTokenRevoked));
    QVERIFY(!matrix::rust_session::suggestsLocalReset(R::AmbiguousStoreCandidates));
    QVERIFY(!matrix::rust_session::suggestsLocalReset(R::ExistingStoreNeedsRestore));
    QVERIFY(!matrix::rust_session::suggestsLocalReset(R::None));

    QVERIFY(matrix::rust_session::suggestsLocalReset(R::DifferentAccount));
    QVERIFY(matrix::rust_session::suggestsLocalReset(R::MissingDeviceId));
    QVERIFY(matrix::rust_session::suggestsLocalReset(R::MissingSessionMetadata));
    QVERIFY(matrix::rust_session::suggestsLocalReset(R::InvalidSavedIdentity));
    QCOMPARE(matrix::rust_session::diagnosticName(R::InvalidSavedIdentity),
             QStringLiteral("invalid_saved_account_identity"));
}

QTEST_MAIN(SessionStoreIdentityTest)
#include "SessionStoreIdentityTest.moc"
