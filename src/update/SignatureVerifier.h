#pragma once

#include "update/UpdateTrustStore.h"

#include <QByteArray>
#include <QString>

#include <optional>

// Lightning secure update system — Ed25519 detached signature verification.
//
// No custom cryptography: OpenSSL 3's EVP one-shot Ed25519 path does the
// verification (EVP_PKEY_new_raw_public_key + EVP_DigestVerifyInit with a
// null digest + EVP_DigestVerify). The only thing implemented here is the
// small JSON envelope parse and the policy around it.
//
// The `.sig` envelope (spec §2):
//   { "alg": "ed25519", "key_id": "...", "sig": "<base64 64-byte sig>" }
//
// Policy, all fail-closed:
//   - only `ed25519` is accepted; any other alg is rejected outright;
//   - the key id must resolve in the COMPILED-IN trust store — the server
//     can never introduce a key;
//   - the signature must decode to exactly 64 bytes and the public key to
//     exactly 32; anything else is rejected before OpenSSL is called;
//   - an empty/absent public key means "no key in this build" -> failure.
namespace lightning::update {

enum class SignatureError {
    None,
    EnvelopeTooLarge,
    EnvelopeNotJson,
    EnvelopeMalformed,
    UnsupportedAlgorithm,
    UnknownKeyId,
    MalformedKey,
    MalformedSignature,
    EmptyPayload,
    VerificationFailed,
    CryptoUnavailable,
};

QString signatureErrorText(SignatureError error);

struct SignatureEnvelope {
    QString algorithm;
    QString keyId;
    QByteArray signature; // raw 64 bytes
};

struct SignatureResult {
    bool ok = false;
    SignatureError error = SignatureError::None;
    QString keyId;
    QString message;
};

// Maximum accepted `.sig` document size (spec §2: bounded, <= 4 KiB).
constexpr qint64 kMaxSignatureEnvelopeBytes = 4 * 1024;

// Parses and validates the envelope only — no verification happens here.
std::optional<SignatureEnvelope> parseSignatureEnvelope(const QByteArray &bytes,
                                                        SignatureError *error);

// Raw Ed25519 verification. `publicKey` is the raw 32-byte key, `signature`
// the raw 64-byte detached signature over `payload` exactly as given.
bool verifyEd25519(const QByteArray &payload, const QByteArray &signature,
                   const QByteArray &publicKey);

// Full path: parse the envelope, resolve the key id in `trust`, verify over
// the RAW payload bytes. Nothing in `payload` is parsed or trusted here.
SignatureResult verifyDetachedSignature(const QByteArray &payload,
                                        const QByteArray &envelopeBytes,
                                        const TrustStore &trust);

} // namespace lightning::update
