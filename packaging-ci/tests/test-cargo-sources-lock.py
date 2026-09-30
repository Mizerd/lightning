#!/usr/bin/env python3
"""The Flathub manifest's crate list must match rust/Cargo.lock.

Flathub builds offline from `cargo-sources.json` in the repository root, and
nothing else in this pipeline reads that file: the CI Flatpak fetches crates
online with `cargo fetch --locked`. So a Cargo.lock change without a
regenerated cargo-sources.json passes every job here and fails only on
Flathub's builder, after the release.

Asserts that every registry crate in the lock file appears in
cargo-sources.json at `cargo/vendor/<name>-<version>` with the lock's
checksum as its sha256. Extra entries are reported but allowed. A git
dependency in the lock file fails outright, because this check cannot verify
one and flatpak-cargo-generator lays it out differently.

Usage: test-cargo-sources-lock.py [Cargo.lock] [cargo-sources.json]
Defaults are the repository's own files. Needs Python 3.11+ (tomllib).
"""
import json
import pathlib
import re
import sys
import tomllib

ROOT = pathlib.Path(__file__).resolve().parents[2]
lock_path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "rust/Cargo.lock"
sources_path = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "cargo-sources.json"

lock = tomllib.loads(lock_path.read_text())
sources = json.loads(sources_path.read_text())

vendored = {}
for entry in sources:
    dest = entry.get("dest", "")
    if entry.get("type") == "archive" and dest.startswith("cargo/vendor/"):
        vendored[dest.removeprefix("cargo/vendor/")] = entry.get("sha256")

errors = []
wanted = set()
for pkg in lock.get("package", []):
    source = pkg.get("source")
    if source is None:
        continue  # a workspace crate, built from the checkout
    key = f"{pkg['name']}-{pkg['version']}"
    if not source.startswith("registry+"):
        errors.append(f"{key}: non-registry source {source!r} cannot be checked")
        continue
    wanted.add(key)
    if key not in vendored:
        errors.append(f"{key}: in Cargo.lock, missing from cargo-sources.json")
    elif vendored[key] != pkg.get("checksum"):
        errors.append(f"{key}: sha256 differs from the Cargo.lock checksum")

extra = sorted(set(vendored) - wanted)
if extra:
    print(f"note: {len(extra)} crates in cargo-sources.json are not in Cargo.lock "
          f"(allowed): {', '.join(extra[:5])}{' ...' if len(extra) > 5 else ''}")

# The local regeneration script and the Flathub repository's copy must use
# the same generator, or the two would lay the file out differently.
pins = {}
for path, rev_re, sum_re in (
        (ROOT / "scripts/update-cargo-sources.sh", r"^REV=([0-9a-f]{40})$", r"^SHA256=([0-9a-f]{64})$"),
        (ROOT / "packaging-ci/flathub/update-cargo-sources.py",
         r'^GENERATOR_REV = "([0-9a-f]{40})"$', r'^GENERATOR_SHA256 = "([0-9a-f]{64})"$')):
    if path.exists():
        text = path.read_text()
        rev, digest = re.search(rev_re, text, re.M), re.search(sum_re, text, re.M)
        if not (rev and digest):
            errors.append(f"{path.name}: no generator pin found")
        else:
            pins[path.name] = (rev.group(1), digest.group(1))
if len(set(pins.values())) > 1:
    errors.append(f"the generator pins disagree: {pins}")

if errors:
    for e in errors[:40]:
        print(f"FAIL {e}")
    print(f"FAIL: {len(errors)} problems; run scripts/update-cargo-sources.sh")
    sys.exit(1)
print(f"PASS: all {len(wanted)} registry crates in Cargo.lock are in cargo-sources.json "
      "with matching checksums")
