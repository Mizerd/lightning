#!/usr/bin/env python3
"""The Flathub submission manifest is a version location nothing validated.

`org.lightning_matrix.Lightning.yaml` in the repository ROOT is what Flathub
builds from, and it pins a tag and a commit of the GitHub mirror. It is NOT the
CI manifest — `packaging-ci/packaging/flatpak/org.lightning_matrix.Lightning.yaml.in`
is a different file that builds from a local checkout, and that is the only one
`test-pipeline-config.py` reads.

So this file had no test at all, and on 2026-09-16 it was found still pinned to
**v0.9.6** while the tree, the AppStream metainfo and all four screenshot URLs
had moved to 0.9.7. A full release of drift, in the one file a distribution
builds from, noticed by accident during a review.

WHAT THIS ASSERTS, and why each one:

1. The pin names a tag of the form vX.Y.Z and a full 40-character commit sha.
   A short sha resolves differently on a shallow clone and Flathub builds from
   a clone, not from here.
2. The tag is either the newest tag that EXISTS, or the version the tree is
   being released as. Both are legitimate: between a release commit and the
   pipeline creating the tag, the newest published tag is the right pin,
   because the tag this tree becomes does not exist yet and a manifest naming
   it could not be built by anyone.
3. When the tag exists locally, the commit is the one the tag PEELS to. A pin
   whose tag and sha disagree builds something nobody reviewed.
4. No paragraph in the file names a DIFFERENT release as the current pin. This
   is the one that would have caught the real defect: when the pin moved
   v0.9.6 -> v0.9.7 the data changed and three paragraphs did not, leaving a
   network verification performed at v0.9.6 sitting two lines above a v0.9.7
   pin, reading as evidence for it. Prose that describes the old state as
   current is how the README drifted a whole release too.

It deliberately does NOT fetch anything. A test that needs the network fails
for reasons that are not the code, and `config-tests` runs on every pipeline.
"""

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "org.lightning_matrix.Lightning.yaml"
CMAKE = ROOT / "CMakeLists.txt"

failures = []
checks = 0


def check(ok: bool, what: str) -> None:
    global checks
    checks += 1
    if ok:
        print(f"  ok: {what}")
    else:
        print(f"  FAIL: {what}")
        failures.append(what)


def tree_version() -> str:
    m = re.search(r"^\s*VERSION\s+(\d+\.\d+\.\d+)\s*$", CMAKE.read_text(), re.M)
    if not m:
        raise SystemExit("could not read VERSION from CMakeLists.txt")
    return m.group(1)


def main() -> int:
    print("Flathub submission manifest pin")
    if not MANIFEST.is_file():
        print(f"  FAIL: {MANIFEST} is missing")
        return 1

    text = MANIFEST.read_text()
    tag_m = re.search(r"^\s*tag:\s*(\S+)\s*$", text, re.M)
    sha_m = re.search(r"^\s*commit:\s*(\S+)\s*$", text, re.M)
    check(tag_m is not None, "the manifest pins a tag")
    check(sha_m is not None, "the manifest pins a commit")
    if not tag_m or not sha_m:
        return 1

    tag, sha = tag_m.group(1), sha_m.group(1)
    check(re.fullmatch(r"v\d+\.\d+\.\d+", tag) is not None,
          f"the pinned tag is a release tag ({tag})")
    check(re.fullmatch(r"[0-9a-f]{40}", sha) is not None,
          "the pinned commit is a full 40-character sha "
          "(a short sha resolves differently on a shallow clone)")

    version = tree_version()
    pending = f"v{version}"
    # A failed `git` must not look like "no tags" (a tagless shallow clone),
    # or any sha would pass with only notes.
    tags_run = subprocess.run(
        ["git", "-C", str(ROOT), "tag", "--list", "v*"],
        capture_output=True, text=True)
    if tags_run.returncode != 0:
        print(f"  FAIL: could not list tags: git exited {tags_run.returncode}"
              f" — {tags_run.stderr.strip() or 'no stderr'}")
        return 1
    known = tags_run.stdout.split()

    def key(t: str):
        return tuple(int(x) for x in t[1:].split("."))

    releases = sorted((t for t in known if re.fullmatch(r"v\d+\.\d+\.\d+", t)),
                      key=key)
    newest = releases[-1] if releases else None
    second = releases[-2] if len(releases) > 1 else None

    if newest is None:
        # A shallow CI clone may carry no tags. Say the check could not run
        # rather than failing a correct pin for a property of the clone.
        print(f"  note: this clone carries no release tags, so the pin "
              f"({tag}) can only be checked for SHAPE here — run this where "
              f"tags are fetched before trusting it")
    else:
        # Allowed: the newest published release, the version being released,
        # or, only while HEAD is the just-tagged commit, the release before
        # it. A release commit cannot pin its own sha, so between
        # finalize-release creating the tag and the re-pin commit the manifest
        # legitimately names the previous release.
        allowed = {pending, newest}
        # Key the window on HEAD being the tagged commit, not on
        # `newest == pending`: the latter holds for the whole inter-release
        # period and would let a stale pin through. This closes as soon as
        # any commit lands on main.
        head = subprocess.run(
            ["git", "-C", str(ROOT), "rev-parse", "HEAD"],
            capture_output=True, text=True).stdout.strip()
        newest_commit = subprocess.run(
            ["git", "-C", str(ROOT), "rev-parse", "--verify", "--quiet",
             f"{newest}^{{commit}}"],
            capture_output=True, text=True).stdout.strip()
        window = (newest == pending and second is not None
                  and bool(head) and head == newest_commit)
        if window:
            allowed.add(second)
        check(tag in allowed,
              f"the pin names the newest existing tag ({newest}), the version "
              f"this tree is being released as ({pending})"
              + (f", or the release before it ({second}) while the re-pin is "
                 f"outstanding" if window else "")
              + f" — it is {tag}")
        if window and tag == second:
            print(f"  note: the re-pin to {newest} is OUTSTANDING — the "
                  f"submission manifest still names {second}")

    exists = subprocess.run(
        ["git", "-C", str(ROOT), "rev-parse", "--verify", "--quiet", f"{tag}^{{commit}}"],
        capture_output=True, text=True)
    if exists.returncode == 0:
        peeled = exists.stdout.strip()
        check(peeled == sha,
              f"the pinned commit is what {tag} peels to")
    else:
        print(f"  note: {tag} is not a local ref, so the sha cannot be checked "
              f"here — fetch tags before trusting a pin check")

    # The prose check. Any OTHER release version mentioned as the current pin.
    others = set()
    for m in re.finditer(r"v(\d+\.\d+\.\d+)", text):
        other = f"v{m.group(1)}"
        if other == tag:
            continue
        line = text[text.rfind("\n", 0, m.start()) + 1:
                    text.find("\n", m.end())]
        # Flag a line that names another version and claims currency, but not
        # history lines ("differs between X and Y", "moved X -> Y").
        claims = re.search(r"\b(is|are|built from|pinned to|submitted|resolve)\b", line)
        history = re.search(r"MOVED|->|once|used to|previous|was |earlier"
                            r"|between|differs|diff\b", line)
        if claims and not history:
            others.add(f"{other}: {line.strip()[:70]}")
    check(not others,
          "no paragraph describes a different release as the current pin"
          + ("" if not others else f" — found {sorted(others)}"))

    print()
    if failures:
        print(f"{len(failures)} of {checks} checks FAILED")
        return 1
    print(f"all {checks} Flathub manifest pin checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
