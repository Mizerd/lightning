#!/usr/bin/env bash
set -Eeuo pipefail

# End-to-end tests of the signed update manifest: generation from an already
# verified publication manifest, Ed25519 signing and self-verification, tamper
# and wrong-key rejection, determinism, and publication to the immutable
# per-release slot plus the mutable "latest" slot.
#
# Every key used here is generated inside the test at run time. No key material
# is committed, and the tests below prove none of it reaches a log.

# The PACKAGING tree, which is where this suite's scripts, packaging
# manifests and fixtures live -- not the repository root. Since the
# packaging project was folded into the application repository those
# are different directories, and the application has a scripts/ of its
# own, so `git rev-parse --show-toplevel` resolved to a real directory
# with none of these files in it. Derived from this file's own
# location so it holds wherever the tree is checked out.
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
JQ="$(command -v jq)"
command -v openssl >/dev/null 2>&1 || { printf 'error: openssl is required\n' >&2; exit 1; }
WORK="$(mktemp -d)"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

SHA=86d30b41457b1c9fd710a646326c9d1c71a52b76
VER=0.6.1
PINNED_TS=2026-08-15T12:00:00Z
MANIFEST_NAME=update-manifest-v1.json
SIG_NAME=update-manifest-v1.json.sig
# Synthetic GitHub mirror credentials. The repo is the real mirror name because
# the derived URLs are asserted literally; the token is a canary that must never
# appear in any log the scripts produce.
MIRROR_REPO=Mizerd/lightning
MIRROR_TOKEN=ghp_MOCKMIRRORCANARY0123456789abcdef
fail=0
note() { printf '  ok: %s\n' "$1"; }
bad() { printf '  FAIL: %s\n' "$1" >&2; fail=1; }

# --- signing keys, generated fresh for this run ------------------------------
KEY_A="$WORK/a.pem"
KEY_B="$WORK/b.pem"
openssl genpkey -algorithm ed25519 -out "$KEY_A" 2>/dev/null
openssl genpkey -algorithm ed25519 -out "$KEY_B" 2>/dev/null
chmod 600 "$KEY_A" "$KEY_B"
KEY_A_B64="$(openssl base64 -A -in "$KEY_A")"
KEY_B_B64="$(openssl base64 -A -in "$KEY_B")"
openssl pkey -in "$KEY_A" -pubout -out "$WORK/a.pub.pem" 2>/dev/null
openssl pkey -in "$KEY_B" -pubout -out "$WORK/b.pub.pem" 2>/dev/null
# The raw 32-byte public halves, base64 — exactly the form Lightning's compiled
# trust table stores and the form UPDATE_SIGNING_PUBKEY_<id> carries.
KEY_A_PUB_B64="$(openssl pkey -in "$KEY_A" -pubout -outform DER 2>/dev/null | tail -c 32 | openssl base64 -A)"
KEY_B_PUB_B64="$(openssl pkey -in "$KEY_B" -pubout -outform DER 2>/dev/null | tail -c 32 | openssl base64 -A)"

# Fresh project dir carrying the nine validated package files, exactly as
# write-manifest.sh expects to find them.
setup() {
    local ver="${1:-$VER}"
    TR="$WORK/run.$RANDOM.$RANDOM"
    mkdir -p "$TR/dist/windows"
    cat >"$TR/dist/version.env" <<EOF
BASE_VERSION=$ver
LOGICAL_VERSION=$ver
DEB_VERSION=$ver
RPM_VERSION=$ver
RPM_RELEASE=1
SOURCE_SHA=$SHA
SOURCE_REF=v$ver
IS_EXACT_TAG=true
PUBLISHING=true
EOF
    printf 'deb-bytes-%s\n' "$RANDOM" >"$TR/dist/lightning_${ver}_amd64.deb"
    printf 'rpm-bytes-%s\n' "$RANDOM" >"$TR/dist/lightning-${ver}-1.x86_64.rpm"
    printf 'flatpak-bytes-%s\n' "$RANDOM" >"$TR/dist/lightning_${ver}_amd64.flatpak"
    printf 'appimage-bytes-%s\n' "$RANDOM" >"$TR/dist/Lightning-${ver}-x86_64.AppImage"
    printf 'snap-bytes-%s\n' "$RANDOM" >"$TR/dist/lightning_${ver}_amd64.snap"
    printf 'win-portable-%s\n' "$RANDOM" \
        >"$TR/dist/windows/Lightning-${ver}-${SHA:0:7}-windows-x86_64-portable.zip"
    printf 'win-msi-%s\n' "$RANDOM" \
        >"$TR/dist/windows/Lightning-${ver}-${SHA:0:7}-windows-x86_64.msi"
    printf 'win-setup-%s\n' "$RANDOM" \
        >"$TR/dist/windows/Lightning-${ver}-${SHA:0:7}-windows-x86_64-setup.exe"
    printf '## Lightning %s\n\nNotes.\n' "$ver" >"$TR/dist/release-notes.md"
    MSTATE="$TR/mockstate"; mkdir -p "$MSTATE"
    MLOG="$TR/curl.log"; : >"$MLOG"
    export CI_PROJECT_DIR="$TR"
    export CI_API_V4_URL=https://gitlab.example/api/v4
    export CI_JOB_TOKEN=mock-secret-value
    export TARGET_PROJECT_ID=6 LIGHTNING_PROJECT_ID=6 PACKAGE_NAME=lightning
    export PUBLISH_PACKAGES=true RELEASE_VERSION="$ver"
    export RELEASE_ACTION=attach-existing SOURCE_REF="v$ver"
    export CURL_BIN="$ROOT/tests/mock-curl.sh"
    export MOCK_STATE_DIR="$MSTATE" MOCK_CURL_LOG="$MLOG"
    export MOCK_SOURCE_SHA="$SHA" MOCK_RELEASE_VERSION="$ver" MOCK_JQ="$JQ"
    export UPDATE_SIGNING_KEY_B64="$KEY_A_B64"
    # sign-update-manifest.sh runs the consistency gate, which requires the key
    # id to be one Lightning actually trusts and the public variable to be that
    # key's public half. The rest of this suite is about manifest mechanics, so
    # it supplies both honestly; the gate's own behaviour is tested separately.
    export UPDATE_SIGNING_KEY_ID=lightning-release-2026a
    export UPDATE_SIGNING_PUBKEY_2026A="$KEY_A_PUB_B64"
    export UPDATE_RELEASED_AT="$PINNED_TS"
    unset CI MOCK_TAG_EXISTS MOCK_RELEASE_EXISTS MOCK_TAG_SHA MOCK_CONFLICT_FILE \
          MOCK_FAIL_UPLOAD MOCK_PRESEED_LINKS MOCK_CREATE_LINKS MOCK_COMMIT_REACHABLE \
          RELEASE_NOTES_B64 PUBLISH_API_BASE CI_PIPELINE_CREATED_AT \
          GITHUB_MIRROR_REPO GITHUB_MIRROR_TOKEN \
          MOCK_GITHUB_TAG_MISSING MOCK_GITHUB_TAG_TYPE MOCK_GITHUB_TAG_OBJECT_SHA \
          MOCK_GITHUB_TAG_COMMIT MOCK_GITHUB_FAIL_CREATE MOCK_GITHUB_FAIL_UPLOAD \
          MOCK_GITHUB_ASSET_STATE MOCK_GITHUB_RELEASE_ID MOCK_GITHUB_TAG_UNAUTHORIZED
    UPD="$TR/dist/$MANIFEST_NAME"
    SIG="$TR/dist/$SIG_NAME"
}

# Every script's output is scanned for the private key in both the base64 form
# CI holds and the PEM form it is decoded to. A signing key that reaches a job
# log is a compromised signing key.
no_leak() {
    local log="$1"
    grep -q 'mock-secret-value' "$log" && bad "credential leaked in $log" || true
    grep -qF "$MIRROR_TOKEN" "$log" && bad "GitHub mirror token leaked in $log" || true
    grep -qF "$KEY_A_B64" "$log" && bad "private key (base64) leaked in $log" || true
    grep -qF "$KEY_B_B64" "$log" && bad "second private key leaked in $log" || true
    grep -q 'BEGIN PRIVATE KEY' "$log" && bad "private key PEM leaked in $log" || true
    # A 40+ char run of the key's own base64 body would be a partial leak.
    grep -qF "${KEY_A_B64:20:40}" "$log" && bad "private key fragment leaked in $log" || true
}
run() { "$@" >"$TR/out.log" 2>&1; local rc=$?; no_leak "$TR/out.log"; cat "$TR/out.log" >>"$TR/all.log"; return $rc; }

# Publish + verify the release packages so a verification record exists.
publish_and_verify() {
    run "$ROOT/scripts/write-manifest.sh" \
        && run "$ROOT/scripts/publish-packages.sh" \
        && run "$ROOT/scripts/verify-published-packages.sh"
}

printf '== generation guards ==\n'
setup
run "$ROOT/scripts/write-manifest.sh" || bad "publication manifest build failed"
if run "$ROOT/scripts/generate-update-manifest.sh"; then
    bad "generated an update manifest without a verification record"
else
    note "refuses to run without dist/verification.json"
fi
publish_and_verify || bad "publish/verify chain failed"
run "$ROOT/scripts/generate-update-manifest.sh" || bad "update manifest generation failed"
[[ -f "$UPD" ]] || bad "no update manifest written"

printf '== manifest content ==\n'
M="$TR/dist/manifest.json"
[[ "$($JQ -r '.schema' "$UPD")" == 1 ]] && note "schema 1" || bad "schema"
[[ "$($JQ -r '.version' "$UPD")" == "$VER" ]] && note "version" || bad "version"
[[ "$($JQ -r '.tag' "$UPD")" == "v$VER" ]] && note "tag" || bad "tag"
[[ "$($JQ -r '.channel' "$UPD")" == stable ]] && note "stable channel" || bad "channel"
[[ "$($JQ -r '.released' "$UPD")" == "$PINNED_TS" ]] && note "pinned release timestamp" || bad "released"
# The expiry is what bounds a replayed `latest` pair; the client refuses a
# manifest without one. Default window 120 days, measured from `released`.
[[ "$($JQ -r '.expires' "$UPD")" == "$(date -u -d "$PINNED_TS + 120 days" +%Y-%m-%dT%H:%M:%SZ)" ]] \
    && note "expires = released + 120 days" || bad "expires"
(cd "$TR" && UPDATE_MANIFEST_VALIDITY_DAYS=0 "$ROOT/scripts/generate-update-manifest.sh" >/dev/null 2>&1) \
    && bad "a zero-day validity was accepted" || note "validity must be at least one day"
run env UPDATE_MANIFEST_VALIDITY_DAYS=30 "$ROOT/scripts/generate-update-manifest.sh" || bad "regenerate with 30-day validity"
[[ "$($JQ -r '.expires' "$UPD")" == "$(date -u -d "$PINNED_TS + 30 days" +%Y-%m-%dT%H:%M:%SZ)" ]] \
    && note "validity window is configurable" || bad "expires (30 days)"
# An explicit expiry instant beats the window (that is what a refresh uses).
run env UPDATE_EXPIRES_AT=2027-03-01T00:00:00Z "$ROOT/scripts/generate-update-manifest.sh" || bad "regenerate with UPDATE_EXPIRES_AT"
[[ "$($JQ -r '.expires' "$UPD")" == 2027-03-01T00:00:00Z ]] && note "UPDATE_EXPIRES_AT takes precedence" || bad "expires (explicit)"
(cd "$TR" && UPDATE_EXPIRES_AT=2020-01-01T00:00:00Z "$ROOT/scripts/generate-update-manifest.sh" >/dev/null 2>&1) \
    && bad "an expiry before the release was accepted" || note "an expiry before the release is refused"
(cd "$TR" && UPDATE_MANIFEST_VALIDITY_DAYS=0120 "$ROOT/scripts/generate-update-manifest.sh" >/dev/null 2>&1) \
    && bad "a leading-zero validity was accepted" || note "a leading-zero validity is refused"
run "$ROOT/scripts/generate-update-manifest.sh" || bad "regenerate with the default validity"
[[ "$($JQ -r '.min_updater_version' "$UPD")" == 1 ]] && note "min_updater_version" || bad "min_updater_version"
[[ "$($JQ -r '.release_notes_url' "$UPD")" == "https://gitlab.smetonis.net/Mizerd/lightning/-/releases/v$VER" ]] \
    && note "release notes URL points at the release page" || bad "release_notes_url"

expected_keys='["linux-appimage","linux-deb","linux-rpm","windows-msi","windows-portable","windows-setup"]'
actual_keys="$($JQ -c '.artifacts | keys' "$UPD")"
[[ "$actual_keys" == "$expected_keys" ]] && note "exactly the six directly-updatable install types" \
    || bad "artifact keys are $actual_keys, expected $expected_keys"

# sha256 / size / filename must be copied from the verified publication
# manifest, never recomputed.
check_passthrough() { # install-key publication-format
    local key="$1" fmt="$2" f s h u pf ps ph
    f="$($JQ -r --arg k "$key" '.artifacts[$k].filename' "$UPD")"
    s="$($JQ -r --arg k "$key" '.artifacts[$k].size' "$UPD")"
    h="$($JQ -r --arg k "$key" '.artifacts[$k].sha256' "$UPD")"
    u="$($JQ -r --arg k "$key" '.artifacts[$k].url' "$UPD")"
    pf="$($JQ -r --arg f "$fmt" '.entries[] | select(.format == $f) | .filename' "$M")"
    ps="$($JQ -r --arg f "$fmt" '.entries[] | select(.format == $f) | .size' "$M")"
    ph="$($JQ -r --arg f "$fmt" '.entries[] | select(.format == $f) | .sha256' "$M")"
    [[ "$f" == "$pf" && "$s" == "$ps" && "$h" == "$ph" ]] \
        && note "$key passes through filename/size/sha256 from the publication manifest" \
        || bad "$key metadata does not match the publication manifest"
    [[ "$h" == "$(sha256sum "$TR/dist/"*"/$pf" 2>/dev/null | cut -d' ' -f1)" || \
       "$h" == "$(sha256sum "$TR/dist/$pf" 2>/dev/null | cut -d' ' -f1)" ]] \
        && note "$key sha256 matches the real published bytes" || bad "$key sha256 does not match the file"
    [[ "$u" == https://gitlab.example/api/v4/projects/6/packages/generic/lightning/$VER/$pf ]] \
        && note "$key url is the canonical https registry URL" || bad "$key url is $u"
}
check_passthrough linux-deb deb
check_passthrough linux-rpm rpm
check_passthrough linux-appimage appimage
check_passthrough windows-msi windows-msi
check_passthrough windows-setup windows-setup
check_passthrough windows-portable windows-portable

printf '== flatpak and snap are not directly-updatable artifacts ==\n'
for k in linux-flatpak linux-snap flatpak snap; do
    [[ "$($JQ -r --arg k "$k" '.artifacts | has($k)' "$UPD")" == false ]] \
        && note "no '$k' artifact entry" || bad "'$k' appears as a directly updatable artifact"
done
# ...and no artifact URL points at the flatpak/snap bundles or SHA256SUMS.
urls="$($JQ -r '.artifacts[].url' "$UPD")"
grep -q '\.flatpak' <<<"$urls" && bad "a .flatpak is advertised as a direct download" || note "no .flatpak download advertised"
grep -q '\.snap' <<<"$urls" && bad "a .snap is advertised as a direct download" || note "no .snap download advertised"
grep -q 'SHA256SUMS' <<<"$urls" && bad "SHA256SUMS is advertised as an update artifact" || note "SHA256SUMS is not an update artifact"

printf '== channels are honest about what does not exist ==\n'
ch_keys="$($JQ -c '.channels | keys' "$UPD")"
[[ "$ch_keys" == '["linux-deb-repo","linux-flatpak","linux-rpm-repo","linux-snap"]' ]] \
    && note "four ecosystem channels described" || bad "channel keys are $ch_keys"
for k in linux-flatpak linux-snap linux-deb-repo linux-rpm-repo; do
    [[ "$($JQ -r --arg k "$k" '.channels[$k].available' "$UPD")" == false ]] \
        && note "$k available=false" || bad "$k claims availability that does not exist"
    [[ "$($JQ -r --arg k "$k" '.channels[$k].version' "$UPD")" == null ]] \
        && note "$k version is null" || bad "$k states a version while unavailable"
    [[ -n "$($JQ -r --arg k "$k" '.channels[$k].note // empty' "$UPD")" ]] \
        && note "$k carries an explanatory note" || bad "$k has no note"
done
$JQ -e '.channels["linux-flatpak"].note | test("Flathub")' "$UPD" >/dev/null \
    && note "flatpak note names the missing Flathub publication" || bad "flatpak note is vague"
$JQ -e '.channels["linux-snap"].note | test("Snap Store")' "$UPD" >/dev/null \
    && note "snap note names the missing Snap Store publication" || bad "snap note is vague"

printf '== a channel may not claim availability without a version ==\n'
if run env UPDATE_CHANNEL_FLATPAK_AVAILABLE=true "$ROOT/scripts/generate-update-manifest.sh"; then
    bad "available channel accepted without a version"
else
    note "available=true requires a version"
fi
if run env UPDATE_CHANNEL_SNAP_AVAILABLE=yes "$ROOT/scripts/generate-update-manifest.sh"; then
    bad "non-boolean availability accepted"
else
    note "non-boolean channel availability rejected"
fi
# Flipping a channel on is a variable change, not a code change.
run env UPDATE_CHANNEL_FLATPAK_AVAILABLE=true UPDATE_CHANNEL_FLATPAK_VERSION="$VER" \
    "$ROOT/scripts/generate-update-manifest.sh" || bad "flippable channel failed"
[[ "$($JQ -r '.channels["linux-flatpak"].available' "$UPD")" == true \
   && "$($JQ -r '.channels["linux-flatpak"].version' "$UPD")" == "$VER" ]] \
    && note "a channel can be flipped available by variable alone" || bad "channel flip did not take"
run "$ROOT/scripts/generate-update-manifest.sh" || bad "regeneration failed"

printf '== deterministic output ==\n'
cp "$UPD" "$WORK/first.json"
run "$ROOT/scripts/generate-update-manifest.sh" || bad "second generation failed"
cmp -s "$WORK/first.json" "$UPD" && note "two runs from the same inputs are byte-identical" \
    || bad "generation is not deterministic"
[[ "$($JQ -S -c . "$UPD" | cmp -s - <($JQ -c . "$UPD"); echo $?)" == 0 ]] \
    && note "keys are sorted (stable serialization)" || bad "keys are not sorted"

printf '== sign / verify round trip ==\n'
run "$ROOT/scripts/sign-update-manifest.sh" || bad "signing failed"
[[ -f "$SIG" ]] || bad "no signature envelope written"
[[ "$($JQ -r '.alg' "$SIG")" == ed25519 ]] && note "envelope alg is ed25519" || bad "envelope alg"
[[ "$($JQ -r '.key_id' "$SIG")" == lightning-release-2026a ]] && note "envelope carries the key id" || bad "envelope key id"
[[ "$($JQ -c 'keys' "$SIG")" == '["alg","key_id","sig"]' ]] && note "envelope has exactly alg/key_id/sig" || bad "envelope shape"
$JQ -r '.sig' "$SIG" | openssl base64 -A -d >"$WORK/sig.bin" 2>/dev/null
[[ "$(wc -c <"$WORK/sig.bin" | tr -d ' ')" == 64 ]] && note "64-byte raw Ed25519 signature" || bad "signature length"
if openssl pkeyutl -verify -pubin -inkey "$WORK/a.pub.pem" -rawin -in "$UPD" -sigfile "$WORK/sig.bin" >/dev/null 2>&1; then
    note "signature verifies against the signing key's public half"
else
    bad "signature does not verify"
fi
[[ "$(wc -c <"$SIG" | tr -d ' ')" -le 4096 ]] && note "envelope is within the client's 4 KiB bound" || bad "envelope too large"

printf '== tampering and wrong keys are rejected ==\n'
cp "$UPD" "$WORK/tampered.json"
# Flip one byte inside the version string; the bytes signed are the manifest's.
sed -i 's/"version": "0.6.1"/"version": "9.9.9"/' "$WORK/tampered.json"
cmp -s "$UPD" "$WORK/tampered.json" && bad "tamper fixture did not change the file" || true
if openssl pkeyutl -verify -pubin -inkey "$WORK/a.pub.pem" -rawin -in "$WORK/tampered.json" -sigfile "$WORK/sig.bin" >/dev/null 2>&1; then
    bad "a tampered manifest still verified"
else
    note "a tampered manifest fails verification"
fi
if openssl pkeyutl -verify -pubin -inkey "$WORK/b.pub.pem" -rawin -in "$UPD" -sigfile "$WORK/sig.bin" >/dev/null 2>&1; then
    bad "signature verified under an unrelated public key"
else
    note "a signature made by a different key fails verification"
fi
# Sign the same manifest with key B; it must not verify under key A.
openssl pkeyutl -sign -inkey "$KEY_B" -rawin -in "$UPD" -out "$WORK/sig-b.bin"
if openssl pkeyutl -verify -pubin -inkey "$WORK/a.pub.pem" -rawin -in "$UPD" -sigfile "$WORK/sig-b.bin" >/dev/null 2>&1; then
    bad "key B's signature verified under key A"
else
    note "key B's signature is rejected by key A"
fi

printf '== signing guards ==\n'
if run env -u UPDATE_SIGNING_KEY_B64 "$ROOT/scripts/sign-update-manifest.sh"; then
    bad "signed without a key"
else
    note "missing UPDATE_SIGNING_KEY_B64 rejected"
fi
if run env -u UPDATE_SIGNING_KEY_ID "$ROOT/scripts/sign-update-manifest.sh"; then
    bad "signed without a key id"
else
    note "missing UPDATE_SIGNING_KEY_ID rejected"
fi
if run env UPDATE_SIGNING_KEY_ID='bad id/../x' "$ROOT/scripts/sign-update-manifest.sh"; then
    bad "malformed key id accepted"
else
    note "malformed key id rejected"
fi
if run env UPDATE_SIGNING_KEY_B64='not base64 !!' "$ROOT/scripts/sign-update-manifest.sh"; then
    bad "non-base64 key accepted"
else
    note "non-base64 key rejected"
fi
# An RSA key is valid PEM and a valid signing key -- but not Ed25519.
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$WORK/rsa.pem" 2>/dev/null
if run env UPDATE_SIGNING_KEY_B64="$(openssl base64 -A -in "$WORK/rsa.pem")" \
        "$ROOT/scripts/sign-update-manifest.sh"; then
    bad "a non-Ed25519 key was accepted"
else
    note "a non-Ed25519 key is rejected"
fi
rm -f "$TR/dist/$MANIFEST_NAME.absent"
mv "$UPD" "$WORK/held.json"
if run "$ROOT/scripts/sign-update-manifest.sh"; then bad "signed a missing manifest"; else note "missing manifest rejected"; fi
mv "$WORK/held.json" "$UPD"
run "$ROOT/scripts/sign-update-manifest.sh" || bad "re-signing failed"

printf '== signing-key consistency gate ==\n'
# Nothing else in the pipeline connects the private key that SIGNS the manifest,
# the public key COMPILED INTO the packages, and the key id the client trusts.
# Out of step, the pipeline still succeeds and publishes a correctly signed
# manifest that every package it just built must reject — invisible until a user
# clicks "check for updates", and unfixable for that release.
GATE="$ROOT/scripts/check-update-signing-keys.sh"
gate() { run env "$@" "$GATE"; }

gate UPDATE_SIGNING_KEY_ID=lightning-release-2026a \
     UPDATE_SIGNING_KEY_B64="$KEY_A_B64" \
     UPDATE_SIGNING_PUBKEY_2026A="$KEY_A_PUB_B64" \
    && note "a matching key id / private key / public key triple is accepted" \
    || bad "a correct triple was rejected"

# The defect the gate exists for: the public half of a DIFFERENT key embedded in
# the packages. Both values are valid Ed25519 keys, so only the comparison
# catches it.
if gate UPDATE_SIGNING_KEY_ID=lightning-release-2026a \
        UPDATE_SIGNING_KEY_B64="$KEY_A_B64" \
        UPDATE_SIGNING_PUBKEY_2026A="$KEY_B_PUB_B64"; then
    bad "a public key from an unrelated keypair was accepted"
else
    note "a public key that is not the private key's half is rejected"
fi
grep -qF "$KEY_A_PUB_B64" "$TR/out.log" || grep -qF "$KEY_B_PUB_B64" "$TR/out.log" \
    && bad "the mismatch diagnostic printed key material" \
    || note "the mismatch diagnostic prints neither key"

# (c): a publishing pipeline REFUSES an empty public key rather than shipping
# packages whose update feature can never accept anything.
if gate UPDATE_SIGNING_KEY_ID=lightning-release-2026a \
        UPDATE_SIGNING_KEY_B64="$KEY_A_B64" \
        UPDATE_SIGNING_PUBKEY_2026A=""; then
    bad "an empty public key was accepted for a publishing release"
else
    note "an empty UPDATE_SIGNING_PUBKEY_2026A is refused, not warned about"
fi

# A PEM pasted where the raw base64 belongs is the likely operator mistake.
if gate UPDATE_SIGNING_KEY_ID=lightning-release-2026a \
        UPDATE_SIGNING_KEY_B64="$KEY_A_B64" \
        UPDATE_SIGNING_PUBKEY_2026A="$(openssl base64 -A -in "$WORK/a.pub.pem")"; then
    bad "a PEM-shaped public key was accepted"
else
    note "a public key that is not 44 base64 chars is rejected"
fi
if gate UPDATE_SIGNING_KEY_ID=lightning-release-2026a \
        UPDATE_SIGNING_KEY_B64="$KEY_A_B64" \
        UPDATE_SIGNING_PUBKEY_2026A="${KEY_A_PUB_B64:0:20}"; then
    bad "a truncated public key was accepted"
else
    note "a truncated public key is rejected"
fi

# Rotation must not be possible from CI variables alone: the id has to exist in
# Lightning's compiled trust table AND have its own public-key variable first.
if gate UPDATE_SIGNING_KEY_ID=lightning-release-2026b \
        UPDATE_SIGNING_KEY_B64="$KEY_B_B64" \
        UPDATE_SIGNING_PUBKEY_2026B="$KEY_B_PUB_B64"; then
    bad "an unknown key id was accepted"
else
    note "a key id no shipped Lightning trusts is rejected"
    grep -q 'not a key id Lightning trusts' "$TR/out.log" \
        && note "the error names the rotation requirement" || bad "unhelpful rotation error"
fi
if gate UPDATE_SIGNING_KEY_ID='bad id/../x' UPDATE_SIGNING_KEY_B64="$KEY_A_B64"; then
    bad "a malformed key id was accepted"
else
    note "a malformed key id is rejected"
fi
if gate -u UPDATE_SIGNING_KEY_B64 UPDATE_SIGNING_KEY_ID=lightning-release-2026a \
        UPDATE_SIGNING_PUBKEY_2026A="$KEY_A_PUB_B64"; then
    bad "the gate ran without a private key"
else
    note "a missing private key is rejected"
fi

# The gate is wired into the FIRST publishing job, so a mismatch fails before
# any build job embeds the wrong trust root into an artifact.
# Caller arguments come FIRST because some of them are `env` options (-u), and
# GNU env stops parsing options at the first NAME=VALUE assignment.
release_gate_env() {
    run env "$@" CI_PROJECT_DIR="$TR" TARGET_PROJECT_ID=6 LIGHTNING_PROJECT_ID=6 \
        PUBLISH_PACKAGES=true RELEASE_ACTION=attach-existing \
        RELEASE_VERSION="$VER" SOURCE_REF="v$VER" \
        GIPHY_API_KEY=CANARY_G KLIPY_API_KEY=CANARY_K \
        "$ROOT/scripts/validate-release-request.sh"
}
release_gate_env UPDATE_SIGNING_KEY_ID=lightning-release-2026a \
    UPDATE_SIGNING_KEY_B64="$KEY_A_B64" UPDATE_SIGNING_PUBKEY_2026A="$KEY_A_PUB_B64" \
    && note "validate-release-request accepts a consistent triple" \
    || bad "validate-release-request rejected a consistent triple"
if release_gate_env UPDATE_SIGNING_KEY_ID=lightning-release-2026a \
        UPDATE_SIGNING_KEY_B64="$KEY_A_B64" \
        UPDATE_SIGNING_PUBKEY_2026A="$KEY_B_PUB_B64"; then
    bad "validate-release-request accepted a key/package mismatch"
else
    note "validate-release-request fails a publishing pipeline on a key mismatch"
fi
if release_gate_env -u UPDATE_SIGNING_PUBKEY_2026A \
        UPDATE_SIGNING_KEY_ID=lightning-release-2026a \
        UPDATE_SIGNING_KEY_B64="$KEY_A_B64"; then
    bad "validate-release-request accepted a release with no embedded public key"
else
    note "validate-release-request refuses to publish with no public key set"
fi
# A build-only pipeline needs none of this and must not be blocked by it.
run env CI_PROJECT_DIR="$TR" PUBLISH_PACKAGES=false \
    "$ROOT/scripts/validate-release-request.sh" \
    && note "a build-only pipeline is unaffected by the signing gate" \
    || bad "the signing gate blocked a build-only pipeline"

printf '== the private key never reaches a log ==\n'
# no_leak() already ran on every invocation above; assert over the accumulated
# transcript too, so a leak in any single step is caught even if that step's
# own log was overwritten.
no_leak "$TR/all.log"
grep -qF "$KEY_A_B64" "$MLOG" && bad "private key reached the request log" || note "no key material in the request log"
[[ "$fail" == 0 ]] && note "no key material in any captured script output" || true
# The public key IS printed, deliberately -- it is not secret.
grep -qF "$(openssl pkey -in "$KEY_A" -pubout -outform DER 2>/dev/null | tail -c 32 | openssl base64 -A)" "$TR/all.log" \
    && note "the public key is reported for operator verification" || bad "public key not reported"

printf '== publication guards ==\n'
setup
publish_and_verify || bad "publish/verify failed"
run "$ROOT/scripts/generate-update-manifest.sh" && run "$ROOT/scripts/sign-update-manifest.sh" || bad "generate+sign failed"
mv "$TR/dist/verification.json" "$WORK/verification.held.json"
if run "$ROOT/scripts/publish-update-manifest.sh"; then
    bad "published an update manifest with no verification record"
else
    note "publication refuses without dist/verification.json"
fi
mv "$WORK/verification.held.json" "$TR/dist/verification.json"
mv "$SIG" "$WORK/sig.held.json"
if run "$ROOT/scripts/publish-update-manifest.sh"; then
    bad "published an unsigned manifest"
else
    note "publication refuses without a signature envelope"
fi
mv "$WORK/sig.held.json" "$SIG"

printf '== publication ordering and both slots ==\n'
: >"$MLOG"
run "$ROOT/scripts/publish-update-manifest.sh" || bad "publication failed"
UREG="$MSTATE/registry-lightning-update"
[[ -f "$UREG/$VER/$MANIFEST_NAME" && -f "$UREG/$VER/$SIG_NAME" ]] \
    && note "immutable per-release copy published" || bad "per-release copy missing"
[[ -f "$UREG/latest/$MANIFEST_NAME" && -f "$UREG/latest/$SIG_NAME" ]] \
    && note "latest slot published" || bad "latest slot missing"
cmp -s "$UPD" "$UREG/$VER/$MANIFEST_NAME" && note "per-release manifest bytes match" || bad "per-release manifest bytes differ"
cmp -s "$UPD" "$UREG/latest/$MANIFEST_NAME" && note "latest manifest bytes match" || bad "latest manifest bytes differ"
cmp -s "$SIG" "$UREG/latest/$SIG_NAME" && note "latest signature bytes match" || bad "latest signature bytes differ"
# Ordering: every PUT into the versioned slot precedes every PUT into latest.
mapfile -t puts < <(grep '^PUT .*/packages/generic/lightning-update/' "$MLOG" | sed 's#.*/lightning-update/##; s#/.*##')
[[ "${#puts[@]}" == 4 ]] && note "four update files uploaded" || bad "unexpected update upload count: ${#puts[@]}"
last_versioned=-1; first_latest=-1
for i in "${!puts[@]}"; do
    if [[ "${puts[$i]}" == latest ]]; then
        [[ "$first_latest" == -1 ]] && first_latest=$i
    else
        last_versioned=$i
    fi
done
(( first_latest > last_versioned )) \
    && note "the latest slot is uploaded only after the versioned copy" \
    || bad "latest was uploaded before the versioned copy finished"
# The signature is promoted before the manifest, so a racing client can only
# ever see "new signature, old manifest" -- which fails closed.
[[ "$(grep '^PUT .*/lightning-update/latest/' "$MLOG" | head -1)" == *"$SIG_NAME" ]] \
    && note "the latest signature is promoted before the latest manifest" || bad "latest promotion order"
# The nine release packages were not touched by this job.
[[ "$(grep -c '^PUT .*/packages/generic/lightning/' "$MLOG")" == 0 ]] \
    && note "no release package was re-uploaded" || bad "the update job wrote a release package"

printf '== per-release copy is immutable, latest is not ==\n'
: >"$MLOG"
run "$ROOT/scripts/publish-update-manifest.sh" || bad "idempotent re-publication failed"
[[ "$(grep -c '^PUT ' "$MLOG")" == 0 ]] && note "identical re-run uploads nothing" || bad "identical re-run re-uploaded"
# A different manifest under the same version is a hard conflict.
cp "$UPD" "$WORK/orig.json"; cp "$SIG" "$WORK/orig.sig"
run env UPDATE_RELEASED_AT=2026-09-01T00:00:00Z "$ROOT/scripts/generate-update-manifest.sh" \
    && run "$ROOT/scripts/sign-update-manifest.sh" || bad "regeneration failed"
cmp -s "$WORK/orig.json" "$UPD" && bad "regeneration produced identical bytes; conflict not exercised" || true
if run "$ROOT/scripts/publish-update-manifest.sh"; then
    bad "a different manifest overwrote the immutable per-release copy"
else
    note "the per-release copy rejects different bytes"
fi
cp "$WORK/orig.json" "$UPD"; cp "$WORK/orig.sig" "$SIG"

# A NEW version must be able to move the latest pointer. Fresh package state,
# but the latest slot carried over from the run above.
printf '== a newer release replaces the latest slot ==\n'
PREV_LATEST="$WORK/prev-latest.json"
cp "$UREG/latest/$MANIFEST_NAME" "$PREV_LATEST"
setup 0.6.2
mkdir -p "$MSTATE/registry-lightning-update/latest"
cp "$PREV_LATEST" "$MSTATE/registry-lightning-update/latest/$MANIFEST_NAME"
cp "$WORK/orig.sig" "$MSTATE/registry-lightning-update/latest/$SIG_NAME"
publish_and_verify || bad "0.6.2 publish/verify failed"
run "$ROOT/scripts/generate-update-manifest.sh" && run "$ROOT/scripts/sign-update-manifest.sh" \
    || bad "0.6.2 generate+sign failed"
: >"$MLOG"
run "$ROOT/scripts/publish-update-manifest.sh" || bad "0.6.2 update publication failed"
UREG="$MSTATE/registry-lightning-update"
[[ "$($JQ -r '.version' "$UREG/latest/$MANIFEST_NAME")" == 0.6.2 ]] \
    && note "the latest slot now advertises 0.6.2" || bad "latest slot was not replaced"
[[ -f "$UREG/0.6.2/$MANIFEST_NAME" ]] && note "0.6.2 has its own immutable copy" || bad "0.6.2 per-release copy missing"
cmp -s "$PREV_LATEST" "$UREG/latest/$MANIFEST_NAME" && bad "latest still holds the old manifest" || note "the old latest bytes were replaced"
[[ -f "$TR/dist/update-publication.json" ]] \
    && [[ "$($JQ -r '.urls.latest_manifest' "$TR/dist/update-publication.json")" == \
          "https://gitlab.example/api/v4/projects/6/packages/generic/lightning-update/latest/$MANIFEST_NAME" ]] \
    && note "publication record names the canonical latest URL" || bad "update-publication.json"

# The latest slot must not go BACKWARDS by accident: attach-existing on an
# old release runs this same script, and re-pointing `latest` at the old
# version freezes every current installation on "up to date".
printf '== the latest slot refuses to roll back unless told to ==\n'
NEWER_LATEST="$WORK/newer-latest.json"
NEWER_LATEST_SIG="$WORK/newer-latest.sig"
cp "$UREG/latest/$MANIFEST_NAME" "$NEWER_LATEST"
cp "$UREG/latest/$SIG_NAME" "$NEWER_LATEST_SIG"
setup 0.6.1
mkdir -p "$MSTATE/registry-lightning-update/latest"
cp "$NEWER_LATEST" "$MSTATE/registry-lightning-update/latest/$MANIFEST_NAME"
cp "$NEWER_LATEST_SIG" "$MSTATE/registry-lightning-update/latest/$SIG_NAME"
publish_and_verify || bad "0.6.1 backfill publish/verify failed"
run "$ROOT/scripts/generate-update-manifest.sh" && run "$ROOT/scripts/sign-update-manifest.sh" \
    || bad "0.6.1 generate+sign failed"
: >"$MLOG"
UREG="$MSTATE/registry-lightning-update"
if run "$ROOT/scripts/publish-update-manifest.sh"; then
    bad "a backfill of 0.6.1 rolled the latest slot back from 0.6.2"
else
    grep -q 'refusing to move the latest slot backwards' "$TR/out.log" \
        && note "the older manifest was refused by name" || bad "refusal reason missing"
fi
[[ "$($JQ -r '.version' "$UREG/latest/$MANIFEST_NAME")" == 0.6.2 ]] \
    && note "the latest slot still advertises 0.6.2" || bad "latest slot was rolled back"
# The per-release copy of the backfilled version IS published: only `latest`
# is protected.
[[ -f "$UREG/0.6.1/$MANIFEST_NAME" ]] && note "0.6.1 keeps its own immutable copy" || bad "0.6.1 per-release copy missing"
# Yanking a bad release is the one legitimate backwards move, and it is spelled out.
: >"$MLOG"
run env UPDATE_ALLOW_LATEST_ROLLBACK=true "$ROOT/scripts/publish-update-manifest.sh" \
    || bad "an explicit rollback was refused"
[[ "$($JQ -r '.version' "$UREG/latest/$MANIFEST_NAME")" == 0.6.1 ]] \
    && note "UPDATE_ALLOW_LATEST_ROLLBACK=true moves latest backwards deliberately" || bad "explicit rollback did not move latest"
grep -q 'WARNING: rolling the latest slot back' "$TR/out.log" && note "the rollback is announced" || bad "rollback warning missing"
unset UPDATE_ALLOW_LATEST_ROLLBACK
# Restore the state the following cases expect: 0.6.2 in the latest slot.
setup 0.6.2
mkdir -p "$MSTATE/registry-lightning-update/latest"
cp "$NEWER_LATEST" "$MSTATE/registry-lightning-update/latest/$MANIFEST_NAME"
cp "$NEWER_LATEST_SIG" "$MSTATE/registry-lightning-update/latest/$SIG_NAME"
publish_and_verify || bad "0.6.2 re-setup publish/verify failed"
run "$ROOT/scripts/generate-update-manifest.sh" && run "$ROOT/scripts/sign-update-manifest.sh" \
    || bad "0.6.2 re-setup generate+sign failed"
run "$ROOT/scripts/publish-update-manifest.sh" || bad "0.6.2 re-setup publication failed"
UREG="$MSTATE/registry-lightning-update"

# A release lull past the window: the latest slot is refreshed IN PLACE with
# a new expiry, the immutable copy is untouched, and no other version can
# ride in on the refresh flag.
printf '== the latest slot can be refreshed with a new expiry, without a release ==\n'
: >"$MLOG"
run env UPDATE_EXPIRES_AT=2027-06-01T00:00:00Z "$ROOT/scripts/generate-update-manifest.sh" \
    && run "$ROOT/scripts/sign-update-manifest.sh" || bad "refresh generate+sign failed"
ORIG_IMMUTABLE_SUM="$(sha256sum "$UREG/0.6.2/$MANIFEST_NAME" | cut -d' ' -f1)"
run env UPDATE_REFRESH_LATEST_ONLY=true "$ROOT/scripts/publish-update-manifest.sh" || bad "refresh publication failed"
[[ "$($JQ -r '.expires' "$UREG/latest/$MANIFEST_NAME")" == 2027-06-01T00:00:00Z ]] \
    && note "the latest slot carries the refreshed expiry" || bad "latest expiry not refreshed"
[[ "$(sha256sum "$UREG/0.6.2/$MANIFEST_NAME" | cut -d' ' -f1)" == "$ORIG_IMMUTABLE_SUM" ]] \
    && note "the immutable per-release copy is untouched by a refresh" || bad "refresh touched the immutable copy"
[[ "$(grep -c '^PUT .*/0.6.2/' "$MLOG")" == 0 ]] && note "refresh uploads nothing under the version slot" || bad "refresh wrote to the version slot"
$JQ -S '.version = "0.6.3"' "$UPD" >"$WORK/refresh-other.json" && cp "$WORK/refresh-other.json" "$UPD"
run "$ROOT/scripts/sign-update-manifest.sh" >/dev/null 2>&1 || true
if run env UPDATE_REFRESH_LATEST_ONLY=true "$ROOT/scripts/publish-update-manifest.sh"; then
    bad "refresh mode promoted a DIFFERENT version"
else
    note "refresh mode refuses any version but the one already in latest"
fi
# Restore a consistent 0.6.2 manifest for the cases that follow.
run "$ROOT/scripts/generate-update-manifest.sh" && run "$ROOT/scripts/sign-update-manifest.sh" \
    || bad "post-refresh regenerate failed"
run env UPDATE_ALLOW_LATEST_ROLLBACK=true "$ROOT/scripts/publish-update-manifest.sh" || bad "post-refresh re-publication failed"

printf '== a manifest from another release cannot be published ==\n'
$JQ -S '.version = "9.9.9"' "$UPD" >"$WORK/wrong.json" && cp "$WORK/wrong.json" "$UPD"
if run "$ROOT/scripts/publish-update-manifest.sh"; then
    bad "a manifest for a different version was published"
else
    note "version mismatch between the manifest and the release is rejected"
fi

printf '== operator key tool ==\n'
KEYDIR="$WORK/keys"; mkdir -p "$KEYDIR"
"$ROOT/scripts/generate-update-signing-key.sh" lightning-release-tool "$KEYDIR" >"$WORK/keytool.log" 2>&1 \
    && note "key generation succeeded" || bad "key generation failed"
GENKEY="$KEYDIR/lightning-release-tool.private.pem"
[[ -f "$GENKEY" ]] && note "private key written to a file" || bad "no key file"
[[ "$(stat -c '%a' "$GENKEY")" == 600 ]] && note "private key file is 0600" || bad "key file mode is $(stat -c '%a' "$GENKEY")"
grep -q 'BEGIN PRIVATE KEY' "$WORK/keytool.log" && bad "the tool printed the private key" || note "the tool never prints the private key"
grep -qF "$(grep -v -- '-----' "$GENKEY" | tr -d '\n')" "$WORK/keytool.log" && bad "key body leaked to stdout" || note "no key body on stdout"
PUB="$(openssl pkey -in "$GENKEY" -pubout -outform DER 2>/dev/null | tail -c 32 | openssl base64 -A)"
grep -qF "$PUB" "$WORK/keytool.log" && note "the raw base64 public key is printed" || bad "public key not printed"
grep -q 'UPDATE_SIGNING_KEY_B64' "$WORK/keytool.log" && note "CI variable instructions printed" || bad "no CI instructions"
if "$ROOT/scripts/generate-update-signing-key.sh" lightning-release-tool "$KEYDIR" >/dev/null 2>&1; then
    bad "the tool overwrote an existing key"
else
    note "the tool refuses to overwrite an existing key"
fi

# =============================================================================
# GitHub bandwidth mirror (MIRROR-SPEC §6, §7 "Deploy side")
#
# GitLab stays the release authority and the canonical binary source; GitHub
# holds byte-identical copies of the SAME published artifacts, and the signed
# manifest points at both. Everything below is exercised against the stateful
# mock, which models the GitHub tag/release/upload endpoints and the anonymous
# asset download separately from the GitLab ones.
# =============================================================================

printf '== mirror disabled: no mirror_url, and mirroring is a clean no-op ==\n'
setup
publish_and_verify || bad "publish/verify failed"
run "$ROOT/scripts/generate-update-manifest.sh" || bad "generation failed with mirroring off"
[[ "$($JQ '[.artifacts[] | select(has("mirror_url"))] | length' "$UPD")" == 0 ]] \
    && note "no artifact carries mirror_url when mirroring is disabled" \
    || bad "mirror_url emitted while mirroring is disabled"
grep -qi 'github' "$UPD" && bad "the manifest mentions github with mirroring disabled" \
    || note "the disabled manifest names no mirror at all"
[[ "$($JQ -r '.schema' "$UPD")" == 1 ]] && note "schema stays 1" || bad "schema changed"
: >"$MLOG"
run "$ROOT/scripts/mirror-release-to-github.sh" || bad "the mirror job failed when unconfigured"
grep -q 'github.com' "$MLOG" && bad "the disabled mirror job still contacted GitHub" \
    || note "the disabled mirror job makes no GitHub request"
[[ -f "$TR/dist/github-mirror.json" ]] && bad "a disabled mirror wrote a mirror record" \
    || note "no mirror record is written when disabled"

printf '== mirror enabled: deterministic, version-specific mirror_url ==\n'
export GITHUB_MIRROR_REPO="$MIRROR_REPO"
run "$ROOT/scripts/generate-update-manifest.sh" || bad "generation failed with mirroring on"
mirror_keys=(linux-appimage linux-deb linux-rpm windows-msi windows-portable windows-setup)
for k in "${mirror_keys[@]}"; do
    fn="$($JQ -r --arg k "$k" '.artifacts[$k].filename' "$UPD")"
    mu="$($JQ -r --arg k "$k" '.artifacts[$k].mirror_url' "$UPD")"
    [[ "$mu" == "https://github.com/${MIRROR_REPO}/releases/download/v${VER}/${fn}" ]] \
        && note "$k mirror_url is the deterministic version-specific asset URL" \
        || bad "$k mirror_url is $mu"
    # The canonical GitLab URL stays required and stays the fallback.
    [[ "$($JQ -r --arg k "$k" '.artifacts[$k].url' "$UPD")" == https://gitlab.example/* ]] \
        && note "$k keeps its canonical GitLab url" || bad "$k canonical url changed"
done
grep -q 'releases/latest/download' "$UPD" \
    && bad "a mutable /releases/latest/download URL was emitted" \
    || note "no /releases/latest/download URL anywhere in the manifest"
[[ "$($JQ -r '.schema' "$UPD")" == 1 ]] && note "adding mirror_url did not bump the schema" || bad "schema bumped"
cp "$UPD" "$WORK/mirror-first.json"
run "$ROOT/scripts/generate-update-manifest.sh" || bad "second mirrored generation failed"
cmp -s "$WORK/mirror-first.json" "$UPD" \
    && note "two mirrored runs from the same inputs are byte-identical" \
    || bad "mirrored generation is not deterministic"

printf '== a mirror repository that could steer a URL is rejected ==\n'
for badrepo in 'https://evil.example/x' 'owner/repo/extra' 'owner' '/repo' 'ow ner/repo' \
               'user:pass@host/repo'; do
    if run env GITHUB_MIRROR_REPO="$badrepo" "$ROOT/scripts/generate-update-manifest.sh"; then
        bad "GITHUB_MIRROR_REPO='$badrepo' was accepted"
    else
        note "GITHUB_MIRROR_REPO='$badrepo' is rejected"
    fi
done
run "$ROOT/scripts/generate-update-manifest.sh" || bad "regeneration failed"
run "$ROOT/scripts/sign-update-manifest.sh" || bad "signing the mirrored manifest failed"

printf '== a signed manifest promising mirrors is never silently unmirrored ==\n'
if run env -u GITHUB_MIRROR_REPO "$ROOT/scripts/mirror-release-to-github.sh"; then
    bad "mirroring no-opped while the signed manifest carried mirror_url"
else
    note "an unconfigured mirror refuses when the manifest already promises one"
fi
if run env -u GITHUB_MIRROR_TOKEN "$ROOT/scripts/mirror-release-to-github.sh"; then
    bad "mirroring ran without a token"
else
    note "a configured repository with no token is a hard failure, not a skip"
fi

printf '== the mirrored tag must exist and be the released commit ==\n'
export GITHUB_MIRROR_TOKEN="$MIRROR_TOKEN"
if run env MOCK_GITHUB_TAG_MISSING=true GITHUB_MIRROR_TAG_WAIT_SECONDS=0 \
        "$ROOT/scripts/mirror-release-to-github.sh"; then
    bad "mirrored a release whose tag is not on GitHub"
else
    note "an unmirrored tag fails after the bounded wait"
    grep -q 'did not appear' "$TR/out.log" \
        && note "the timeout message names the push mirror" || bad "unclear timeout message"
fi
[[ ! -f "$MSTATE/github/release_created" ]] \
    && note "no GitHub release was created for a missing tag" || bad "a release was created anyway"
if run env MOCK_GITHUB_TAG_COMMIT=1111111111111111111111111111111111111111 \
        "$ROOT/scripts/mirror-release-to-github.sh"; then
    bad "mirrored a tag pointing at a different commit"
else
    note "a tag peeling to another commit is a hard failure"
    grep -q 'refusing to mirror a different commit' "$TR/out.log" \
        && note "the mismatch message is explicit" || bad "unclear commit-mismatch message"
fi
[[ ! -f "$MSTATE/github/release_created" ]] \
    && note "no GitHub release was created for a mismatched tag" || bad "a release was created anyway"
# Only "not there yet" is retryable. A rejected credential must fail at once,
# not sit through the whole tag wait: the lookup runs in a command substitution,
# where a hard failure and an absent tag are otherwise indistinguishable.
poll_start=$SECONDS
if run env MOCK_GITHUB_TAG_UNAUTHORIZED=true \
        GITHUB_MIRROR_TAG_WAIT_SECONDS=90 GITHUB_MIRROR_TAG_POLL_SECONDS=30 \
        "$ROOT/scripts/mirror-release-to-github.sh"; then
    bad "a rejected GitHub credential was treated as success"
else
    note "a rejected GitHub credential is a hard failure"
fi
(( SECONDS - poll_start < 20 )) \
    && note "a non-404 tag lookup failure does not enter the retry wait" \
    || bad "a hard tag-lookup failure was retried as though the tag were absent"

printf '== mirror upload and anonymous verification ==\n'
: >"$MLOG"; rm -f "$MLOG.auth"
run "$ROOT/scripts/mirror-release-to-github.sh" || bad "mirroring failed"
GHA="$MSTATE/github/assets"
REC="$TR/dist/github-mirror.json"
expected_count="$($JQ '.entries | length' "$TR/dist/manifest.json")"
[[ "$(find "$GHA" -type f | wc -l)" == "$expected_count" ]] \
    && note "every publication-manifest artifact was mirrored ($expected_count)" \
    || bad "mirrored asset count is $(find "$GHA" -type f | wc -l), expected $expected_count"
while IFS= read -r row; do
    fn="$($JQ -r '.filename' <<<"$row")"
    lp="$($JQ -r '.local_path' <<<"$row")"
    cmp -s "$TR/$lp" "$GHA/$fn" || bad "mirrored $fn is not byte-identical to the published file"
done < <($JQ -c '.entries[]' "$TR/dist/manifest.json")
note "mirrored bytes are identical to the published dist/ files"
[[ -f "$REC" ]] && note "a mirror record was written" || bad "no dist/github-mirror.json"
[[ "$($JQ -r '.repository' "$REC")" == "$MIRROR_REPO" && "$($JQ -r '.tag' "$REC")" == "v$VER" ]] \
    && note "the record names the repository and tag" || bad "mirror record metadata"
[[ "$($JQ '.assets | length' "$REC")" == "$expected_count" && "$($JQ -r '.uploaded' "$REC")" == "$expected_count" ]] \
    && note "the record accounts for every uploaded asset" || bad "mirror record asset accounting"
$JQ -e '(.assets | map(select(.mirror_url | startswith("https://github.com/"))) | length) == (.assets | length)' "$REC" >/dev/null \
    && note "every recorded mirror URL is an https github.com asset URL" || bad "mirror record URLs"
grep -qF "$MIRROR_TOKEN" "$REC" && bad "the token reached the mirror record" || note "no token in the mirror record"

# The verification download is what a client will do: no credential at all.
mapfile -t dl_lines < <(grep '^GET https://github.com/.*/releases/download/' "$MLOG.auth" || true)
[[ "${#dl_lines[@]}" == "$expected_count" ]] \
    && note "every asset was re-downloaded for verification" \
    || bad "expected $expected_count verification downloads, saw ${#dl_lines[@]}"
anon_ok=1
for line in "${dl_lines[@]}"; do [[ "$line" == *" ANON" ]] || anon_ok=0; done
[[ "$anon_ok" == 1 ]] && note "every verification download was anonymous (no token)" \
    || bad "a verification download carried a credential"
grep -q '^GET https://api.github.com/.* AUTH$' "$MLOG.auth" \
    && note "GitHub API requests are authenticated (via a credential file, not argv)" \
    || bad "GitHub API requests were not authenticated"

printf '== the mirror token never reaches a log ==\n'
no_leak "$TR/all.log"
grep -qF "$MIRROR_TOKEN" "$MLOG" && bad "the token reached the request log" \
    || note "no token in the request URL log"
grep -qF "$MIRROR_TOKEN" "$MLOG.auth" && bad "the token reached the auth log" \
    || note "no token in the auth log"
[[ "$($JQ -r '.assets[0].mirror_url' "$REC")" != *"$MIRROR_TOKEN"* ]] \
    && note "no token in any published URL" || bad "token in a URL"

printf '== a manifest refresh never touches the release mirror ==\n'
: >"$MLOG"
run env MOCK_GITHUB_TAG_UNAUTHORIZED=true UPDATE_REFRESH_LATEST_ONLY=true "$ROOT/scripts/mirror-release-to-github.sh" \
    && note "the release mirror steps aside for a manifest refresh, dead token and all" || bad "refresh blocked by the release mirror"
[[ "$(grep -c 'github' "$MLOG")" == 0 ]] && note "the refresh made no GitHub request" || bad "refresh contacted GitHub"

printf '== re-running the mirror is idempotent ==\n'
: >"$MLOG"; rm -f "$MLOG.auth"
run "$ROOT/scripts/mirror-release-to-github.sh" || bad "idempotent re-run failed"
[[ "$(grep -c '^POST https://uploads.github.com' "$MLOG" || true)" == 0 ]] \
    && note "an identical re-run uploads nothing" || bad "the re-run re-uploaded assets"
[[ "$(grep -c '^POST https://api.github.com/repos/.*/releases$' "$MLOG" || true)" == 0 ]] \
    && note "an identical re-run does not recreate the release" || bad "the re-run created a second release"
[[ "$($JQ -r '.uploaded' "$REC")" == 0 && "$($JQ -r '.already_present' "$REC")" == "$expected_count" ]] \
    && note "already-present byte-identical assets are accepted" || bad "idempotent accounting"

printf '== an already-present DIFFERING asset is a hard failure ==\n'
victim="$($JQ -r '.entries[0].filename' "$TR/dist/manifest.json")"
cp "$GHA/$victim" "$WORK/mirror-victim.bin"
vsize="$(wc -c <"$WORK/mirror-victim.bin" | tr -d ' ')"
# (a) same size, different bytes: only the anonymous SHA-256 read-back catches it.
head -c "$vsize" /dev/zero | tr '\0' 'X' >"$GHA/$victim"
cmp -s "$WORK/mirror-victim.bin" "$GHA/$victim" && bad "same-size tamper fixture did not change the bytes" || true
[[ "$(wc -c <"$GHA/$victim" | tr -d ' ')" == "$vsize" ]] || bad "same-size tamper changed the size"
if run "$ROOT/scripts/mirror-release-to-github.sh"; then
    bad "a mirrored asset with different bytes was accepted"
else
    note "a re-downloaded asset whose bytes differ fails the job"
    grep -q 'does not match the published SHA-256' "$TR/out.log" \
        && note "the failure names the SHA-256 mismatch" || bad "unclear byte-mismatch message"
fi
[[ "$(grep -c '^POST https://uploads.github.com' "$MLOG" || true)" == 0 ]] \
    && note "the differing asset was never overwritten" || bad "the job overwrote a published asset"
# (b) different size: caught before a single byte is uploaded.
printf 'extra' >>"$GHA/$victim"
: >"$MLOG"
if run "$ROOT/scripts/mirror-release-to-github.sh"; then
    bad "a mirrored asset with a different size was accepted"
else
    note "an asset already present with a different size is a hard failure"
    grep -q 'refusing to overwrite a published asset' "$TR/out.log" \
        && note "the failure says it refuses to overwrite" || bad "unclear size-mismatch message"
fi
[[ "$(grep -c '^POST https://uploads.github.com' "$MLOG" || true)" == 0 ]] \
    && note "nothing was uploaded once the conflict was detected" || bad "an upload happened after a conflict"
cp "$WORK/mirror-victim.bin" "$GHA/$victim"
run "$ROOT/scripts/mirror-release-to-github.sh" || bad "the mirror did not converge after restoring the asset"

printf '== a partially uploaded asset is never treated as present ==\n'
if run env MOCK_GITHUB_ASSET_STATE=starter "$ROOT/scripts/mirror-release-to-github.sh"; then
    bad "an asset stuck in the starter state was accepted"
else
    note "an asset that is not in the uploaded state is a hard failure"
fi

printf '== a local file that is not the published bytes is refused ==\n'
deb="$TR/dist/lightning_${VER}_amd64.deb"
cp "$deb" "$WORK/deb.held"
printf 'not-the-published-bytes\n' >"$deb"
if run "$ROOT/scripts/mirror-release-to-github.sh"; then
    bad "mirrored bytes that were never published"
else
    note "a local file disagreeing with the publication manifest is refused"
fi
cp "$WORK/deb.held" "$deb"

printf '== mirroring requires a verified publication ==\n'
mv "$TR/dist/verification.json" "$WORK/mirror-verification.held.json"
if run "$ROOT/scripts/mirror-release-to-github.sh"; then
    bad "mirrored without a verification record"
else
    note "mirroring refuses without dist/verification.json"
fi
mv "$WORK/mirror-verification.held.json" "$TR/dist/verification.json"

# --- The mirror token preflight -----------------------------------------------
printf '== the mirror token is proved usable before anything is published ==\n'
setup
export GITHUB_MIRROR_REPO="$MIRROR_REPO" GITHUB_MIRROR_TOKEN="$MIRROR_TOKEN"
run "$ROOT/scripts/github-mirror-preflight.sh" && note "a usable token passes the preflight" || bad "preflight refused a usable token"
[[ "$($JQ -r '.enabled' "$TR/dist/github-mirror-preflight.json")" == true ]] && note "preflight record written" || bad "preflight record"
if run env MOCK_GITHUB_TAG_UNAUTHORIZED=true "$ROOT/scripts/github-mirror-preflight.sh"; then
    bad "a dead token passed the preflight"
else
    grep -q 'expired or been revoked' "$TR/out.log" && note "a dead token is refused with the reason" || bad "dead-token reason missing"
fi
if run env MOCK_GITHUB_CAN_PUSH=false "$ROOT/scripts/github-mirror-preflight.sh"; then
    bad "a read-only token passed the preflight"
else
    grep -q 'cannot write to it' "$TR/out.log" && note "a token that cannot write is refused by name" || bad "read-only refusal reason missing"
fi
run env MOCK_GITHUB_NO_PERMISSIONS=true "$ROOT/scripts/github-mirror-preflight.sh" || bad "an answer without a permissions object blocked publication"
grep -q 'no permissions object' "$TR/out.log" && note "an absent permissions object warns rather than refusing" || bad "absent-permissions warning missing"
run env MOCK_GITHUB_TOKEN_EXPIRES='garbage-value' "$ROOT/scripts/github-mirror-preflight.sh" || bad "an unparseable expiry blocked publication"
grep -q 'could not parse' "$TR/out.log" && note "an unparseable expiry warns rather than refusing" || bad "unparseable-expiry warning missing"
grep -q 'WARNING: the GitHub update slot does not serve' "$TR/out.log" && note "an absent update slot is announced, not fatal" || bad "empty-slot warning missing"
far="$(date -u -d '+200 days' '+%Y-%m-%d %H:%M:%S UTC')"
soon="$(date -u -d '+10 days' '+%Y-%m-%d %H:%M:%S UTC')"
dying="$(date -u -d '+1 day' '+%Y-%m-%d %H:%M:%S UTC')"
run env MOCK_GITHUB_TOKEN_EXPIRES="$far" "$ROOT/scripts/github-mirror-preflight.sh" || bad "a token with 200 days left was refused"
[[ "$($JQ -r '.days_left' "$TR/dist/github-mirror-preflight.json")" -ge 199 ]] && note "days left are recorded" || bad "days_left"
run env MOCK_GITHUB_TOKEN_EXPIRES="$soon" "$ROOT/scripts/github-mirror-preflight.sh" || bad "a token with 10 days left was refused"
grep -q 'WARNING: GITHUB_MIRROR_TOKEN expires in' "$TR/out.log" && note "an expiring token warns" || bad "no expiry warning"
if run env MOCK_GITHUB_TOKEN_EXPIRES="$dying" "$ROOT/scripts/github-mirror-preflight.sh"; then
    bad "a token dying tomorrow passed the preflight"
else
    note "a token dying tomorrow is refused before publication"
fi
# A manifest refresh needs nothing from GitHub: with a DEAD token the
# preflight and the release mirror both step aside, and the slot job (best
# effort) is the only one that would fail.
: >"$MLOG"
run env MOCK_GITHUB_TAG_UNAUTHORIZED=true UPDATE_REFRESH_LATEST_ONLY=true "$ROOT/scripts/github-mirror-preflight.sh" \
    && note "a manifest refresh skips the preflight even with a dead token" || bad "refresh blocked by the preflight"
[[ "$(grep -c '^GET https://api.github.com' "$MLOG")" == 0 ]] && note "the skipped preflight contacts GitHub not at all" || bad "refresh preflight contacted GitHub"
no_leak "$TR/all.log"
unset GITHUB_MIRROR_REPO GITHUB_MIRROR_TOKEN
run "$ROOT/scripts/github-mirror-preflight.sh" && note "preflight is a no-op without a mirror" || bad "preflight without a mirror"

# --- The update slot on GitHub ---------------------------------------------------
printf '== the signed manifest pair is mirrored to the fixed update-latest slot ==\n'
setup
export GITHUB_MIRROR_REPO="$MIRROR_REPO" GITHUB_MIRROR_TOKEN="$MIRROR_TOKEN"
publish_and_verify || bad "publish/verify for the slot case failed"
run "$ROOT/scripts/generate-update-manifest.sh" && run "$ROOT/scripts/sign-update-manifest.sh" || bad "generate+sign for the slot case"
if run "$ROOT/scripts/mirror-update-manifest-to-github.sh"; then
    bad "the update slot was written BEFORE GitLab promoted latest (no publication record)"
else
    grep -q 'has not been promoted' "$TR/out.log" && note "without GitLab's promotion record the slot is refused by name" || bad "promotion-record refusal reason missing"
fi
run "$ROOT/scripts/mirror-release-to-github.sh" || bad "release mirror for the slot case failed"
run "$ROOT/scripts/publish-update-manifest.sh" || bad "GitLab promotion for the slot case failed"
: >"$MLOG"
run "$ROOT/scripts/mirror-update-manifest-to-github.sh" || bad "update slot publication failed"
SLOT="$MSTATE/github/assets-update-latest"
[[ -f "$SLOT/$MANIFEST_NAME" && -f "$SLOT/$SIG_NAME" ]] && note "both files are in the update slot" || bad "slot assets missing"
cmp -s "$SLOT/$MANIFEST_NAME" "$UPD" && note "the slot carries the promoted manifest bytes" || bad "slot manifest differs"
grep -q "^GET https://github.com/$MIRROR_REPO/releases/download/update-latest/$MANIFEST_NAME" "$MLOG" \
    && note "the slot was read back at the client's compiled-in fallback URL" || bad "no anonymous read-back of the slot"
grep -q "ANON$" "${MLOG}.auth" && note "the read-back carried no token" || bad "read-back was authenticated"
[[ -f "$MSTATE/github/release_created_update-latest" ]] && note "the update-latest release was created" || bad "update-latest release missing"
[[ "$($JQ -r '.manifest_sha256' "$TR/dist/github-update-manifest-mirror.json")" == "$(sha256sum "$UPD" | cut -d' ' -f1)" ]] \
    && note "the slot record names the promoted bytes" || bad "slot record"
# Once the slot exists the preflight sees it served.
run "$ROOT/scripts/github-mirror-preflight.sh" || bad "preflight after slot publication failed"
grep -q "GitHub update slot serves $MANIFEST_NAME" "$TR/out.log" && note "the preflight sees the populated slot" || bad "preflight did not see the slot"
# Idempotent: identical bytes upload nothing.
: >"$MLOG"
run "$ROOT/scripts/mirror-update-manifest-to-github.sh" || bad "slot re-run failed"
[[ "$(grep -c 'POST .*assets?name=' "$MLOG")" == 0 ]] && note "identical bytes are not re-uploaded" || bad "slot re-run re-uploaded"
# A NEWER promotion replaces both assets, signature first.
run env UPDATE_EXPIRES_AT=2027-09-01T00:00:00Z "$ROOT/scripts/generate-update-manifest.sh" \
    && run "$ROOT/scripts/sign-update-manifest.sh" || bad "regenerate for slot replacement"
run env UPDATE_REFRESH_LATEST_ONLY=true "$ROOT/scripts/publish-update-manifest.sh" || bad "refresh promotion for slot replacement"
: >"$MLOG"
run "$ROOT/scripts/mirror-update-manifest-to-github.sh" || bad "slot replacement failed"
cmp -s "$SLOT/$MANIFEST_NAME" "$UPD" && note "the slot now carries the refreshed manifest" || bad "slot not replaced"
[[ "$(grep -c '^DELETE .*/releases/assets/' "$MLOG")" == 2 ]] && note "both previous assets were removed first" || bad "delete count $(grep -c '^DELETE ' "$MLOG")"
sig_line="$(grep -n "POST .*assets?name=${SIG_NAME}\$" "$MLOG" | head -1 | cut -d: -f1)"
man_line="$(grep -n "POST .*assets?name=${MANIFEST_NAME}\$" "$MLOG" | head -1 | cut -d: -f1)"
[[ -n "$sig_line" && -n "$man_line" && "$sig_line" -lt "$man_line" ]] && note "signature is replaced before the manifest" || bad "replacement order"
# Tampered local bytes never reach the slot.
cp "$UPD" "$WORK/slot-good.json"
$JQ -S '.version = "9.9.9"' "$UPD" >"$WORK/slot-bad.json" && cp "$WORK/slot-bad.json" "$UPD"
if run "$ROOT/scripts/mirror-update-manifest-to-github.sh"; then
    bad "a manifest that GitLab did not promote reached the slot"
else
    note "a manifest that GitLab did not promote is refused"
fi
cp "$WORK/slot-good.json" "$UPD"
no_leak "$TR/all.log"
unset GITHUB_MIRROR_REPO GITHUB_MIRROR_TOKEN
run "$ROOT/scripts/mirror-update-manifest-to-github.sh" && note "the slot job is a no-op without a mirror" || bad "slot job without a mirror"

printf '== the signed manifest and the mirror job must agree on every URL ==\n'
$JQ -S '.artifacts["linux-deb"].mirror_url = "https://github.com/Someone/else/releases/download/v9.9.9/x.deb"' \
    "$UPD" >"$WORK/mirror-divergent.json"
cp "$UPD" "$WORK/mirror-good.json"
cp "$WORK/mirror-divergent.json" "$UPD"
if run "$ROOT/scripts/mirror-release-to-github.sh"; then
    bad "a manifest pointing at a different mirror was accepted"
else
    note "a mirror_url the job would not create is a hard failure"
fi
cp "$WORK/mirror-good.json" "$UPD"
unset GITHUB_MIRROR_REPO GITHUB_MIRROR_TOKEN

if [[ "$fail" == 0 ]]; then
    printf 'Update manifest tests passed\n'
else
    printf 'Update manifest tests FAILED\n' >&2
    exit 1
fi
