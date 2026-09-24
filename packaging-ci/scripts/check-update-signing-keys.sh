#!/usr/bin/env bash
set -Eeuo pipefail

# Prove, before anything is built or published, that the three update-signing
# inputs agree with each other AND with the client that has to accept the result:
#
#   UPDATE_SIGNING_KEY_ID       the id written into the signature envelope
#   UPDATE_SIGNING_KEY_B64      the private key that signs the manifest (CI only)
#   UPDATE_SIGNING_PUBKEY_<id>  the public key compiled into the packages
#
# If any pair is out of step the pipeline still succeeds but publishes a
# manifest its own packages reject, and the wrong public key is already
# compiled in, so it cannot be fixed for that release. An empty public key is
# therefore fatal for a publishing pipeline.
#
# The private key is decoded via stdin to a 0600 temp file removed on exit.
# Neither key is ever printed, not even on a mismatch. Do not enable `set -x`.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
# shellcheck source=./scripts/update-lib.sh
source "$SCRIPT_DIR/update-lib.sh"

command -v openssl >/dev/null 2>&1 || \
    die "openssl is required to check the update-signing key consistency"

# `--public-only` skips the private key, which is scoped to the signing job;
# the first job uses it to reject a bad public key before builds embed it.
PUBLIC_ONLY=false
case "${1:-}" in
    "") ;;
    --public-only) PUBLIC_ONLY=true ;;
    *) die "unknown option: $1 (the only option is --public-only)" ;;
esac

require_var UPDATE_SIGNING_KEY_ID
[[ "$PUBLIC_ONLY" == true ]] || require_var UPDATE_SIGNING_KEY_B64
key_id="$UPDATE_SIGNING_KEY_ID"
update_valid_key_id "$key_id" || \
    die "UPDATE_SIGNING_KEY_ID must be 1-64 chars of [A-Za-z0-9._-] starting alphanumeric"

# Clients reject any key id absent from their compiled-in table.
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

if [[ "$PUBLIC_ONLY" == true ]]; then
    printf 'Update-signing public key for %s is configured and well-formed (private half checked in the signing job)\n' "$key_id"
    exit 0
fi

work="$(mktemp -d)"
key_file="$(mktemp)"
chmod 600 "$key_file"
cleanup() { rm -f "$key_file"; rm -rf "$work"; }
trap cleanup EXIT

# Decode via stdin so the key never appears in argv (readable via /proc).
if ! printf '%s' "$UPDATE_SIGNING_KEY_B64" | base64 -d >"$key_file" 2>/dev/null; then
    die "UPDATE_SIGNING_KEY_B64 is not valid base64"
fi
[[ -s "$key_file" ]] || die "UPDATE_SIGNING_KEY_B64 decoded to an empty key"

# Derives only the public half and checks the Ed25519 SPKI shape.
derived_pub="$(update_public_key_b64 "$key_file" private)" || \
    die "the configured signing key is not a usable Ed25519 private key"

if [[ "$derived_pub" != "$configured_pub" ]]; then
    die "$pubkey_var is not the public half of UPDATE_SIGNING_KEY_B64. Every package built by this pipeline would embed a key that cannot verify the manifest this pipeline signs. Neither value is printed; re-derive the public key from the private key file with generate-update-signing-key.sh's instructions and update the variable."
fi

printf 'Update-signing inputs agree: key id %s, %s matches the configured private key\n' \
    "$key_id" "$pubkey_var"
