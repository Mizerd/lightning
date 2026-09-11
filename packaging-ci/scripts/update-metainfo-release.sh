#!/usr/bin/env bash
# Keep the AppStream <releases> list in step with the release actually being
# built, and assert that it IS in step.
#
# WHY THIS EXISTS. Nothing in the pipeline ever wrote <releases>. §14's release
# flow bumps CMake and Cargo and writes docs/releases/v<X.Y.Z>.md; configure-
# build.sh and build-flatpak.sh only COPY the static metainfo. On a store that
# renders AppStream -- Flathub, GNOME Software, KDE Discover -- that listing
# shows no version and no changelog, for every release, forever, and nothing
# fails to tell you.
#
# The release notes are the source. `write` derives one <release> entry from
# docs/releases/v<version>.md: its version, its date, a link to the release
# page, and the notes' own lead paragraph as the description. That paragraph is
# a human summary in every file the repository has (all 22 checked), and using
# it means the changelog cannot drift from the notes.
#
# `check` is the other half and the one that belongs in CI: it asserts the TOP
# <release> is the version being built. Same shape as the --version assertion
# validate-flatpak.sh already runs against the installed binary -- a build that
# forgot to run `write` fails loudly instead of publishing a stale changelog.
#
# Both are idempotent. `write` replaces an existing entry for the same version
# rather than stacking a second one, and touches nothing else in the file:
# it is a targeted text edit, not an XML re-serialisation, so the comments and
# formatting that explain the OARS values and the branding colours survive.
#
# Usage:
#   update-metainfo-release.sh write <X.Y.Z> [--date YYYY-MM-DD]
#                                            [--notes PATH] [--metainfo PATH]
#   update-metainfo-release.sh check <X.Y.Z> [--metainfo PATH]
#
# --date defaults to the commit date of tag v<X.Y.Z> when that tag exists
# (which is the honest release date, and is never in the future -- a date in
# the future is an appstreamcli validation error), and to today in UTC when it
# does not, because in RELEASE_ACTION=create mode the tag is made AFTER the
# packages are built and verified.
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib.sh
. "$SCRIPT_DIR/lib.sh"
ROOT=$(project_dir)

# The release page a user reads the full notes on. GitLab is the release
# authority (§14): it holds the tag, the signed packages and the notes.
: "${METAINFO_RELEASE_URL_PREFIX:=https://gitlab.smetonis.net/Mizerd/lightning/-/releases/v}"

METAINFO="$ROOT/packaging-ci/packaging/common/lightning.metainfo.xml"
NOTES=""
DATE=""

command="${1:-}"
version="${2:-}"
[ -n "$command" ] || die "usage: update-metainfo-release.sh <write|check> <X.Y.Z> [options]"
[ -n "$version" ] || die "usage: update-metainfo-release.sh $command <X.Y.Z> [options]"
shift 2 || true

case "$version" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) die "version must be X.Y.Z, got '$version'" ;;
esac

while [ $# -gt 0 ]; do
    case "$1" in
        --date)     DATE="${2:-}";     [ -n "$DATE" ]     || die "--date needs a value";     shift 2 ;;
        --notes)    NOTES="${2:-}";    [ -n "$NOTES" ]    || die "--notes needs a value";    shift 2 ;;
        --metainfo) METAINFO="${2:-}"; [ -n "$METAINFO" ] || die "--metainfo needs a value"; shift 2 ;;
        *) die "unknown option '$1'" ;;
    esac
done

[ -f "$METAINFO" ] || die "no metainfo at $METAINFO"

if [ "$command" = check ]; then
    python3 - "$METAINFO" "$version" <<'PY'
import re, sys
path, want = sys.argv[1], sys.argv[2]
text = open(path, encoding="utf-8").read()
# Mask comments -- same length, so offsets still line up -- before looking for
# any element. A comment that merely NAMES a tag is not that tag, and a search
# that cannot tell the difference will splice into prose.
text = re.sub(r"<!--.*?-->", lambda m: " " * len(m.group(0)), text, flags=re.S)
block = re.search(r"<releases\b[^>]*>(.*?)</releases>", text, re.S)
if not block:
    sys.exit(f"FAIL: {path} has no <releases> element at all. "
             "Flathub rejects a metainfo without one.")
first = re.search(r"<release\b[^>]*\bversion=\"([^\"]+)\"", block.group(1))
if not first:
    sys.exit(f"FAIL: {path} has an EMPTY <releases> element. "
             "Run: update-metainfo-release.sh write <version>")
got = first.group(1)
if got != want:
    sys.exit(
        f"FAIL: the metainfo's newest <release> is {got}, but this build is "
        f"{want}. The published listing would show {got}'s version and "
        f"changelog for a {want} package. Run:\n"
        f"  packaging-ci/scripts/update-metainfo-release.sh write {want}")
print(f"metainfo newest release: {got} (matches the build)")
PY
    exit 0
fi

[ "$command" = write ] || die "unknown command '$command' (expected write or check)"

[ -n "$NOTES" ] || NOTES="$ROOT/docs/releases/v$version.md"
[ -f "$NOTES" ] || die "no release notes at $NOTES -- §14 writes docs/releases/v<version>.md as part of the release commit; this script will not invent a changelog"

if [ -z "$DATE" ]; then
    DATE=$(git -C "$ROOT" log -1 --format=%cs "v$version" 2>/dev/null || true)
    [ -n "$DATE" ] || DATE=$(date -u +%F)
fi
case "$DATE" in
    [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]) ;;
    *) die "--date must be YYYY-MM-DD, got '$DATE'" ;;
esac
[ "$DATE" \> "$(date -u +%F)" ] && die "release date $DATE is in the future; appstreamcli rejects that"

python3 - "$METAINFO" "$NOTES" "$version" "$DATE" "$METAINFO_RELEASE_URL_PREFIX" <<'PY'
import html, re, sys, textwrap

path, notes_path, version, date, url_prefix = sys.argv[1:6]

# --- the lead paragraph of the release notes -------------------------------
#
# Everything between the "# Lightning X.Y.Z" title and the first "## " section,
# first non-empty block only. Deliberately NOT the "## " headings: several
# releases head a section "Calls", "Under the hood" or "Known limitations",
# which say nothing on their own and would read as a changelog that is mostly
# filler. The lead paragraph is the summary its author already wrote.
lines = open(notes_path, encoding="utf-8").read().splitlines()
start = next((i + 1 for i, l in enumerate(lines) if l.startswith("# ")), None)
if start is None:
    sys.exit(f"FAIL: {notes_path} has no '# ' title line")
lead = []
for line in lines[start:]:
    if line.startswith("## "):
        break
    if not line.strip():
        if lead:
            break
        continue
    lead.append(line.strip())
if not lead:
    sys.exit(f"FAIL: {notes_path} has no lead paragraph under its title. "
             "Write one, or pass --notes at a file that has one.")

text = " ".join(lead)
text = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", text)      # [label](url) -> label
text = html.escape(text, quote=False)                      # & < > first
text = re.sub(r"\*\*(.+?)\*\*", r"<em>\1</em>", text)      # AppStream has no <strong>
text = re.sub(r"`([^`]+)`", r"<code>\1</code>", text)
text = re.sub(r"\s+", " ", text).strip()

body = "\n".join("          " + l for l in textwrap.wrap(text, 70))
entry = (
    f'    <release version="{version}" date="{date}" type="stable">\n'
    f'      <url type="details">{url_prefix}{version}</url>\n'
    f"      <description>\n"
    f"        <p>\n{body}\n        </p>\n"
    f"      </description>\n"
    f"    </release>\n"
)

# --- splice it in ----------------------------------------------------------
#
# Every match is found in a COMMENT-MASKED copy and applied to the original by
# offset. The mask is the same length as what it replaces, so the offsets are
# identical in both. This is not fastidiousness: a first revision searched the
# raw text, matched a comment that merely named the element, and replaced the
# whole document with one release entry.
doc = open(path, encoding="utf-8").read()
masked = re.sub(r"<!--.*?-->", lambda m: " " * len(m.group(0)), doc, flags=re.S)

v = re.escape(version)
spans = [m.span() for m in re.finditer(
    rf'[ \t]*<release\b[^>]*\bversion="{v}"[^>]*(?:/>|>.*?</release>)[ \t]*\n',
    masked, flags=re.S)]
# Idempotent: drop any existing entry for this exact version, paired or
# self-closing, before prepending the new one. Reverse order so earlier
# offsets stay valid.
for start, end in reversed(spans):
    doc = doc[:start] + doc[end:]
    masked = masked[:start] + masked[end:]

open_tag = re.search(r"<releases\b[^>]*>[ \t]*\n", masked)
if not open_tag:
    sys.exit(f"FAIL: {path} has no releases element to write into")
at = open_tag.end()
doc = doc[:at] + entry + doc[at:]

open(path, "w", encoding="utf-8").write(doc)
print(f"{'replaced' if spans else 'added'} release {version} ({date}) in {path}")
PY
