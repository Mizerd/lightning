#!/usr/bin/env bash
set -Eeuo pipefail

# Open the Flathub update pull request for a release that has just been
# published and mirrored.
#
# The two files Flathub builds from live in this repository's root:
# org.lightning_matrix.Lightning.yaml and cargo-sources.json, and
# packaging-ci/flathub/ holds the Flathub repository's own workflow. At the release
# commit the manifest still pins the PREVIOUS tag, because a commit cannot
# name its own sha, so this job re-pins it to the new tag and commit and
# pushes them to a branch of the Flathub repository, then opens a PR.
#
# It opens a PR and never merges one. Flathub protects the default branch
# (only a merged PR publishes), runs a test build on every PR, and restricts
# automatic merging, so the maintainer's merge is the last step.
#
# The files come from the GitHub mirror at the release tag, cloned
# anonymously, because that clone is exactly what Flathub's builder fetches:
# if the tag is missing there or peels elsewhere, the job stops.
#
# allow_failure in CI: a GitHub or Flathub problem never fails a release that
# is already published. Idempotent: an identical branch or open PR is reused.
#
# Needs FLATHUB_GITHUB_TOKEN (protected + masked, environment `flathub`): a
# GitHub token of a Flathub maintainer that can push branches to and open pull
# requests on FLATHUB_REPO. With it unset the job reports and exits 0.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT="$(project_dir)"

: "${FLATHUB_REPO:=flathub/org.lightning_matrix.Lightning}"
: "${FLATHUB_BASE_BRANCH:=master}"
: "${FLATHUB_SOURCE_REPO:=https://github.com/Mizerd/lightning.git}"
: "${FLATHUB_API_HOST:=https://api.github.com}"
: "${FLATHUB_DRY_RUN:=false}"
MANIFEST=org.lightning_matrix.Lightning.yaml
SOURCES=cargo-sources.json
# Copied verbatim from packaging-ci/flathub/ at the tag: the workflow that
# regenerates cargo-sources.json on any PR that moves the pin (flathubbot's
# included), and the script it runs. Pushing a workflow file needs the
# token's `workflow` scope.
EXTRA=(update-cargo-sources.py .github/workflows/update-cargo-sources.yaml)
PUSHED=("$MANIFEST" "$SOURCES" "${EXTRA[@]}")

mkdir -p "$ROOT/dist/flathub"
result="$ROOT/dist/flathub/flathub-pr.json"
report() { # status detail [url]
    jq -n --arg status "$1" --arg detail "$2" --arg url "${3:-}" \
        '{status:$status, detail:$detail, pr_url:$url}' >"$result"
    printf 'flathub: %s: %s %s\n' "$1" "$2" "${3:-}"
}

if [[ "${UPDATE_REFRESH_LATEST_ONLY:-false}" == true || "${RELEASE_ACTION:-}" != create ]]; then
    report skipped "not a create-mode release"
    exit 0
fi
if [[ -z "${FLATHUB_GITHUB_TOKEN:-}" && "$FLATHUB_DRY_RUN" != true ]]; then
    report skipped "FLATHUB_GITHUB_TOKEN is not set"
    exit 0
fi

load_versions
require_var RELEASE_VERSION
[[ "$RELEASE_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "RELEASE_VERSION must be X.Y.Z"
[[ "$SOURCE_SHA" =~ ^[0-9a-f]{40}$ ]] || die "SOURCE_SHA is not a full sha"
[[ "$FLATHUB_REPO" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] || die "FLATHUB_REPO must be <owner>/<repo>"
TAG="v${RELEASE_VERSION}"
BRANCH="lightning-${RELEASE_VERSION}"

work="$(mktemp -d)"
auth_conf="$(mktemp)"
chmod 600 "$auth_conf"
cleanup() { rm -rf "$work" "$auth_conf"; }
trap cleanup EXIT

# --- the release as Flathub will fetch it ------------------------------------
export GIT_TERMINAL_PROMPT=0
git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$TAG" -- "$FLATHUB_SOURCE_REPO" "$work/src" \
    || die "cannot clone $FLATHUB_SOURCE_REPO at $TAG; is the tag mirrored yet?"
got="$(git -C "$work/src" rev-parse HEAD)"
[[ "$got" == "$SOURCE_SHA" ]] || die "$TAG on $FLATHUB_SOURCE_REPO is $got, not the released $SOURCE_SHA"
for f in "$MANIFEST" "$SOURCES"; do
    [[ -f "$work/src/$f" ]] || die "$f is missing from the release"
done
for f in "${EXTRA[@]}"; do
    [[ -f "$work/src/packaging-ci/flathub/$f" ]] || die "packaging-ci/flathub/$f is missing from the release"
done

# The crate list must match the lock file the tag builds with.
python3 "$SCRIPT_DIR/../tests/test-cargo-sources-lock.py" \
    "$work/src/rust/Cargo.lock" "$work/src/$SOURCES"

# --- re-pin the manifest -----------------------------------------------------
# Exactly one git source names the mirror; its tag and commit follow it.
old_tag="$(awk -v url="$FLATHUB_SOURCE_REPO" '
    $1 == "url:" && $2 == url { hit = 1; next }
    hit && $1 == "tag:" { print $2; exit }' "$work/src/$MANIFEST")"
[[ "$old_tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "cannot find the pinned tag in $MANIFEST"
[[ "$(grep -c "url: $FLATHUB_SOURCE_REPO\$" "$work/src/$MANIFEST")" == 1 ]] \
    || die "$MANIFEST must name $FLATHUB_SOURCE_REPO exactly once"
awk -v url="$FLATHUB_SOURCE_REPO" -v tag="$TAG" -v sha="$SOURCE_SHA" '
    $1 == "url:" && $2 == url { hit = 1; print; next }
    hit && $1 == "tag:"    { sub(/tag: .*/, "tag: " tag); print; next }
    hit && $1 == "commit:" { sub(/commit: .*/, "commit: " sha); print; hit = 0; next }
    { print }' "$work/src/$MANIFEST" >"$ROOT/dist/flathub/$MANIFEST"
out="$ROOT/dist/flathub/$MANIFEST"
[[ "$(grep -c "^ *tag: $TAG\$" "$out")" == 1 ]] || die "re-pin did not write tag $TAG exactly once"
[[ "$(grep -c "^ *commit: $SOURCE_SHA\$" "$out")" == 1 ]] || die "re-pin did not write the commit exactly once"
old_version="${old_tag#v}"
if [[ "$old_tag" != "$TAG" ]] \
    && grep -qE "(^|[^0-9.])${old_version//./\\.}([^0-9]|\$)" "$out"; then
    die "$MANIFEST still names ${old_tag#v} after the re-pin; update its prose in the release commit"
fi
# Nothing that reaches the Flathub repository carries comments (Rokas,
# 2026-09-30): full-line comments are dropped from the manifest, runs of blank
# lines squeezed, and a comment left anywhere else stops the job.
awk '/^[[:space:]]*#/ { next } /^[[:space:]]*$/ { if (blank++) next; print; next } { blank = 0; print }' \
    "$out" >"$out.tmp" && mv "$out.tmp" "$out"
sed -i '1{/^[[:space:]]*$/d}' "$out"
if grep -nE '(^|[[:space:]])#' "$out"; then
    die "$MANIFEST still carries a comment after stripping; remove it in the release commit"
fi
cp "$work/src/$SOURCES" "$ROOT/dist/flathub/$SOURCES"
for f in "${EXTRA[@]}"; do
    install -D -m 0644 "$work/src/packaging-ci/flathub/$f" "$ROOT/dist/flathub/$f"
    if grep -nE '^[[:space:]]*#' "$ROOT/dist/flathub/$f"; then
        die "packaging-ci/flathub/$f carries a comment; Flathub files must not"
    fi
done
printf 'Re-pinned %s: %s -> %s (%s)\n' "$MANIFEST" "$old_tag" "$TAG" "$SOURCE_SHA"

if [[ "$FLATHUB_DRY_RUN" == true ]]; then
    report dry-run "re-pinned files written to dist/flathub; nothing pushed"
    exit 0
fi

# --- push the branch ---------------------------------------------------------
# Credential via a 0600 config file (curl) and git's environment config
# (GIT_CONFIG_COUNT), never argv or a URL: argv is readable by every user on
# the runner host through /proc, the environment only by this user.
{
    printf 'header = "Authorization: Bearer %s"\n' "$FLATHUB_GITHUB_TOKEN"
    printf 'header = "Accept: application/vnd.github+json"\n'
    printf 'header = "X-GitHub-Api-Version: 2022-11-28"\n'
} >"$auth_conf"
basic="$(printf 'x-access-token:%s' "$FLATHUB_GITHUB_TOKEN" | base64 | tr -d '\n')"
git_auth() {
    GIT_CONFIG_COUNT=1 \
        GIT_CONFIG_KEY_0="http.https://github.com/.extraheader" \
        GIT_CONFIG_VALUE_0="Authorization: Basic $basic" \
        git "$@"
}
# FLATHUB_REMOTE_URL exists for a rehearsal against a scratch remote.
remote="${FLATHUB_REMOTE_URL:-https://github.com/${FLATHUB_REPO}.git}"

git_auth clone --quiet --depth 1 --branch "$FLATHUB_BASE_BRANCH" "$remote" "$work/flathub" \
    || die "cannot clone $FLATHUB_REPO; check FLATHUB_GITHUB_TOKEN"
for f in "${PUSHED[@]}"; do
    install -D -m 0644 "$ROOT/dist/flathub/$f" "$work/flathub/$f"
done
if [[ -z "$(git -C "$work/flathub" status --porcelain -- "${PUSHED[@]}")" ]]; then
    report up-to-date "$FLATHUB_BASE_BRANCH already builds $TAG"
    exit 0
fi

git -C "$work/flathub" checkout --quiet -b "$BRANCH"
git -C "$work/flathub" add -- "${PUSHED[@]}"
git -C "$work/flathub" \
    -c user.name="${GITLAB_USER_NAME:-Lightning release pipeline}" \
    -c user.email="${GITLAB_USER_EMAIL:-noreply@lightning-matrix.org}" \
    commit --quiet -m "Update to ${RELEASE_VERSION}"

if git_auth ls-remote --exit-code --heads "$remote" "$BRANCH" >/dev/null 2>&1; then
    # Reuse an earlier attempt only if it carries the same two files.
    git_auth -C "$work/flathub" fetch --quiet --depth 1 origin "$BRANCH"
    git -C "$work/flathub" diff --quiet FETCH_HEAD HEAD -- "${PUSHED[@]}" \
        || die "branch $BRANCH already exists on $FLATHUB_REPO with different files; delete it or fix it by hand"
    printf 'Branch %s already carries these files\n' "$BRANCH"
else
    git_auth -C "$work/flathub" push --quiet origin "$BRANCH" \
        || die "push to $FLATHUB_REPO refused; the token needs write access (Contents)"
fi

# --- open the pull request -----------------------------------------------------
api="${FLATHUB_API_HOST}/repos/${FLATHUB_REPO}/pulls"
owner="${FLATHUB_REPO%%/*}"
status="$(curl --silent --show-error --max-redirs 0 --config "$auth_conf" \
    --output "$work/open.json" --write-out '%{http_code}' \
    "${api}?state=open&head=${owner}:${BRANCH}")"
[[ "$status" == 200 ]] || die "listing pull requests failed (HTTP $status)"
existing="$(jq -r '.[0].html_url // empty' "$work/open.json")"
if [[ -n "$existing" ]]; then
    report exists "a pull request for $BRANCH is already open" "$existing"
    exit 0
fi

notes_url="https://gitlab.smetonis.net/Mizerd/lightning/-/releases/${TAG}"
jq -n --arg title "Update to ${RELEASE_VERSION}" --arg head "$BRANCH" \
    --arg base "$FLATHUB_BASE_BRANCH" \
    --arg body "Lightning ${RELEASE_VERSION}. Release notes: ${notes_url}" \
    '{title:$title, head:$head, base:$base, body:$body}' \
    >"$work/pr.json"
status="$(curl --silent --show-error --max-redirs 0 --config "$auth_conf" \
    --header 'Content-Type: application/json' --data @"$work/pr.json" \
    --output "$work/created.json" --write-out '%{http_code}' "$api")"
[[ "$status" == 201 ]] || die "opening the pull request failed (HTTP $status): $(jq -r '.message // empty' "$work/created.json")"
report opened "Flathub runs a test build on it; merge it to publish" \
    "$(jq -r '.html_url' "$work/created.json")"
