#!/usr/bin/env bash
set -Eeuo pipefail

# Prove, before anything is built or published, that the three update-signing
# inputs agree with each other AND with the client that has to accept the result:
#
#   UPDATE_SIGNING_KEY_ID       the id written into the signature envelope
#   UPDATE_SIGNING_KEY_B64      the PRIVATE key that signs the manifest (CI only)
#   UPDATE_SIGNING_PUBKEY_<id>  the PUBLIC key COMPILED INTO the packages
#
# Why this exists at all: nothing else connects them. The signing job uses the
# private key; the build jobs embed the public one; the client trusts a key id
# from a table compiled into it. Get any pair out of step and the pipeline still
# succeeds — it publishes a correctly signed manifest that every package it just
# built must reject. That failure is invisible until a user clicks "check for
# updates", and it is unfixable for that release: the wrong public key is already
# compiled into the shipped binaries.
#
# So all three are checked here, and an empty public key is a REFUSAL for a
# publishing pipeline rather than a warning. A build with no key compiled in
# fails closed, which is right — but shipping such a build knowingly, from a
# pipeline that holds the private key, is shipping a feature that cannot work.
#
# Credential handling: the private key is decoded through stdin into a mktemp
# file with mode 0600 and an EXIT trap, never placed in argv, and never printed.
# NEITHER key is printed by this script, not even on a mismatch — a diff of two
# keys is not a useful diagnostic and the habit is what leaks material. Do not
# enable `set -x` here.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
# shellcheck source=./scripts/update-lib.sh
source "$SCRIPT_DIR/update-lib.sh"

command -v openssl >/dev/null 2>&1 || \
    die "openssl is required to check the update-signing key consistency"

require_var UPDATE_SIGNING_KEY_ID
require_var UPDATE_SIGNING_KEY_B64
key_id="$UPDATE_SIGNING_KEY_ID"
update_valid_key_id "$key_id" || \
    die "UPDATE_SIGNING_KEY_ID must be 1-64 chars of [A-Za-z0-9._-] starting alphanumeric"

# The id must be one a shipped Lightning actually trusts. A manifest signed with
# an id absent from the compiled-in table is rejected by every client, and the
# server can never introduce a key — so this is not a formality.
trusted=false
for known in "${UPDATE_CLIENT_TRUSTED_KEY_IDS[@]}"; do
    [[ "$key_id" == "$known" ]] && trusted=true
done
[[ "$trusted" == true ]] || die \
    "UPDATE_SIGNING_KEY_ID '$key_id' is not a key id Lightning trusts (known: ${UPDATE_CLIENT_TRUSTED_KEY_IDS[*]}). Rotation needs a trust-table row and a LIGHTNING_UPDATE_PUBKEY_<id> variable in the application first; see docs/update-manifest.md."

pubkey_var="$(update_pubkey_var_for_key_id "$key_id")" || \
    die "no public-key variable is mapped for key id '$key_id' (scripts/update-lib.sh)"
configured_pub="${!pubkey_var:-}"
[[ -n "$configured_pub" ]] || die \
    "$pubkey_var is empty. Every package embeds it as the update trust root, so a release cut without it can never accept an update — and that cannot be fixed after the fact. Set it on project 7 (protected; it is NOT secret) before publishing."
update_valid_public_key_b64 "$configured_pub" || die \
    "$pubkey_var is not a base64 raw 32-byte Ed25519 public key (44 chars ending in '='). It is the value generate-update-signing-key.sh prints as 'public key', not a PEM."

work="$(mktemp -d)"
key_file="$(mktemp)"
chmod 600 "$key_file"
cleanup() { rm -f "$key_file"; rm -rf "$work"; }
trap cleanup EXIT

# Decode via stdin so the key never appears in an argument vector (argv is
# world-readable through /proc on a shared runner).
if ! printf '%s' "$UPDATE_SIGNING_KEY_B64" | base64 -d >"$key_file" 2>/dev/null; then
    die "UPDATE_SIGNING_KEY_B64 is not valid base64"
fi
[[ -s "$key_file" ]] || die "UPDATE_SIGNING_KEY_B64 decoded to an empty key"

# update_public_key_b64 derives ONLY the public half (openssl pkey -pubout) and
# proves the key is Ed25519 by its fixed SPKI shape. `openssl pkey -text` is
# never used anywhere in this pipeline: it prints private key material.
derived_pub="$(update_public_key_b64 "$key_file" private)" || \
    die "the configured signing key is not a usable Ed25519 private key"

if [[ "$derived_pub" != "$configured_pub" ]]; then
    die "$pubkey_var is not the public half of UPDATE_SIGNING_KEY_B64. Every package built by this pipeline would embed a key that cannot verify the manifest this pipeline signs. Neither value is printed; re-derive the public key from the private key file with generate-update-signing-key.sh's instructions and update the variable."
fi

printf 'Update-signing inputs agree: key id %s, %s matches the configured private key\n' \
    "$key_id" "$pubkey_var"
