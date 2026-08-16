#!/usr/bin/env bash

# Shared constants and helpers for the signed update manifest.
#
# The update manifest is a SEPARATE artifact from dist/manifest.json. The latter
# is the publication manifest (what this pipeline uploads to project 6); this one
# is the small, signed document Lightning itself polls to learn that a newer
# release exists. Keeping the names in one place stops the three scripts that
# touch it (generate / sign / publish) from drifting apart.
#
# Do not enable command tracing in anything that sources this: the signing
# script handles private key material.
[[ -n "${BASH_VERSION:-}" ]] || { printf 'error: bash is required\n' >&2; exit 1; }

# File names are part of the client contract (UPDATE-SPEC §2, §6). Changing
# either one breaks every already-shipped Lightning build.
UPDATE_MANIFEST_NAME="update-manifest-v1.json"
# Consumed by the scripts that source this library.
# shellcheck disable=SC2034
UPDATE_SIG_NAME="${UPDATE_MANIFEST_NAME}.sig"

# Generic-package name for the update documents. Deliberately NOT the
# "lightning" package that holds the release artifacts: the update documents
# have their own version namespace, including the mutable "latest" slot, and
# must never be able to collide with, shadow, or be mistaken for a release
# package file.
UPDATE_PACKAGE_NAME="${UPDATE_PACKAGE_NAME:-lightning-update}"

# The stable slot Lightning polls. It is the ONE deliberately mutable location
# in this pipeline; every other published path is immutable.
# shellcheck disable=SC2034
UPDATE_LATEST_SLOT="latest"

# Signature envelope algorithm identifier (UPDATE-SPEC §2). Ed25519 only.
# shellcheck disable=SC2034
UPDATE_SIG_ALG="ed25519"

# Ed25519 SubjectPublicKeyInfo DER is exactly 44 bytes with this fixed 12-byte
# prefix, followed by the raw 32-byte public key. Used to prove a supplied key
# really is Ed25519 WITHOUT ever running `openssl pkey -text`, which would print
# private key material.
UPDATE_ED25519_SPKI_PREFIX="302a300506032b6570032100"
UPDATE_ED25519_SPKI_BYTES=44

# Key ids appear in the published envelope and are matched against a compiled-in
# table in Lightning. Restrict them to a boring, log-safe alphabet.
update_valid_key_id() {
    [[ "$1" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$ ]]
}

# ---------------------------------------------------------------------------
# GitHub bandwidth mirror (MIRROR-SPEC §6)
#
# GitLab stays the release authority and the canonical binary source. GitHub
# holds byte-identical copies of the SAME published artifacts so a client can
# fetch the bytes from a faster host. Nothing GitHub says is ever an input to a
# decision: the manifest is fetched only from GitLab, its signature is verified
# against a key compiled into Lightning, and the SHA-256 a download is checked
# against is fixed before any byte is fetched.
#
# The three hosts are constants on purpose. If they were overridable by an
# environment variable, that variable could steer the mirror_url that ends up
# INSIDE the signed manifest — the one field whose whole value is that the
# release authority chose it.
# Consumed by the scripts that source this library.
# shellcheck disable=SC2034
UPDATE_MIRROR_API_HOST="https://api.github.com"
# shellcheck disable=SC2034
UPDATE_MIRROR_UPLOAD_HOST="https://uploads.github.com"
UPDATE_MIRROR_DOWNLOAD_HOST="https://github.com"

# ONE switch decides whether this pipeline mirrors, and it is the non-secret
# half of the configuration. Deliberately NOT keyed on the token as well:
# "repo set, token missing" must be a loud failure in the mirror job, not a
# silent no-op that leaves a mirror_url in a signed manifest pointing at a
# release nobody ever created.
update_mirror_enabled() {
    [[ -n "${GITHUB_MIRROR_REPO:-}" ]]
}

# <owner>/<repo>, and nothing that could carry a path segment, a scheme, a
# query, or userinfo into a URL that is about to be signed.
update_mirror_repo_valid() {
    [[ "$1" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*/[A-Za-z0-9][A-Za-z0-9._-]*$ ]]
}

# GitHub rewrites an asset name that contains characters it does not accept
# (spaces become dots, for one), which would leave the deterministic URL in the
# manifest pointing at a name the release does not have. Refuse such a filename
# instead of publishing a URL that will not resolve. Every artifact this
# pipeline builds already matches.
# '+' is deliberately NOT allowed: the upload endpoint takes the asset name in
# a QUERY STRING, where '+' decodes to a space. GitHub would store the file
# under a rewritten name and the mirror_url already inside the SIGNED manifest
# would not resolve -- exactly the class of rewrite this predicate exists to
# prevent. Refuse the name instead.
update_mirror_filename_safe() {
    [[ "$1" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]
}

# The immutable, version-specific asset URL. Never /releases/latest/download/…:
# that is a GitHub-derived pointer, and the version a client installs must come
# only from the signed manifest.
update_mirror_asset_url() { # repo tag filename
    printf '%s/%s/releases/download/%s/%s' \
        "$UPDATE_MIRROR_DOWNLOAD_HOST" "$1" "$2" "$3"
}

# ---------------------------------------------------------------------------
# The bridge between this project and the trust table compiled into Lightning.
#
# Lightning's src/update/UpdateTrustStore.cpp holds ONE row per trusted key id,
# and the public key for each row comes from its OWN CMake cache variable —
# LIGHTNING_UPDATE_PUBKEY_2026A for lightning-release-2026a. The name is key-id
# specific by design: a build can trust several ids at once, so one shared
# variable could not express which key is which.
#
# The consequence, and the reason this table is duplicated here: introducing
# `lightning-release-2026b` is a THREE-part change, and any one of them done
# alone produces a release that cannot verify its own updates.
#
#   1. Lightning (project 6): a new trust-table row AND a new
#      LIGHTNING_UPDATE_PUBKEY_2026B cache variable.
#   2. lightning-deploy (project 7): a new UPDATE_SIGNING_PUBKEY_2026B CI
#      variable, the mapping row below, and the -D flag in configure-build.sh,
#      build-windows.sh and the Flatpak manifest.
#   3. Only then may UPDATE_SIGNING_KEY_ID be switched to it.
#
# Until step 2 is done, check-update-signing-keys.sh fails closed on the unknown
# id rather than letting a pipeline sign with a key no shipped client trusts.
# Consumed by the scripts that source this library.
# shellcheck disable=SC2034
UPDATE_CLIENT_TRUSTED_KEY_IDS=(lightning-release-2026a)

# Name of the CI variable (and, one-to-one, the CMake cache variable minus its
# LIGHTNING_ prefix) that carries the PUBLIC half for a given key id.
update_pubkey_var_for_key_id() {
    case "$1" in
        lightning-release-2026a) printf 'UPDATE_SIGNING_PUBKEY_2026A' ;;
        *) return 1 ;;
    esac
}

# A raw Ed25519 public key is 32 bytes, which is exactly 44 base64 characters
# ending in one '='. Shape-checking it here means a truncated or PEM-pasted
# value is rejected with a clear message instead of silently embedding a key
# that can never verify anything.
update_valid_public_key_b64() {
    [[ "$1" =~ ^[A-Za-z0-9+/]{43}=$ ]]
}

# Write the DER SubjectPublicKeyInfo of a key PEM to a file, and prove it is
# Ed25519 by its fixed 44-byte/12-byte-prefix shape.
#
# `openssl pkey -pubout` derives only the PUBLIC half, so this is safe to run on
# a private key file. It is deliberately used instead of `openssl pkey -text`,
# which would print private key material to stdout.
#   $1 = key PEM path, $2 = output DER path, $3 = "public" when $1 is a pubkey
update_write_spki_der() {
    local pem="$1" out="$2" kind="${3:-private}" size hex
    if [[ "$kind" == public ]]; then
        openssl pkey -in "$pem" -pubin -pubout -outform DER -out "$out" 2>/dev/null || return 1
    else
        openssl pkey -in "$pem" -pubout -outform DER -out "$out" 2>/dev/null || return 1
    fi
    size="$(wc -c <"$out" | tr -d ' ')"
    [[ "$size" == "$UPDATE_ED25519_SPKI_BYTES" ]] || return 1
    hex="$(od -An -tx1 -v -N 12 "$out" | tr -d ' \n')"
    [[ "$hex" == "$UPDATE_ED25519_SPKI_PREFIX" ]] || return 1
}

# Print the raw 32-byte Ed25519 public key of a key PEM as base64 — exactly the
# form Lightning's compiled-in trust table stores.
update_public_key_b64() {
    local pem="$1" kind="${2:-private}" der
    der="$(mktemp)"
    if ! update_write_spki_der "$pem" "$der" "$kind"; then
        rm -f "$der"
        return 1
    fi
    tail -c 32 "$der" | openssl base64 -A
    rm -f "$der"
}
