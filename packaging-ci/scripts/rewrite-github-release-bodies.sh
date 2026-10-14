#!/usr/bin/env bash
set -Eeuo pipefail

# Rewrite the "read-only mirror ... canonical release lives on GitLab" footer of
# EXISTING GitHub release bodies (v0.7.x .. v0.10.1) to the current wording
# produced by mirror-release-to-github.sh.
#
# DRY-RUN BY DEFAULT: prints a unified diff per release and changes nothing.
# Pass --apply to PATCH the bodies. Needs an authenticated `gh` with write
# access to the repository; the token is gh's own and is never printed.
#
#   rewrite-github-release-bodies.sh                 # dry run, all tags
#   rewrite-github-release-bodies.sh --apply         # rewrite
#   rewrite-github-release-bodies.sh v0.10.1 v0.9.9  # only these tags
#
# Environment: GITHUB_MIRROR_REPO (default Mizerd/lightning).
# Idempotent: a body already carrying the new footer is skipped. A body with no
# recognisable old footer is skipped with a notice, never guessed at.

repo="${GITHUB_MIRROR_REPO:-Mizerd/lightning}"
apply=false
tags=()
for a in "$@"; do
    case "$a" in
        --apply) apply=true ;;
        v*) tags+=("$a") ;;
        *) printf 'unknown argument: %s\n' "$a" >&2; exit 2 ;;
    esac
done
command -v gh >/dev/null && command -v jq >/dev/null || { echo 'gh and jq are required' >&2; exit 2; }

if ((${#tags[@]} == 0)); then
    all_tags="$(gh api --paginate "repos/${repo}/releases?per_page=100" --jq '.[].tag_name')" || {
        echo 'listing releases failed (auth or rate limit?); refusing to continue' >&2
        exit 1
    }
    mapfile -t tags < <(printf '%s\n' "$all_tags" | grep -E '^v0\.(7|8|9|10)\.[0-9]+$' | sort -V || true)
    ((${#tags[@]} > 0)) || { echo 'no v0.7.x..v0.10.x releases found; refusing to report success' >&2; exit 1; }
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
# One anchored pattern for both the detection and the cut, so they cannot disagree.
OLD_FOOTER='^\*\*This is a read-only mirror'
repo_url="https://github.com/${repo}"

new_footer() { # version
    cat <<EOT
Lightning $1 is published here on GitHub. The release assets are
built and validated on clean systems by the project's packaging pipeline and
uploaded unchanged.

Verify any download against the \`SHA256SUMS\` asset.

Lightning's in-app updater reads a signed update manifest (published as the
\`update-latest\` release on this repository) and verifies an Ed25519 signature
made with a key that is not stored on GitHub. Every downloaded file is checked
against the SHA-256 recorded in that signed manifest before it is installed, so
replacing a file here cannot cause Lightning to install it.

Issues, source, and history: ${repo_url}
EOT
}

changed=0
for tag in "${tags[@]}"; do
    rel="$tmp/$tag.json"
    if ! gh api "repos/${repo}/releases/tags/${tag}" >"$rel" 2>"$tmp/err"; then
        if grep -q 'HTTP 404' "$tmp/err"; then
            printf '%s: no such release, skipped\n' "$tag"
            continue
        fi
        printf '%s: gh api failed (auth or rate limit?): %s\n' "$tag" "$(head -c 300 "$tmp/err")" >&2
        exit 1
    fi
    id="$(jq -r .id "$rel")"
    jq -r '.body // ""' "$rel" >"$tmp/$tag.old"
    if ! grep -Eq "$OLD_FOOTER" "$tmp/$tag.old"; then
        printf '%s: no old footer found (already rewritten or hand-edited), skipped\n' "$tag"
        continue
    fi
    # Keep everything before the old footer marker, replace the rest.
    OLD_FOOTER="$OLD_FOOTER" awk '$0 ~ ENVIRON["OLD_FOOTER"] {exit} {print}' "$tmp/$tag.old" >"$tmp/$tag.head"
    { cat "$tmp/$tag.head"; new_footer "${tag#v}"; } >"$tmp/$tag.new"
    printf '=== %s (release id %s)\n' "$tag" "$id"
    diff -u "$tmp/$tag.old" "$tmp/$tag.new" || true
    if $apply; then
        jq -n --rawfile body "$tmp/$tag.new" '{body: $body}' \
            | gh api --method PATCH "repos/${repo}/releases/${id}" --input - >/dev/null
        printf '%s: body rewritten\n' "$tag"
    fi
    changed=$((changed + 1))
done
$apply || printf '\nDRY RUN: %d release(s) would change. Re-run with --apply.\n' "$changed"
