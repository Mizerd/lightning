#!/usr/bin/env bash
# Keep the AppStream <releases> list in step with the release being built.
#
# `write` derives a <release> entry from docs/releases/v<version>.md: version,
# date, release page link, and the notes' lead paragraph as the description.
# It replaces an existing entry for the same version and is a targeted text
# edit, so the file's comments and formatting survive.
#
# `check` (for CI) asserts the newest <release> is the version being built.
#
# Usage:
#   update-metainfo-release.sh write <X.Y.Z> [--date YYYY-MM-DD]
#                                            [--notes PATH] [--metainfo PATH]
#   update-metainfo-release.sh check <X.Y.Z> [--metainfo PATH]
#
# --date defaults to the commit date of tag v<X.Y.Z>, or today (UTC) when the
# tag does not exist yet, as in RELEASE_ACTION=create. A future date fails
# appstreamcli validation.
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib.sh
. "$SCRIPT_DIR/lib.sh"
ROOT=$(project_dir)

# The GitLab release page holding the full notes.
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
# Mask comments (same length) so a comment naming a tag never matches.
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

# The notes' lead paragraph: the first block under the title, before any
# "## " section. Section headings alone say nothing.
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

# Never break on a hyphen or inside a word: AppStream turns the newline into
# a space ("first- class").
body = "\n".join("          " + l for l in textwrap.wrap(
    text, 70, break_on_hyphens=False, break_long_words=False))
entry = (
    f'    <release version="{version}" date="{date}" type="stable">\n'
    f'      <url type="details">{url_prefix}{version}</url>\n'
    f"      <description>\n"
    f"        <p>\n{body}\n        </p>\n"
    f"      </description>\n"
    f"    </release>\n"
)

# Splice it in. Matches come from a comment-masked copy of the same length and
# apply to the original by offset, so a comment naming an element never
# matches.
doc = open(path, encoding="utf-8").read()
masked = re.sub(r"<!--.*?-->", lambda m: " " * len(m.group(0)), doc, flags=re.S)

v = re.escape(version)
spans = [m.span() for m in re.finditer(
    rf'[ \t]*<release\b[^>]*\bversion="{v}"[^>]*(?:/>|>.*?</release>)[ \t]*\n',
    masked, flags=re.S)]
# Replace any existing entry for this version. Reverse order keeps offsets
# valid.
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
