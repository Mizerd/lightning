#!/usr/bin/env bash
set -Eeuo pipefail

# Operator tool. Generates one Ed25519 update-signing keypair and prints
# everything that is NOT secret: the raw public key Lightning embeds, the key
# id, and the instructions for loading the private half into CI.
#
# The private key is written to a file with mode 0600 and is NEVER printed. That
# is not squeamishness: this script's output is the sort of thing that ends up
# pasted into a chat window or scrolled back through in a terminal that is being
# shared, and a printed signing key is a compromised signing key. The operator
# loads it into GitLab by hand, from the file, and then removes the file.
#
# Run this OUTSIDE CI, on a trusted machine. Do not run it in a pipeline.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
# shellcheck source=./scripts/update-lib.sh
source "$SCRIPT_DIR/update-lib.sh"

command -v openssl >/dev/null 2>&1 || die "openssl 3.x is required"

key_id="${1:-}"
out_dir="${2:-$PWD}"
if [[ -z "$key_id" ]]; then
    # Convention: lightning-release-<year><letter>, bumped on every rotation.
    key_id="lightning-release-$(date -u +%Y)a"
fi
update_valid_key_id "$key_id" || \
    die "key id must be 1-64 chars of [A-Za-z0-9._-] starting alphanumeric"

[[ -d "$out_dir" ]] || die "output directory does not exist: $out_dir"
key_file="${out_dir%/}/${key_id}.private.pem"
[[ -e "$key_file" ]] && die "refusing to overwrite an existing key file: $key_file"

# Create the file empty and locked down BEFORE openssl writes into it, so the
# key never exists on disk under a permissive mode, not even momentarily.
umask 077
: >"$key_file"
chmod 600 "$key_file"
openssl genpkey -algorithm ed25519 -out "$key_file" || die "key generation failed"
chmod 600 "$key_file"

der="$(mktemp)"
trap 'rm -f "$der"' EXIT
update_write_spki_der "$key_file" "$der" private || die "generated key is not a usable Ed25519 key"
public_b64="$(tail -c 32 "$der" | openssl base64 -A)"

# Prove the key actually signs and verifies before the operator trusts it.
probe="$(mktemp)"; probe_sig="$(mktemp)"; probe_pub="$(mktemp)"
trap 'rm -f "$der" "$probe" "$probe_sig" "$probe_pub"' EXIT
printf 'lightning update signing self-test\n' >"$probe"
openssl pkey -in "$key_file" -pubout -out "$probe_pub" 2>/dev/null || die "could not derive the public key"
openssl pkeyutl -sign -inkey "$key_file" -rawin -in "$probe" -out "$probe_sig" || die "self-test signing failed"
openssl pkeyutl -verify -pubin -inkey "$probe_pub" -rawin -in "$probe" -sigfile "$probe_sig" >/dev/null 2>&1 || \
    die "self-test verification failed; do not use this key"

# The two key-id-specific variable names, so the instructions below name the
# REAL ones rather than a 2026a example the operator has to translate. An id
# this project does not map yet (i.e. a rotation) gets an explicit placeholder
# and the note that both projects need a new variable.
pub_var="$(update_pubkey_var_for_key_id "$key_id" 2>/dev/null || true)"
if [[ -n "$pub_var" ]]; then
    cmake_var="LIGHTNING_UPDATE_PUBKEY_${pub_var#UPDATE_SIGNING_PUBKEY_}"
    mapping_note="Both names are already wired up for this key id."
else
    pub_var="UPDATE_SIGNING_PUBKEY_<SUFFIX>"
    cmake_var="LIGHTNING_UPDATE_PUBKEY_<SUFFIX>"
    mapping_note="This key id is NOT wired up yet: choose a <SUFFIX> (e.g. 2026B),
   then add the CMake cache variable + trust row in Lightning and the CI
   variable + update_pubkey_var_for_key_id row + build-script -D flags in
   lightning-deploy. Until then the publishing pipeline refuses this key id."
fi

cat <<EOF
Ed25519 update-signing keypair generated.

  key id     : ${key_id}
  public key : ${public_b64}
  private key: ${key_file}   (mode 0600, NOT printed anywhere)

The public key and key id are not secret. Publish them freely.

1) Embed the public key in Lightning (project 6), in the compiled-in trust
   table src/update/UpdateTrustStore.cpp:

     { "${key_id}", ${cmake_var}, false },

   The row reads its key from a key-id-specific CMake cache variable, which
   packaging sets per build; the value itself is the public key above. A NEW key
   id needs a NEW cache variable AND a new row -- one variable cannot serve two
   ids, because a build may trust several at once.
   ${mapping_note}

   Keep any previously trusted key id in the table (retired: false) until every
   release signed by it is out of support; only then flip it to retired: true.
   A client can never learn a key from the server, so a key that is not in a
   shipped build simply does not exist as far as that build is concerned.

2) Load the private key into lightning-deploy (project 7) CI as PROTECTED and
   MASKED variables. Do NOT paste the key from your scrollback; read it from
   the file:

     base64 -w0 < "${key_file}"

   Settings -> CI/CD -> Variables, on project 7:
     UPDATE_SIGNING_KEY_B64 = <the base64 above>  (protected, masked, File: no)
     UPDATE_SIGNING_KEY_ID  = ${key_id}   (protected)
     ${pub_var} = ${public_b64}   (protected, NOT masked)

   Masking requires a single-line value with no newline, which is why the
   private key is base64-encoded rather than pasted as PEM.

   The PUBLIC variable is not optional and is not secret: every package embeds
   it at build time as its update trust root, and a release cut without it can
   never accept an update -- which cannot be fixed afterwards, because the empty
   key is already compiled into the shipped binaries.
   scripts/check-update-signing-keys.sh refuses to publish unless it is set and
   is the public half of UPDATE_SIGNING_KEY_B64.

3) Remove the local private key file once CI holds it, or move it to the
   maintainer's offline backup. It has no other use.

     shred -u "${key_file}"    # or rm -P / secure equivalent

4) Verify the next release: download update-manifest-v1.json and its .sig from
   the latest slot and check the envelope's key_id is ${key_id}.
EOF
