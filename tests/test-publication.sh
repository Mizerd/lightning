#!/usr/bin/env bash
set -Eeuo pipefail

# End-to-end tests of the publication, verification, and release scripts against
# a stateful mock GitLab API. Covers the release-request gate, manifest build,
# idempotent uploads, immutability conflicts, partial-upload rollback+retry,
# registry verification, and both release actions (attach-existing and create).

ROOT="$(git rev-parse --show-toplevel)"
JQ="$(command -v jq)"
WORK="$(mktemp -d)"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

SHA=86d30b41457b1c9fd710a646326c9d1c71a52b76
VER=0.6.1
fail=0
note() { printf '  ok: %s\n' "$1"; }
bad() { printf '  FAIL: %s\n' "$1" >&2; fail=1; }

# validate-release-request.sh also gates the update-signing key triple (the
# public half is compiled into every package, so a mismatch must fail before any
# build job runs). The publishing cases below therefore need a consistent triple
# to reach their own assertions. Generated fresh at run time; nothing is
# committed, and the gate's own behaviour lives in tests/test-update-manifest.sh.
command -v openssl >/dev/null 2>&1 || { printf 'error: openssl is required\n' >&2; exit 1; }
SIGN_KEY="$WORK/update-signing.pem"
openssl genpkey -algorithm ed25519 -out "$SIGN_KEY" 2>/dev/null
chmod 600 "$SIGN_KEY"
SIGN_KEY_B64="$(openssl base64 -A -in "$SIGN_KEY")"
SIGN_PUB_B64="$(openssl pkey -in "$SIGN_KEY" -pubout -outform DER 2>/dev/null \
    | tail -c 32 | openssl base64 -A)"

# Fresh project dir with version.env and the two validated package files.
setup() {
    TR="$WORK/run.$RANDOM.$RANDOM"
    mkdir -p "$TR/dist"
    cat >"$TR/dist/version.env" <<EOF
BASE_VERSION=$VER
LOGICAL_VERSION=$VER
DEB_VERSION=$VER
RPM_VERSION=$VER
RPM_RELEASE=1
SOURCE_SHA=$SHA
SOURCE_REF=v$VER
IS_EXACT_TAG=true
PUBLISHING=true
EOF
    printf 'deb-bytes-%s\n' "$RANDOM" >"$TR/dist/lightning_${VER}_amd64.deb"
    printf 'rpm-bytes-%s\n' "$RANDOM" >"$TR/dist/lightning-${VER}-1.x86_64.rpm"
    printf 'flatpak-bytes-%s\n' "$RANDOM" >"$TR/dist/lightning_${VER}_amd64.flatpak"
    printf 'appimage-bytes-%s\n' "$RANDOM" >"$TR/dist/Lightning-${VER}-x86_64.AppImage"
    printf 'snap-bytes-%s\n' "$RANDOM" >"$TR/dist/lightning_${VER}_amd64.snap"
    # Windows artifacts (names carry the 7-char source SHA, as build-windows.sh
    # produces them) live under dist/windows/.
    mkdir -p "$TR/dist/windows"
    printf 'win-portable-%s\n' "$RANDOM" \
        >"$TR/dist/windows/Lightning-${VER}-${SHA:0:7}-windows-x86_64-portable.zip"
    printf 'win-msi-%s\n' "$RANDOM" \
        >"$TR/dist/windows/Lightning-${VER}-${SHA:0:7}-windows-x86_64.msi"
    printf 'win-setup-%s\n' "$RANDOM" \
        >"$TR/dist/windows/Lightning-${VER}-${SHA:0:7}-windows-x86_64-setup.exe"
    MSTATE="$TR/mockstate"; mkdir -p "$MSTATE"
    MLOG="$TR/curl.log"; : >"$MLOG"
    export CI_PROJECT_DIR="$TR"
    export CI_API_V4_URL=https://gitlab.example/api/v4
    export CI_JOB_TOKEN=mock-secret-value
    export TARGET_PROJECT_ID=6 LIGHTNING_PROJECT_ID=6 PACKAGE_NAME=lightning
    export PUBLISH_PACKAGES=true RELEASE_VERSION=$VER
    # Synthetic provider keys so the publish-time presence gate is satisfied.
    export GIPHY_API_KEY="SYNTH_GIPHY_pubtest" KLIPY_API_KEY="SYNTH_KLIPY_pubtest"
    # ...and a consistent update-signing triple, for the same reason.
    export UPDATE_SIGNING_KEY_ID=lightning-release-2026a
    export UPDATE_SIGNING_KEY_B64="$SIGN_KEY_B64"
    export UPDATE_SIGNING_PUBKEY_2026A="$SIGN_PUB_B64"
    export CURL_BIN="$ROOT/tests/mock-curl.sh"
    export MOCK_STATE_DIR="$MSTATE" MOCK_CURL_LOG="$MLOG"
    export MOCK_SOURCE_SHA="$SHA" MOCK_RELEASE_VERSION="$VER" MOCK_JQ="$JQ"
    unset CI MOCK_TAG_EXISTS MOCK_RELEASE_EXISTS MOCK_TAG_SHA MOCK_CONFLICT_FILE \
          MOCK_FAIL_UPLOAD MOCK_PRESEED_LINKS MOCK_CREATE_LINKS MOCK_COMMIT_REACHABLE \
          RELEASE_NOTES_B64 SOURCE_REF PUBLISH_API_BASE
}
no_leak() { grep -q 'mock-secret-value' "$1" && bad "credential leaked in $1" || true; }
run() { "$@" >"$TR/out.log" 2>&1; local rc=$?; no_leak "$TR/out.log"; return $rc; }

printf '== release-request gate ==\n'
setup; export RELEASE_ACTION=attach-existing SOURCE_REF=v$VER
if PUBLISH_PACKAGES=false SOURCE_REF=main run "$ROOT/scripts/validate-release-request.sh"; then note "build-only accepted"; else bad "build-only rejected"; fi
setup; export SOURCE_REF=v$VER
if run env RELEASE_ACTION=bogus "$ROOT/scripts/validate-release-request.sh"; then bad "invalid action accepted"; else note "invalid RELEASE_ACTION rejected"; fi
setup; export RELEASE_ACTION=attach-existing SOURCE_REF=v$VER
if run env -u RELEASE_VERSION "$ROOT/scripts/validate-release-request.sh"; then bad "missing RELEASE_VERSION accepted"; else note "missing RELEASE_VERSION rejected"; fi
setup; export RELEASE_ACTION=create
if run env SOURCE_REF=main "$ROOT/scripts/validate-release-request.sh"; then bad "create with branch ref accepted"; else note "create requires full SHA"; fi
setup; export RELEASE_ACTION=create SOURCE_REF=$SHA
if run "$ROOT/scripts/validate-release-request.sh"; then note "create with SHA accepted"; else bad "create with SHA rejected"; fi

printf '== manifest ==\n'
setup; export RELEASE_ACTION=attach-existing SOURCE_REF=v$VER
run "$ROOT/scripts/write-manifest.sh" || bad "manifest build failed"
M="$TR/dist/manifest.json"
[[ "$($JQ '.entries|length' "$M")" == 9 ]] && note "manifest has 9 entries" || bad "manifest entry count"
[[ "$($JQ -r '.entries[0].filename' "$M")" == "lightning_${VER}_amd64.deb" ]] && note "deb filename" || bad "deb filename"
[[ "$($JQ -r '.entries[1].filename' "$M")" == "lightning-${VER}-1.x86_64.rpm" ]] && note "rpm filename" || bad "rpm filename"
[[ "$($JQ -r '.entries[2].filename' "$M")" == "lightning_${VER}_amd64.flatpak" ]] && note "flatpak filename" || bad "flatpak filename"
[[ "$($JQ -r '.entries[3].filename' "$M")" == "Lightning-${VER}-x86_64.AppImage" ]] && note "appimage filename" || bad "appimage filename"
[[ "$($JQ -r '.entries[4].filename' "$M")" == "lightning_${VER}_amd64.snap" ]] && note "snap filename" || bad "snap filename"
# Registry layout: every file shares one package name + version (extensible).
[[ "$($JQ -r '[.entries[].version]|unique|length' "$M")" == 1 ]] && note "single version layout" || bad "version layout"
[[ "$($JQ -r '.entries[]|.asset_path' "$M" | grep -c "^/packages/${VER}/")" == 9 ]] && note "asset paths under /packages/<version>/" || bad "asset paths"
# Full metadata present on each entry (extension-ready schema).
[[ "$($JQ -r '[.entries[]|select(.sha256 and .size and .architecture and .source_sha and .registry_url and .asset_name)]|length' "$M")" == 9 ]] && note "entries carry full metadata" || bad "entry metadata"

printf '== publish ==\n'
setup; export RELEASE_ACTION=attach-existing SOURCE_REF=v$VER
run "$ROOT/scripts/write-manifest.sh" && run "$ROOT/scripts/publish-packages.sh" || bad "publish failed"
[[ "$(grep -c '^PUT .*/packages/generic/lightning/' "$MLOG")" == 9 ]] && note "9 uploads" || bad "upload count"
[[ "$($JQ '.entries|length' "$TR/dist/publication.json")" == 9 ]] && note "publication.json" || bad "publication.json"
# only the five package files are ever PUT (no logs/metadata)
[[ "$(grep -c '^PUT ' "$MLOG")" == 9 ]] && note "only manifest files uploaded" || bad "extra uploads"

printf '== internal API base override (PUBLISH_API_BASE) ==\n'
setup; export RELEASE_ACTION=attach-existing SOURCE_REF=v$VER
export PUBLISH_API_BASE=https://internal.example/api/v4
run "$ROOT/scripts/write-manifest.sh" && run "$ROOT/scripts/publish-packages.sh" || bad "publish with API base override failed"
MO="$TR/dist/manifest.json"
[[ "$($JQ -r '[.entries[].registry_url] | all(startswith("https://gitlab.example/api/v4/"))' "$MO")" == true ]] \
    && note "manifest registry URLs stay canonical/public" || bad "manifest URLs not canonical"
[[ "$(grep -c '^PUT https://internal.example/api/v4/' "$MLOG")" == 9 ]] \
    && note "uploads routed through the internal API base" || bad "uploads not routed internally"
[[ "$(grep -c '^PUT https://gitlab.example/' "$MLOG")" == 0 ]] \
    && note "no upload used the public host" || bad "upload leaked to the public host"
run "$ROOT/scripts/verify-published-packages.sh" && note "verify works via internal base" || bad "verify failed via internal base"
unset PUBLISH_API_BASE

printf '== idempotent retry (identical) ==\n'
# reuse state: run publish twice in same MOCK_STATE_DIR
: >"$MLOG"; run "$ROOT/scripts/publish-packages.sh" || bad "second publish failed"
[[ "$(grep -c '^PUT ' "$MLOG")" == 0 ]] && note "identical files not re-uploaded" || bad "re-uploaded identical"

printf '== immutable conflict ==\n'
setup; export RELEASE_ACTION=attach-existing SOURCE_REF=v$VER MOCK_CONFLICT_FILE="lightning_${VER}_amd64.deb"
run "$ROOT/scripts/write-manifest.sh"
if run "$ROOT/scripts/publish-packages.sh"; then bad "conflict accepted"; else note "different bytes rejected"; fi

printf '== checksum mismatch (validated bytes differ from manifest) ==\n'
setup; export RELEASE_ACTION=attach-existing SOURCE_REF=v$VER
run "$ROOT/scripts/write-manifest.sh"
printf 'tampered\n' >>"$TR/dist/lightning_${VER}_amd64.deb"
if run "$ROOT/scripts/publish-packages.sh"; then bad "tampered file accepted"; else note "manifest checksum enforced"; fi

printf '== partial upload rollback + retry ==\n'
setup; export RELEASE_ACTION=attach-existing SOURCE_REF=v$VER MOCK_FAIL_UPLOAD="lightning-${VER}-1.x86_64.rpm"
run "$ROOT/scripts/write-manifest.sh"
if run "$ROOT/scripts/publish-packages.sh"; then bad "partial upload succeeded"; else note "partial upload failed"; fi
[[ "$(grep -c '^DELETE .*package_files/' "$MLOG")" -ge 1 ]] && note "rollback deleted uploaded deb" || bad "no rollback"
[[ -z "$(ls -A "$MSTATE/registry" 2>/dev/null)" ]] && note "registry clean after rollback" || bad "registry not clean"
# retry now succeeds
unset MOCK_FAIL_UPLOAD; : >"$MLOG"
run "$ROOT/scripts/publish-packages.sh" && note "retry published both" || bad "retry failed"
[[ "$(grep -c '^PUT ' "$MLOG")" == 9 ]] && note "retry uploaded all files" || bad "retry upload count"

printf '== verify published ==\n'
setup; export RELEASE_ACTION=attach-existing SOURCE_REF=v$VER
run "$ROOT/scripts/write-manifest.sh" && run "$ROOT/scripts/publish-packages.sh"
run "$ROOT/scripts/verify-published-packages.sh" && note "verify passed" || bad "verify failed"
[[ -f "$TR/dist/verification.json" ]] && note "verification.json written" || bad "no verification.json"
# unexpected extra file present -> fail
cp "$TR/dist/lightning_${VER}_amd64.deb" "$MSTATE/registry/stray-file.txt"
if run "$ROOT/scripts/verify-published-packages.sh"; then bad "extra registry file accepted"; else note "unexpected registry file rejected"; fi
# tampered stored file -> checksum fail
rm -f "$MSTATE/registry/stray-file.txt"; printf 'x' >>"$MSTATE/registry/lightning_${VER}_amd64.deb"
if run "$ROOT/scripts/verify-published-packages.sh"; then bad "tampered stored file accepted"; else note "stored checksum mismatch rejected"; fi

printf '== finalize attach-existing ==\n'
setup; export RELEASE_ACTION=attach-existing SOURCE_REF=v$VER MOCK_TAG_EXISTS=true MOCK_RELEASE_EXISTS=true
run "$ROOT/scripts/write-manifest.sh" && run "$ROOT/scripts/publish-packages.sh" && run "$ROOT/scripts/verify-published-packages.sh"
: >"$MLOG"
run "$ROOT/scripts/finalize-release.sh" && note "attach-existing succeeded" || bad "attach-existing failed"
[[ "$(grep -c '^POST .*/assets/links' "$MLOG")" == 9 ]] && note "9 links created" || bad "link count"
# duplicate: rerun -> 0 new POSTs
: >"$MLOG"; run "$ROOT/scripts/finalize-release.sh" && note "rerun idempotent" || bad "rerun failed"
[[ "$(grep -c '^POST .*/assets/links' "$MLOG")" == 0 ]] && note "no duplicate links" || bad "duplicate links created"

printf '== attach-existing conflicting link ==\n'
setup; export RELEASE_ACTION=attach-existing SOURCE_REF=v$VER MOCK_TAG_EXISTS=true MOCK_RELEASE_EXISTS=true
run "$ROOT/scripts/write-manifest.sh" && run "$ROOT/scripts/publish-packages.sh" && run "$ROOT/scripts/verify-published-packages.sh"
export MOCK_PRESEED_LINKS="[{\"name\":\"Lightning ${VER} — Debian amd64\",\"url\":\"https://wrong.example/x\",\"link_type\":\"package\"}]"
printf '%s' "$MOCK_PRESEED_LINKS" >"$MSTATE/links.json"
if run "$ROOT/scripts/finalize-release.sh"; then bad "conflicting link accepted"; else note "conflicting link rejected"; fi

printf '== attach-existing missing release / moved tag ==\n'
setup; export RELEASE_ACTION=attach-existing SOURCE_REF=v$VER MOCK_TAG_EXISTS=true MOCK_RELEASE_EXISTS=false
run "$ROOT/scripts/write-manifest.sh" && run "$ROOT/scripts/publish-packages.sh" && run "$ROOT/scripts/verify-published-packages.sh"
if run "$ROOT/scripts/finalize-release.sh"; then bad "missing release accepted"; else note "missing release rejected"; fi
setup; export RELEASE_ACTION=attach-existing SOURCE_REF=v$VER MOCK_TAG_EXISTS=true MOCK_RELEASE_EXISTS=true MOCK_TAG_SHA=0000000000000000000000000000000000000000
run "$ROOT/scripts/write-manifest.sh" && run "$ROOT/scripts/publish-packages.sh" && run "$ROOT/scripts/verify-published-packages.sh"
if run "$ROOT/scripts/finalize-release.sh"; then bad "moved tag accepted"; else note "moved tag rejected"; fi

printf '== finalize create ==\n'
setup; export RELEASE_ACTION=create SOURCE_REF=$SHA MOCK_TAG_EXISTS=false MOCK_RELEASE_EXISTS=false MOCK_COMMIT_REACHABLE=true
export RELEASE_NOTES_B64="$(printf '## Lightning %s\nNotes.\n' "$VER" | base64 | tr -d '\n')"
run "$ROOT/scripts/write-manifest.sh" && run "$ROOT/scripts/publish-packages.sh" && run "$ROOT/scripts/verify-published-packages.sh"
export MOCK_CREATE_LINKS="$($JQ -c '[.entries[]|{name:.asset_name,url:.registry_url,link_type:"package"}]' "$TR/dist/manifest.json")"
: >"$MLOG"
run "$ROOT/scripts/finalize-release.sh" && note "create succeeded" || bad "create failed"
[[ "$(grep -c '^POST .*/releases$' "$MLOG")" == 1 ]] && note "release created via API" || bad "release POST"

printf '== create rejects existing tag/release ==\n'
setup; export RELEASE_ACTION=create SOURCE_REF=$SHA MOCK_TAG_EXISTS=true MOCK_RELEASE_EXISTS=false MOCK_COMMIT_REACHABLE=true RELEASE_NOTES_B64=$(printf x|base64)
run "$ROOT/scripts/write-manifest.sh" && run "$ROOT/scripts/publish-packages.sh" && run "$ROOT/scripts/verify-published-packages.sh"
if run "$ROOT/scripts/finalize-release.sh"; then bad "create with existing tag accepted"; else note "existing tag rejected"; fi
setup; export RELEASE_ACTION=create SOURCE_REF=$SHA MOCK_TAG_EXISTS=false MOCK_RELEASE_EXISTS=true MOCK_COMMIT_REACHABLE=true RELEASE_NOTES_B64=$(printf x|base64)
run "$ROOT/scripts/write-manifest.sh" && run "$ROOT/scripts/publish-packages.sh" && run "$ROOT/scripts/verify-published-packages.sh"
if run "$ROOT/scripts/finalize-release.sh"; then bad "create with existing release accepted"; else note "existing release rejected"; fi

printf '== create rejects unreachable commit and missing notes ==\n'
setup; export RELEASE_ACTION=create SOURCE_REF=$SHA MOCK_TAG_EXISTS=false MOCK_RELEASE_EXISTS=false MOCK_COMMIT_REACHABLE=false RELEASE_NOTES_B64=$(printf x|base64)
run "$ROOT/scripts/write-manifest.sh" && run "$ROOT/scripts/publish-packages.sh" && run "$ROOT/scripts/verify-published-packages.sh"
if run "$ROOT/scripts/finalize-release.sh"; then bad "unreachable commit accepted"; else note "unreachable commit rejected"; fi
setup; export RELEASE_ACTION=create SOURCE_REF=$SHA MOCK_TAG_EXISTS=false MOCK_RELEASE_EXISTS=false MOCK_COMMIT_REACHABLE=true
run "$ROOT/scripts/write-manifest.sh" && run "$ROOT/scripts/publish-packages.sh" && run "$ROOT/scripts/verify-published-packages.sh"
if run "$ROOT/scripts/finalize-release.sh"; then bad "missing notes accepted"; else note "missing notes rejected"; fi

if [[ "$fail" == 0 ]]; then printf 'Publication and release tests passed\n'; else printf 'Publication tests FAILED\n' >&2; exit 1; fi
