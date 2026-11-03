#!/usr/bin/env bash
# Run the gitlab.com Linux package lane (.gitlab/packages.yml) for SOURCE_REF,
# wait for it, and collect every validated package into dist/ exactly as the
# local validate-* jobs leave them.
#
#   GITLAB_COM_PROJECT_ID     the mirror project (88402223)
#   GITLAB_COM_TRIGGER_TOKEN  pipeline trigger token of that project
#   GITLAB_COM_READ_TOKEN     read_api token of that project (job artifacts)
#   SOURCE_REF                full commit SHA; the mirror's GITLAB_COM_REF must
#                             point at it, so scripts and source are one commit
#   GITLAB_COM_PIPELINE_ID    optional: collect from this finished pipeline
#                             instead of triggering one
#
# PUBLISH_PACKAGES and RELEASE_VERSION are passed through, so a release
# pipeline gets an official (key-embedding) build. Nothing is published there.
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT="$(project_dir)"

: "${GITLAB_COM_API:=https://gitlab.com/api/v4}"
: "${GITLAB_COM_PROJECT_ID:=88402223}"
: "${GITLAB_COM_REF:=main}"
: "${GITLAB_COM_TIMEOUT_S:=14400}"
require_var GITLAB_COM_READ_TOKEN
require_var SOURCE_REF
[[ "$SOURCE_REF" =~ ^[0-9a-f]{40}$ ]] || die "SOURCE_REF must be a full commit SHA"
[[ "$GITLAB_COM_PROJECT_ID" =~ ^[0-9]+$ ]] || die "GITLAB_COM_PROJECT_ID must be numeric"

API="$GITLAB_COM_API/projects/$GITLAB_COM_PROJECT_ID"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
# Tokens reach curl through a 0600 config file, never the command line.
( umask 077; printf 'header = "PRIVATE-TOKEN: %s"\n' "$GITLAB_COM_READ_TOKEN" >"$WORK/auth" )
api() { curl -fsS --retry 5 --retry-all-errors --max-time 120 -K "$WORK/auth" "$API/$1"; }

# Every package the release publishes, by the job that validated it.
LANE_JOBS=(
    validate-deb-amd64 validate-deb-arm64
    validate-deb-ubuntu-amd64 validate-deb-ubuntu-arm64
    validate-rpm-amd64 validate-rpm-arm64
    validate-appimage-amd64 validate-appimage-arm64
    validate-snap-amd64 validate-snap-arm64
    validate-flatpak-amd64
)
PACKAGE_GLOB='\.(deb|rpm|AppImage|snap|flatpak)$'

pipeline="${GITLAB_COM_PIPELINE_ID:-}"
if [[ -z "$pipeline" ]]; then
    require_var GITLAB_COM_TRIGGER_TOKEN
    deadline=$(( $(date +%s) + 900 ))
    until [[ "$(api "repository/branches/$GITLAB_COM_REF" | jq -r .commit.id)" == "$SOURCE_REF" ]]; do
        (( $(date +%s) < deadline )) || \
            die "gitlab.com $GITLAB_COM_REF never reached $SOURCE_REF; the push mirror has not caught up"
        sleep 30
    done
    ( umask 077; printf '%s' "$GITLAB_COM_TRIGGER_TOKEN" >"$WORK/trigger" )
    pipeline="$(curl -fsS --max-time 120 -X POST \
        --data-urlencode "token@$WORK/trigger" \
        --data-urlencode "ref=$GITLAB_COM_REF" \
        --data-urlencode "variables[PACKAGE_LANE]=1" \
        --data-urlencode "variables[RUN_TESTS]=0" \
        --data-urlencode "variables[SOURCE_REF]=$SOURCE_REF" \
        --data-urlencode "variables[PUBLISH_PACKAGES]=${PUBLISH_PACKAGES:-false}" \
        --data-urlencode "variables[RELEASE_VERSION]=${RELEASE_VERSION:-}" \
        "$API/trigger/pipeline" | jq -er .id)"
fi
[[ "$pipeline" =~ ^[0-9]+$ ]] || die "no gitlab.com pipeline id"
printf 'gitlab.com package lane: pipeline %s\n' "$pipeline"

deadline=$(( $(date +%s) + GITLAB_COM_TIMEOUT_S ))
while :; do
    status="$(api "pipelines/$pipeline" | jq -er .status)"
    case "$status" in
        success) break ;;
        failed|canceled|skipped) die "gitlab.com pipeline $pipeline ended $status" ;;
    esac
    (( $(date +%s) < deadline )) || die "gitlab.com pipeline $pipeline still $status after ${GITLAB_COM_TIMEOUT_S}s"
    sleep 60
done
pipeline_sha="$(api "pipelines/$pipeline" | jq -er .sha)"
[[ "$pipeline_sha" == "$SOURCE_REF" ]] || die "gitlab.com pipeline $pipeline built $pipeline_sha, not $SOURCE_REF"

api "pipelines/$pipeline/jobs?per_page=100" >"$WORK/jobs.json"
# A release may not pass with any job red, canaries included.
if [[ "${PUBLISH_PACKAGES:-false}" == true ]]; then
    red="$(jq -r '[.[] | select(.status != "success") | .name] | join(" ")' "$WORK/jobs.json")"
    [[ -z "$red" ]] || die "gitlab.com pipeline $pipeline has jobs that did not succeed: $red"
fi
mkdir -p "$ROOT/dist"
collected=0
for name in "${LANE_JOBS[@]}"; do
    job="$(jq -er --arg n "$name" '[.[] | select(.name == $n and .status == "success")][0].id' "$WORK/jobs.json")" || \
        die "gitlab.com pipeline $pipeline has no successful $name"
    rm -rf "$WORK/unpack"; mkdir -p "$WORK/unpack"
    curl -fsSL --retry 5 --retry-all-errors --max-time 1800 -K "$WORK/auth" \
        -o "$WORK/artifacts.zip" "$API/jobs/$job/artifacts"
    python3 -I -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' \
        "$WORK/artifacts.zip" "$WORK/unpack"
    found=0
    while IFS= read -r -d '' pkg; do
        base="$(basename "$pkg")"
        [[ -f "$pkg.sha256" ]] || die "$name: $base has no .sha256"
        ( cd "$(dirname "$pkg")" && sha256sum -c --quiet "$base.sha256" ) || die "$name: $base does not match its .sha256"
        [[ ! -e "$ROOT/dist/$base" ]] || die "$base was collected twice"
        install -m 0644 "$pkg" "$pkg.sha256" "$ROOT/dist/"
        found=$((found + 1))
    done < <(find "$WORK/unpack/dist" -maxdepth 1 -type f -regextype posix-extended -regex ".*$PACKAGE_GLOB" -print0)
    (( found == 1 )) || die "$name left $found packages, expected one"
    collected=$((collected + 1))
done
(( collected == ${#LANE_JOBS[@]} )) || die "collected $collected of ${#LANE_JOBS[@]} packages"
jq -n --arg id "$pipeline" --arg sha "$pipeline_sha" --argjson n "$collected" \
    '{gitlab_com_pipeline: $id, source_sha: $sha, packages: $n}' >"$ROOT/dist/gitlab-com-lane.json"
printf 'Collected %d packages from gitlab.com pipeline %s\n' "$collected" "$pipeline"
