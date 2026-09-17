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
    # A FAILED `git` MUST NOT LOOK LIKE "NO TAGS". Without this, "git is
    # missing", "not a repository" and "the tag fetch failed" all produced the
    # same empty list as a legitimately tagless shallow clone — and each one
    # then degraded into a green job with two note: lines, on a manifest
    # carrying any sha at all. Measured with a git shim exiting 128.
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
        # A CHECK THAT CANNOT RUN MUST SAY SO, NOT FAIL AND NOT PASS QUIETLY.
        #
        # This is the only entry in config-tests that needs the repository's
        # TAGS, and a CI clone decides on its own whether it has any: a
        # shallow fetch carries none. Without this branch `newest` is None,
        # the predicate collapses to `tag == pending`, and a correct pin at
        # the newest published release fails a gate for a property of the
        # clone. The peel check below already degrades with a note; this one
        # used to degrade into a hard failure.
        print(f"  note: this clone carries no release tags, so the pin "
              f"({tag}) can only be checked for SHAPE here — run this where "
              f"tags are fetched before trusting it")
    else:
        # The pin may name the newest published release, or the version this
        # tree is being released as, or — ONLY while the tree is still the
        # version that was just tagged — the release before it.
        #
        # That last window is structural, not sloppiness: a release commit
        # cannot pin its own sha, because the sha does not exist until the
        # commit does. So between `finalize-release` creating v<pending> and
        # the re-pin commit that follows it, the manifest legitimately names
        # the PREVIOUS release while `newest` has already moved on. Without
        # this clause every pipeline in that window dies on a gate that is
        # reporting correct state.
        #
        # It closes itself. The moment development moves to the next version,
        # `newest != pending` and the previous-release spelling is refused
        # again, so a pin that was never updated cannot hide behind it.
        allowed = {pending, newest}
        # THE WINDOW IS THE TAGGED COMMIT ITSELF, NOT THE WHOLE RELEASE CYCLE.
        #
        # Keying it on `newest == pending` alone was wrong in a way that
        # defeated the check's own purpose: the release commit bumps the tree
        # version, so that condition holds from the moment v<X> is tagged
        # until the version is bumped for the NEXT release — the entire
        # inter-release life of the repository. A stale pin would have drawn
        # only a note for all of it, and the very drift this gate was written
        # for (the manifest at v0.9.6 while the tree was 0.9.7) is that state.
        # It would have caught its own motivating defect one release late.
        #
        # The real window is the minutes between `finalize-release` creating
        # the tag and the re-pin commit landing — during which HEAD IS the
        # tagged commit, because nothing else has been pushed yet. Keyed on
        # that, it closes the instant any commit lands on main.
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
        # A line that both names another version AND claims currency.
        # A currency claim, not a comparison or a history line. "differs
        # between X and Y" and "moved X -> Y" are the shapes this file uses to
        # record how it got here, and they are exactly what must stay legible.
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
