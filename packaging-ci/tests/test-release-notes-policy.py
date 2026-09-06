#!/usr/bin/env python3
"""The release description must carry a Code signing policy link.

SignPath Foundation requires the term "Code signing policy" on the project's
download/release pages. finalize-release.sh appends it to every created
release's description, so this is checked here rather than trusted to whoever
writes the next release notes by hand.

The footer function is sourced and exercised directly; nothing here touches
GitLab.
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent

failures: list[str] = []


def check(condition: bool, message: str) -> None:
    if condition:
        print(f"  ok: {message}")
    else:
        print(f"  FAIL: {message}", file=sys.stderr)
        failures.append(message)


def append_footer(notes: str, tag: str = "v0.6.7", signed: bool = False) -> str:
    """Run append_policy_footer from finalize-release.sh over `notes`."""
    with tempfile.TemporaryDirectory() as tmp:
        path = pathlib.Path(tmp) / "notes.md"
        path.write_text(notes, encoding="utf-8")
        script = f"""
set -Eeuo pipefail
source "{ROOT}/scripts/lib.sh"
RELEASE_TAG="{tag}"
# Take only the footer definitions from finalize-release.sh; the rest of that
# script talks to the GitLab API.
eval "$(sed -n '/^POLICY_HEADING=/,/^}}$/p' "{ROOT}/scripts/finalize-release.sh")"
append_policy_footer "{path}" >/dev/null
cat "{path}"
"""
        environment = dict(os.environ)
        environment["LIGHTNING_WINDOWS_SIGNED"] = "true" if signed else "false"
        result = subprocess.run(
            ["bash", "-c", script],
            capture_output=True, text=True, env=environment,
        )
        if result.returncode != 0:
            raise SystemExit(f"footer helper failed: {result.stderr}")
        return result.stdout


def main() -> int:
    print("the policy section is appended to ordinary release notes")
    body = append_footer("# Lightning 0.6.7\n\nSome notes.\n")
    check("Code signing policy" in body,
          "the exact term 'Code signing policy' is present")
    check("docs/code-signing-policy.md" in body, "the policy document is linked")
    check("docs/privacy.md" in body, "the privacy policy is linked")
    check("/-/blob/v0.6.7/" in body, "links are pinned to the release tag")
    check("Some notes." in body, "the original notes are preserved")
    check("not signed" in body,
          "an unsigned release says so on its own release page")

    print("appending is idempotent")
    twice = append_footer(body)
    check(twice.count("## Code signing policy") == 1,
          "the section is not duplicated when notes already carry it")

    print("a prose mention of the policy does not suppress the disclosure")
    # This used to be the OPPOSITE assertion: any note containing the bare
    # phrase suppressed the whole footer, including "the Windows artifacts in
    # this release are not signed". Whoever writes the release notes must not
    # be able to switch off a security disclosure with a sentence.
    custom = "# Notes\n\nSee our Code signing policy elsewhere.\n"
    prose_body = append_footer(custom)
    check(prose_body != custom and "## Code signing policy" in prose_body,
          "a prose mention still gets the policy section appended")
    check("not signed" in prose_body,
          "the unsigned disclosure survives a prose mention")

    print("a hand-written policy HEADING is respected")
    handwritten = ("# Notes\n\n## Code signing policy\n\nOur own wording. "
                   "**The Windows artifacts in this release are not signed.**\n")
    check(append_footer(handwritten) == handwritten,
          "an explicit policy heading carrying the disclosure is left alone")
    lacking = "# Notes\n\n## Code signing policy\n\nOur own wording.\n"
    with_disclosure = append_footer(lacking)
    check(with_disclosure.count("## Code signing policy") == 1
          and "not signed" in with_disclosure,
          "a hand-written heading without the disclosure gets the sentence, not a second section")

    print("a signed release stops claiming to be unsigned")
    signed_body = append_footer("# Lightning 9.9.9\n", tag="v9.9.9", signed=True)
    check("not signed" not in signed_body,
          "the unsigned warning is gone when signing is active")
    check("Authenticode-signed" in signed_body, "the signed wording is used")
    check("Code signing policy" in signed_body,
          "the policy term is present either way")

    if failures:
        print(f"\n{len(failures)} check(s) failed", file=sys.stderr)
        return 1
    print("\nrelease-notes policy footer: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
