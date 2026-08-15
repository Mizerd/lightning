#include "update/SignatureVerifier.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLatin1String>

#include <openssl/err.h>
#include <openssl/evp.h>

#include <memory>

namespace lightning::update {
namespace {

constexpr int kEd25519SignatureBytes = 64;
constexpr int kEd25519PublicKeyBytes = 32;

struct EvpPkeyDeleter {
    void operator()(EVP_PKEY *key) const noexcept
    {
        if (key)
            EVP_PKEY_free(key);
    }
};
struct EvpMdCtxDeleter {
    void operator()(EVP_MD_CTX *ctx) const noexcept
    {
        if (ctx)
            EVP_MD_CTX_free(ctx);
    }
};

using PkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;

// Strict base64 decode — a malformed encoding is an error, never a
// best-effort partial decode.
std::optional<QByteArray> decodeBase64Strict(const QByteArray &text)
{
    const QByteArray::FromBase64Result result =
        QByteArray::fromBase64Encoding(text, QByteArray::Base64Encoding
                                           | QByteArray::AbortOnBase64DecodingErrors);
    if (!result)
        return std::nullopt;
    return *result;
}

} // namespace

QString signatureErrorText(SignatureError error)
{
    switch (error) {
    case SignatureError::None:
        return QStringLiteral("No error");
    case SignatureError::EnvelopeTooLarge:
        return QStringLiteral("The signature file is larger than the allowed limit");
    case SignatureError::EnvelopeNotJson:
        return QStringLiteral("The signature file is not valid JSON");
    case SignatureError::EnvelopeMalformed:
        return QStringLiteral("The signature file is missing required fields");
    case SignatureError::UnsupportedAlgorithm:
        return QStringLiteral("The signature uses an unsupported algorithm");
    case SignatureError::UnknownKeyId:
        return QStringLiteral("The signature was made with a key this build does not trust");
    case SignatureError::MalformedKey:
        return QStringLiteral("The trusted key material is malformed");
    case SignatureError::MalformedSignature:
        return QStringLiteral("The signature value is malformed");
    case SignatureError::EmptyPayload:
        return QStringLiteral("There is nothing to verify");
    case SignatureError::VerificationFailed:
        return QStringLiteral("The signature does not match the downloaded file");
    case SignatureError::CryptoUnavailable:
        return QStringLiteral("The cryptographic library could not verify the signature");
    }
    return QStringLiteral("Unknown signature error");
}

std::optional<SignatureEnvelope> parseSignatureEnvelope(const QByteArray &bytes,
                                                        SignatureError *error)
{
    const auto fail = [error](SignatureError value) -> std::optional<SignatureEnvelope> {
        if (error)
            *error = value;
        return std::nullopt;
    };

    if (bytes.size() > kMaxSignatureEnvelopeBytes)
        return fail(SignatureError::EnvelopeTooLarge);
    if (bytes.isEmpty())
        return fail(SignatureError::EnvelopeNotJson);

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(SignatureError::EnvelopeNotJson);

    const QJsonObject object = document.object();
    const QJsonValue algValue = object.value(QLatin1String("alg"));
    const QJsonValue keyIdValue = object.value(QLatin1String("key_id"));
    const QJsonValue sigValue = object.value(QLatin1String("sig"));
    if (!algValue.isString() || !keyIdValue.isString() || !sigValue.isString())
        return fail(SignatureError::EnvelopeMalformed);

    SignatureEnvelope envelope;
    envelope.algorithm = algValue.toString();
    envelope.keyId = keyIdValue.toString();
    if (envelope.keyId.isEmpty())
        return fail(SignatureError::EnvelopeMalformed);

    // Algorithm agility is deliberately absent: exactly one algorithm is
    // accepted, so a downgrade cannot be negotiated by the server.
    if (envelope.algorithm != QLatin1String("ed25519"))
        return fail(SignatureError::UnsupportedAlgorithm);

    const std::optional<QByteArray> signature =
        decodeBase64Strict(sigValue.toString().toLatin1());
    if (!signature)
        return fail(SignatureError::MalformedSignature);
    if (signature->size() != kEd25519SignatureBytes)
        return fail(SignatureError::MalformedSignature);
    envelope.signature = *signature;

    if (error)
        *error = SignatureError::None;
    return envelope;
}

bool verifyEd25519(const QByteArray &payload, const QByteArray &signature,
                   const QByteArray &publicKey)
{
    if (signature.size() != kEd25519SignatureBytes)
        return false;
    if (publicKey.size() != kEd25519PublicKeyBytes)
        return false;
    if (payload.isEmpty())
        return false;

    ERR_clear_error();

    PkeyPtr key(EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char *>(publicKey.constData()),
        static_cast<std::size_t>(publicKey.size())));
    if (!key) {
        ERR_clear_error();
        return false;
    }

    MdCtxPtr ctx(EVP_MD_CTX_new());
    if (!ctx) {
        ERR_clear_error();
        return false;
    }

    // Ed25519 is a one-shot algorithm: no digest, no streaming update.
    if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
        ERR_clear_error();
        return false;
    }

    const int result = EVP_DigestVerify(
        ctx.get(), reinterpret_cast<const unsigned char *>(signature.constData()),
        static_cast<std::size_t>(signature.size()),
        reinterpret_cast<const unsigned char *>(payload.constData()),
        static_cast<std::size_t>(payload.size()));

    // Any non-1 result is a failure; the error queue is cleared either way so
    // a stale entry cannot influence a later call.
    ERR_clear_error();
    return result == 1;
}

SignatureResult verifyDetachedSignature(const QByteArray &payload,
                                        const QByteArray &envelopeBytes,
                                        const TrustStore &trust)
{
    SignatureResult result;

    if (payload.isEmpty()) {
        result.error = SignatureError::EmptyPayload;
        result.message = signatureErrorText(result.error);
        return result;
    }

    SignatureError parseError = SignatureError::None;
    const std::optional<SignatureEnvelope> envelope =
        parseSignatureEnvelope(envelopeBytes, &parseError);
    if (!envelope) {
        result.error = parseError;
        result.message = signatureErrorText(parseError);
        return result;
    }
    result.keyId = envelope->keyId;

    const QString base64Key = trust.publicKeyForId(envelope->keyId);
    if (base64Key.isEmpty()) {
        // Unknown, retired, or not compiled into this build. All three are
        // the same answer: this signature cannot be trusted.
        result.error = SignatureError::UnknownKeyId;
        result.message = signatureErrorText(result.error);
        return result;
    }

    const std::optional<QByteArray> publicKey = decodeBase64Strict(base64Key.toLatin1());
    if (!publicKey || publicKey->size() != kEd25519PublicKeyBytes) {
        result.error = SignatureError::MalformedKey;
        result.message = signatureErrorText(result.error);
        return result;
    }

    if (!verifyEd25519(payload, envelope->signature, *publicKey)) {
        result.error = SignatureError::VerificationFailed;
        result.message = signatureErrorText(result.error);
        return result;
    }

    result.ok = true;
    result.error = SignatureError::None;
    return result;
}

} // namespace lightning::update
