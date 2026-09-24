#!/usr/bin/env python3
"""The metainfo must not advertise files that do not exist, or the wrong version.

TWO DEFECTS SHIPPED IN 0.9.5 AND NOTHING COULD SEE EITHER.

1. The four `docs/screenshots/flathub/` images were added in the SAME commit
   that pointed the metainfo at them — but the URLs name tag `v0.9.4`, and the
   files do not exist at that tag. So every published 0.9.5 package (deb, rpm,
   AppImage, snap and a Flathub submission alike) carries four screenshot URLs
   that answer 404, and GNOME Software and KDE Discover show no screenshots at
   all. `flatpak-builder-lint` fails the repo on it outright, with an error
   Flathub documents as one whose "exception is never granted".

   CI could not catch it because all three package validators run
   `appstreamcli validate --no-net` (packaging-ci/scripts/validate-deb.sh,
   validate-rpm.sh, packaging/rpm/lightning.spec) — and the network check is
   the only part of appstreamcli that can see a dead URL. That is deliberate
   and worth keeping: a package build should not fail because GitHub is slow.

   So this test answers the same question OFFLINE instead. A URL that names
   one of this repository's own tags is checkable with `git ls-tree` and no
   network at all, which is strictly better than the check that was switched
   off: it fails in the commit that introduces the mistake rather than after a
   release is already published.

2. The newest `<release>` read 0.9.4 in a tree whose version was 0.9.5, so a
   listing would show the previous release's version and changelog.
   `update-metainfo-release.sh` exists to prevent exactly this, is correct, and
   `git grep` finds NO caller anywhere — the recorded "a test file can be
   committed and never registered", in its packaging costume. This calls it.

Offline by construction: `git ls-tree` and a file read. No network, no build.
"""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
METAINFO = ROOT / "packaging-ci/packaging/common/lightning.metainfo.xml"

failures: list[str] = []


def check(condition: bool, message: str) -> None:
    if condition:
        print(f"  ok: {message}")
    else:
        print(f"  FAIL: {message}", file=sys.stderr)
        failures.append(message)


def git(*args: str) -> tuple[int, str]:
    proc = subprocess.run(
        ["git", "-C", str(ROOT), *args],
        capture_output=True, text=True, check=False,
    )
    return proc.returncode, proc.stdout


def project_version() -> str:
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\s*\(\s*\w+\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)",
                      text)
    if not match:
        match = re.search(r'APP_VERSION_LABEL\s+"?([0-9]+\.[0-9]+\.[0-9]+)', text)
    return match.group(1) if match else ""


def main() -> int:
    text = METAINFO.read_text(encoding="utf-8")

    # 1. Every screenshot URL naming one of our tags must name a tag that
    #    contains the file. Other URLs are left to appstreamcli.
    pattern = re.compile(
        r"https://raw\.githubusercontent\.com/[^/]+/[^/]+/(?P<ref>[^/]+)/"
        r"(?P<path>\S+?)</image>")
    version = project_version()
    # The release commit's refs name the tag it is about to receive, which
    # does not exist yet. For that one ref, check that the committed tree
    # (not the working tree) contains the file. Other unknown refs are
    # skipped.
    pending = f"v{version}" if version else None
    found = 0
    for match in pattern.finditer(text):
        ref, path = match.group("ref"), match.group("path")
        # Checked before the tag lookup so a ref that is neither a known tag
        # nor this version (a typo) cannot fall through to `skip` uncounted.
        if pending:
            check(ref == pending,
                  f"the screenshot ref {ref} names this version ({pending})")
        rc, _ = git("rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}")
        if rc != 0:
            if ref == pending:
                found += 1
                # Ask git, not the filesystem: an untracked screenshot passes
                # is_file() but can never be in a tag.
                rc, _ = git("cat-file", "-e", f"HEAD:{path}")
                check(rc == 0,
                      f"{path} is COMMITTED in the tree that will become "
                      f"{ref} (the tag does not exist yet; this is a release "
                      "commit)")
            else:
                # Not a ref this clone knows (a shallow checkout, or a branch
                # name). Cannot be answered offline, so it is not asserted.
                print(f"  skip: {ref} is not a ref in this clone ({path})")
            continue
        found += 1
        rc, out = git("ls-tree", "-r", "--name-only", ref, "--", path)
        check(bool(out.strip()),
              f"{path} exists at {ref} (the URL in the metainfo would 404)")

    # Guard against a scan that matches nothing and passes vacuously.
    check(found > 0,
          "at least one screenshot URL naming a known tag was checked "
          f"(checked {found})")

    # 2. The newest <release> must be the version this tree builds.
    check(bool(version), "the project version was readable from CMakeLists.txt")
    releases = re.findall(r'<release version="([0-9][^"]*)"', text)
    check(bool(releases), "the metainfo declares at least one <release>")
    if version and releases:
        check(releases[0] == version,
              f"the newest <release> is {version}, matching the project "
              f"(metainfo says {releases[0]})")

    # 3. The script that enforces this must agree too, so it cannot rot
    #    unused.
    script = ROOT / "packaging-ci/scripts/update-metainfo-release.sh"
    if script.exists() and version:
        proc = subprocess.run(
            ["bash", str(script), "check", version],
            capture_output=True, text=True, check=False, cwd=str(ROOT))
        check(proc.returncode == 0,
              "update-metainfo-release.sh check agrees "
              f"({proc.stdout.strip() or proc.stderr.strip()})")

    if failures:
        print(f"\n{len(failures)} check(s) FAILED", file=sys.stderr)
        return 1
    print("\nall metainfo consistency checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
