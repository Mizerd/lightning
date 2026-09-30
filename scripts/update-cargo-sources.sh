#!/usr/bin/env bash
# Regenerate cargo-sources.json (the crate list Flathub builds offline from)
# from rust/Cargo.lock. Run it after any Cargo.lock change; config-tests fails
# until you do (packaging-ci/tests/test-cargo-sources-lock.py).
#
# The generator is flatpak-builder-tools' flatpak-cargo-generator, pinned by
# commit AND checksum: it reproduces the committed file byte for byte, and a
# newer one may lay it out differently. Needs nix, or python3 with aiohttp and
# tomlkit on PATH (set CARGO_SOURCES_PYTHON).
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
REV=41c20aa10819cdb2a4f3ca171758a96d1955c018
SHA256=0a2db6be87d75910facef28ab46d4d6460802e8419ab850d0caa6a364d26b380
URL="https://raw.githubusercontent.com/flatpak/flatpak-builder-tools/${REV}/cargo/flatpak-cargo-generator.py"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
curl --proto '=https' --tlsv1.2 -fsSL --max-redirs 3 "$URL" -o "$tmp/fcg.py"
printf '%s  %s\n' "$SHA256" "$tmp/fcg.py" | sha256sum -c --quiet -

if [[ -n "${CARGO_SOURCES_PYTHON:-}" ]]; then
    "$CARGO_SOURCES_PYTHON" "$tmp/fcg.py" "$ROOT/rust/Cargo.lock" -o "$ROOT/cargo-sources.json"
else
    nix-shell -p "python3.withPackages (p: [p.aiohttp p.tomlkit])" --run \
        "python3 '$tmp/fcg.py' '$ROOT/rust/Cargo.lock' -o '$ROOT/cargo-sources.json'"
fi
python3 "$ROOT/packaging-ci/tests/test-cargo-sources-lock.py"
