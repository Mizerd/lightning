#!/usr/bin/env bash
set -Eeuo pipefail

# Produce dist/update-manifest-v1.json.sig — a detached Ed25519 signature over
# the exact bytes of dist/update-manifest-v1.json, wrapped in the small JSON
# envelope from UPDATE-SPEC §2.
#
# The private key comes from a protected, masked CI variable (base64 PKCS#8
# PEM), is decoded to a 0600 temp file removed on exit, and never reaches argv,
# output or the working tree. Keep `set -x` off here and in everything sourced.
# Never use `openssl pkey -text`: it prints private key material.
#
# The signature is verified immediately against the derived public key; an
# unverifiable signature would strand every client, so it is fatal.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
# shellcheck source=./scripts/update-lib.sh
source "$SCRIPT_DIR/update-lib.sh"

ROOT="$(project_dir)"
command -v openssl >/dev/null 2>&1 || die "openssl is required to sign the update manifest"

manifest="$ROOT/dist/${UPDATE_MANIFEST_NAME}"
[[ -f "$manifest" ]] || die "update manifest is missing; run generate-update-manifest.sh first"
[[ -s "$manifest" ]] || die "update manifest is empty"
jq -e 'type == "object" and .schema == 1' "$manifest" >/dev/null || \
    die "update manifest is not a schema 1 document"

require_var UPDATE_SIGNING_KEY_ID
require_var UPDATE_SIGNING_KEY_B64
key_id="$UPDATE_SIGNING_KEY_ID"
update_valid_key_id "$key_id" || \
    die "UPDATE_SIGNING_KEY_ID must be 1-64 chars of [A-Za-z0-9._-] starting alphanumeric"

# Re-run the resolve-source key gate in the job holding the private key:
# signing with a key the packages do not trust yields a useless manifest.
"$SCRIPT_DIR/check-update-signing-keys.sh"

work="$(mktemp -d)"
key_file="$(mktemp)"
chmod 600 "$key_file"
cleanup() {
    rm -f "$key_file"
    rm -rf "$work"
}
trap cleanup EXIT

# Decode via stdin so the key never appears in argv (readable via /proc).
if ! printf '%s' "$UPDATE_SIGNING_KEY_B64" | base64 -d >"$key_file" 2>/dev/null; then
    die "UPDATE_SIGNING_KEY_B64 is not valid base64"
fi
[[ -s "$key_file" ]] || die "UPDATE_SIGNING_KEY_B64 decoded to an empty key"

# Prove the key is Ed25519 without printing any of it.
pub_der="$work/pub.der"
update_write_spki_der "$key_file" "$pub_der" private || \
    die "the configured signing key is not a usable Ed25519 private key"
pub_pem="$work/pub.pem"
openssl pkey -in "$key_file" -pubout -out "$pub_pem" 2>/dev/null || \
    die "could not derive the public key from the configured signing key"

# -rawin selects one-shot Ed25519, matching the client's EVP_DigestVerify().
sig_bin="$work/sig.bin"
openssl pkeyutl -sign -inkey "$key_file" -rawin -in "$manifest" -out "$sig_bin" || \
    die "signing the update manifest failed"
sig_size="$(wc -c <"$sig_bin" | tr -d ' ')"
[[ "$sig_size" == 64 ]] || die "expected a 64-byte Ed25519 signature, got ${sig_size}"

# Self-verify before anything reaches dist/.
openssl pkeyutl -verify -pubin -inkey "$pub_pem" -rawin -in "$manifest" -sigfile "$sig_bin" >/dev/null 2>&1 || \
    die "the produced signature does not verify against its own public key; refusing to publish"

sig_b64="$(openssl base64 -A -in "$sig_bin")"
out="$ROOT/dist/${UPDATE_SIG_NAME}"
jq -S -n \
    --arg alg "$UPDATE_SIG_ALG" \
    --arg key_id "$key_id" \
    --arg sig "$sig_b64" \
    '{alg:$alg, key_id:$key_id, sig:$sig}' >"$out"

# The client fetches the envelope with a 4 KiB bound (UPDATE-SPEC §2).
envelope_size="$(wc -c <"$out" | tr -d ' ')"
(( envelope_size <= 4096 )) || die "signature envelope is ${envelope_size} bytes; the client bound is 4 KiB"

# Verify again through the envelope, as a client would, to catch encoding
# mistakes the raw-signature check cannot see.
round_trip="$work/roundtrip.bin"
jq -er '.sig' "$out" | openssl base64 -A -d -out "$round_trip" || \
    die "the signature envelope does not decode"
cmp -s "$sig_bin" "$round_trip" || die "the envelope's base64 does not round-trip to the signature"
openssl pkeyutl -verify -pubin -inkey "$pub_pem" -rawin -in "$manifest" -sigfile "$round_trip" >/dev/null 2>&1 || \
    die "the envelope's signature does not verify; refusing to publish"

# The public key is not secret; logging it shows which key signed.
printf 'Signed %s with key id %s (Ed25519, %s-byte signature)\n' \
    "$UPDATE_MANIFEST_NAME" "$key_id" "$sig_size"
printf 'Signing public key (base64 raw 32 bytes): %s\n' "$(tail -c 32 "$pub_der" | openssl base64 -A)"
printf 'Signature verified against the derived public key and via the published envelope\n'
