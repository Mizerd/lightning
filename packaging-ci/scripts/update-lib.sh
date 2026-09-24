#!/usr/bin/env bash

# Shared constants and helpers for the signed update manifest.
#
# The update manifest is the small signed document clients poll; it is separate
# from dist/manifest.json, the publication manifest. Do not enable command
# tracing in anything that sources this: the signing script handles key material.
[[ -n "${BASH_VERSION:-}" ]] || { printf 'error: bash is required\n' >&2; exit 1; }

# File names are part of the client contract (UPDATE-SPEC §2, §6). Changing
# either one breaks every already-shipped Lightning build.
UPDATE_MANIFEST_NAME="update-manifest-v1.json"
# Consumed by the scripts that source this library.
# shellcheck disable=SC2034
UPDATE_SIG_NAME="${UPDATE_MANIFEST_NAME}.sig"

# A separate generic package from "lightning", so the update documents (with
# their mutable "latest" slot) can never collide with release artifacts.
UPDATE_PACKAGE_NAME="${UPDATE_PACKAGE_NAME:-lightning-update}"

# The slot clients poll; the only mutable published path.
# shellcheck disable=SC2034
UPDATE_LATEST_SLOT="latest"
# The GitHub fallback slot compiled into clients (src/update/UpdateEndpoints.cpp).
# Constant so no environment variable can redirect it to a slot nobody reads.
# shellcheck disable=SC2034
if [[ "${UPDATE_LATEST_TAG:-}" != update-latest ]]; then
    UPDATE_LATEST_TAG="update-latest"
fi
# Guarded so sourcing this file twice does not abort on the readonly.
if ! (readonly -p 2>/dev/null | grep -q ' UPDATE_LATEST_TAG='); then
    readonly UPDATE_LATEST_TAG
fi

# Signature envelope algorithm identifier (UPDATE-SPEC §2). Ed25519 only.
# shellcheck disable=SC2034
UPDATE_SIG_ALG="ed25519"

# Ed25519 SPKI DER is 44 bytes: this fixed 12-byte prefix plus the raw 32-byte
# key. Checking the shape avoids `openssl pkey -text`, which prints private keys.
UPDATE_ED25519_SPKI_PREFIX="302a300506032b6570032100"
UPDATE_ED25519_SPKI_BYTES=44

# Key ids are published and matched against a compiled-in table; keep them
# log-safe.
update_valid_key_id() {
    [[ "$1" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$ ]]
}

# ---------------------------------------------------------------------------
# GitHub bandwidth mirror (MIRROR-SPEC §6)
#
# GitHub holds byte-identical copies only; the signed manifest from GitLab
# decides everything. The hosts are constants so no environment variable can
# steer the mirror_url that ends up inside the signed manifest.
# Consumed by the scripts that source this library.
# shellcheck disable=SC2034
UPDATE_MIRROR_API_HOST="https://api.github.com"
# shellcheck disable=SC2034
UPDATE_MIRROR_UPLOAD_HOST="https://uploads.github.com"
UPDATE_MIRROR_DOWNLOAD_HOST="https://github.com"

# Keyed on the repo only, not the token: "repo set, token missing" must fail
# loudly in the mirror job rather than leave an unbacked mirror_url signed.
update_mirror_enabled() {
    [[ -n "${GITHUB_MIRROR_REPO:-}" ]]
}

# <owner>/<repo> only: no path, scheme, query or userinfo in a signed URL.
update_mirror_repo_valid() {
    [[ "$1" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*/[A-Za-z0-9][A-Za-z0-9._-]*$ ]]
}

# GitHub rewrites asset names it does not accept, which would break the
# deterministic mirror_url in the signed manifest. '+' is excluded because the
# upload endpoint takes the name in a query string, where it decodes to a space.
update_mirror_filename_safe() {
    [[ "$1" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]
}

# The immutable, version-specific asset URL. Never /releases/latest/download/:
# the version a client installs must come only from the signed manifest.
update_mirror_asset_url() { # repo tag filename
    printf '%s/%s/releases/download/%s/%s' \
        "$UPDATE_MIRROR_DOWNLOAD_HOST" "$1" "$2" "$3"
}

# ---------------------------------------------------------------------------
# Mirrors the trust table in src/update/UpdateTrustStore.cpp, where each key id
# has its own CMake variable (LIGHTNING_UPDATE_PUBKEY_2026A for
# lightning-release-2026a). Adding a key id takes three steps, in order:
#
#   1. App: a new trust-table row and LIGHTNING_UPDATE_PUBKEY_<ID> variable.
#   2. Pipeline: an UPDATE_SIGNING_PUBKEY_<ID> CI variable, the mapping row
#      below, and the -D flag in configure-build.sh, build-windows.sh and the
#      Flatpak manifest.
#   3. Only then switch UPDATE_SIGNING_KEY_ID to it.
#
# check-update-signing-keys.sh fails closed on an id missing from this table.
# Consumed by the scripts that source this library.
# shellcheck disable=SC2034
UPDATE_CLIENT_TRUSTED_KEY_IDS=(lightning-release-2026a)

# Name of the CI variable (and, one-to-one, the CMake cache variable minus its
# LIGHTNING_ prefix) that carries the public half for a given key id.
update_pubkey_var_for_key_id() {
    case "$1" in
        lightning-release-2026a) printf 'UPDATE_SIGNING_PUBKEY_2026A' ;;
        *) return 1 ;;
    esac
}

# A raw 32-byte Ed25519 key is 44 base64 characters ending in one '=';
# rejects truncated or PEM-pasted values early.
update_valid_public_key_b64() {
    [[ "$1" =~ ^[A-Za-z0-9+/]{43}=$ ]]
}

# Write the DER SubjectPublicKeyInfo of a key PEM and prove it is Ed25519 by
# shape. `-pubout` emits only the public half, so it is safe on a private key.
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
