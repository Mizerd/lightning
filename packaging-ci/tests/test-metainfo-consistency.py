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

    # 1. Every screenshot URL that names one of our own tags must name a tag
    #    that actually contains the file. A URL pointing somewhere else is not
    #    this test's business; it is checked by appstreamcli where there is a
    #    network.
    pattern = re.compile(
        r"https://raw\.githubusercontent\.com/[^/]+/[^/]+/(?P<ref>[^/]+)/"
        r"(?P<path>\S+?)</image>")
    version = project_version()
    # THE RELEASE COMMIT'S OWN STATE IS NOT A FAILURE. A release prepares the
    # metainfo for the tag it is about to be given, so between the commit and
    # the pipeline creating `v<version>` the URLs name a tag that does not
    # exist yet. Refusing that would make the check unsatisfiable in exactly
    # the commit it most needs to run in -- so for that one ref the question
    # becomes "will the tag cut from THIS tree contain the file", which the
    # COMMITTED tree answers (not the working tree: an untracked file is in
    # the working tree and in no tag). Every other unknown ref is skipped.
    pending = f"v{version}" if version else None
    found = 0
    for match in pattern.finditer(text):
        ref, path = match.group("ref"), match.group("path")
        # HOISTED ABOVE THE TAG LOOKUP, so it is unconditional. Sitting inside
        # the tag-exists branch let a ref that is neither a known tag nor this
        # version -- a typo like v0.9.7 -- fall through to `skip`, assert
        # nothing, and not even count toward the found>0 vacuity guard.
        if pending:
            check(ref == pending,
                  f"the screenshot ref {ref} names this version ({pending})")
        rc, _ = git("rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}")
        if rc != 0:
            if ref == pending:
                found += 1
                # ASK GIT, NOT THE FILESYSTEM. An UNTRACKED file passes
                # is_file() and no tag can ever contain it -- and since §4
                # forbids `git add .`, staging is explicit and a new
                # screenshot being left untracked is exactly the near-miss
                # this test exists to catch.
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

    # A scan that matches nothing passes vacuously, which is how a sweep
    # silently stops testing anything. See CLAUDE.md's mutation-check rule.
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

    # 3. And the script written to enforce that must still agree, so that a
    #    checker with no callers cannot quietly rot.
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
