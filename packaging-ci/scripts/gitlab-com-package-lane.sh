#!/usr/bin/env bash
# Run the gitlab.com package lane (.gitlab/packages.yml) for SOURCE_REF, wait
# for it, and collect its validated packages into dist/ exactly as the local
# validate-* and build-windows jobs leave them.
#
#   GITLAB_COM_PROJECT_ID     the mirror project (88402223)
#   GITLAB_COM_TRIGGER_TOKEN  pipeline trigger token of that project
#   GITLAB_COM_READ_TOKEN     read_api token of that project (job artifacts)
#   SOURCE_REF                full commit SHA; the mirror's GITLAB_COM_REF must
#                             point at it, so scripts and source are one commit
#   GITLAB_COM_PIPELINE_ID    optional: adopt this pipeline instead of
#                             triggering one
#   GITLAB_COM_COLLECT        comma list of linux-amd64, linux-arm64, windows,
#                             macos, or none (default: all four)
#
# Only https://gitlab.com/api/v4 and project 88402223 are accepted, unless
# GITLAB_COM_FAKE_API_FOR_TESTS=1 (tests/test-gitlab-com-lane.sh).
#
# linux-amd64 and windows are required: each of their packages must have
# validated. linux-arm64 is optional: whatever validated is collected, and a
# red arm64 job neither fails the lane nor this script. macos is required in a
# release or when BUILD_MACOS_PACKAGES=true, and optional when only
# PACKAGE_FORMATS names it.
#
# Writes dist/gitlab-com-lane.env (GITLAB_COM_LANE_PIPELINE=<id>) and, per
# collected group, dist/gitlab-com-lane-<group>.json.
#
# PUBLISH_PACKAGES, RELEASE_ACTION and RELEASE_VERSION are passed through, so a
# release gets an official (key-embedding) build; the lane's resolve-source runs
# validate-release-request.sh on them. Nothing is published there.
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT="$(project_dir)"

: "${GITLAB_COM_API:=https://gitlab.com/api/v4}"
: "${GITLAB_COM_PROJECT_ID:=88402223}"
: "${GITLAB_COM_REF:=main}"
# Above the lane's critical path: resolve 15m, build 3h, snap 1h, its validator
# 1h, plus queueing.
: "${GITLAB_COM_TIMEOUT_S:=25200}"
: "${GITLAB_COM_COLLECT:=linux-amd64,linux-arm64,windows,macos}"
require_var GITLAB_COM_READ_TOKEN
require_var SOURCE_REF
[[ "$SOURCE_REF" =~ ^[0-9a-f]{40}$ ]] || die "SOURCE_REF must be a full commit SHA"
[[ "$GITLAB_COM_PROJECT_ID" =~ ^[0-9]+$ ]] || die "GITLAB_COM_PROJECT_ID must be numeric"
# Both tokens go to this host, and its answers decide what is published.
if [[ "${GITLAB_COM_FAKE_API_FOR_TESTS:-}" != 1 ]]; then
    [[ "$GITLAB_COM_API" == https://gitlab.com/api/v4 && "$GITLAB_COM_PROJECT_ID" == 88402223 ]] || \
        die "GITLAB_COM_API/GITLAB_COM_PROJECT_ID must be https://gitlab.com/api/v4 and 88402223"
fi
[[ "$GITLAB_COM_REF" =~ ^[A-Za-z0-9][A-Za-z0-9._/-]{0,254}$ ]] || die "GITLAB_COM_REF is not a plausible branch name"
if [[ "${PUBLISH_PACKAGES:-false}" == true && "$GITLAB_COM_REF" != main ]]; then
    die "a release builds only from gitlab.com's protected main, not $GITLAB_COM_REF"
fi

declare -A collect=()
IFS=',' read -r -a _groups <<<"$GITLAB_COM_COLLECT"
for group in "${_groups[@]}"; do
    case "$group" in
        linux-amd64|linux-arm64|windows|macos) collect[$group]=1 ;;
        none) ;;
        *) die "GITLAB_COM_COLLECT: unknown group '$group'" ;;
    esac
done
if [[ -n "${collect[*]:-}" && ",$GITLAB_COM_COLLECT," == *,none,* ]]; then
    die "GITLAB_COM_COLLECT cannot combine none with a group"
fi

API="$GITLAB_COM_API/projects/$GITLAB_COM_PROJECT_ID"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
# Tokens reach curl through a 0600 config file, never the command line. An
# Authorization header, unlike PRIVATE-TOKEN, is dropped when the artifact
# download redirects to object storage.
( umask 077; printf 'header = "Authorization: Bearer %s"\n' "$GITLAB_COM_READ_TOKEN" >"$WORK/auth" )
api() { curl -fsS --retry 5 --retry-all-errors --max-time 120 -K "$WORK/auth" "$API/$1"; }

# Every Linux package the release publishes, by the job that validated it.
AMD64_JOBS=(
    validate-deb-amd64 validate-deb-ubuntu-amd64 validate-rpm-amd64
    validate-appimage-amd64 validate-snap-amd64 validate-flatpak-amd64
)
ARM64_JOBS=(
    validate-deb-arm64 validate-deb-ubuntu-arm64 validate-rpm-arm64
    validate-appimage-arm64 validate-snap-arm64
)
PACKAGE_GLOB='\.(deb|rpm|AppImage|snap|flatpak)$'
# The architecture each group's package names must carry, so one group can
# never deliver a file that overwrites another's.
declare -A ARCH_NAME=(
    [linux-amd64]='(_amd64\.(deb|flatpak|snap)|\.x86_64\.rpm|-x86_64\.AppImage)$'
    [linux-arm64]='(_arm64\.(deb|snap)|\.aarch64\.rpm|-aarch64\.AppImage)$'
)

pipeline="${GITLAB_COM_PIPELINE_ID:-}"
if [[ -z "$pipeline" ]]; then
    require_var GITLAB_COM_TRIGGER_TOKEN
    deadline=$(( $(date +%s) + 900 ))
    until [[ "$(api "repository/branches/$GITLAB_COM_REF" | jq -r .commit.id)" == "$SOURCE_REF" ]]; do
        (( $(date +%s) < deadline )) || \
            die "gitlab.com $GITLAB_COM_REF never reached $SOURCE_REF; the push mirror has not caught up, or $GITLAB_COM_REF has moved past it"
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
        --data-urlencode "variables[BUILD_MACOS_PACKAGES]=${BUILD_MACOS_PACKAGES:-false}" \
        --data-urlencode "variables[RELEASE_ACTION]=${RELEASE_ACTION:-}" \
        --data-urlencode "variables[RELEASE_VERSION]=${RELEASE_VERSION:-}" \
        "$API/trigger/pipeline" | jq -er .id)"
fi
[[ "$pipeline" =~ ^[0-9]+$ ]] || die "no gitlab.com pipeline id"
printf 'gitlab.com package lane: pipeline %s\n' "$pipeline"
mkdir -p "$ROOT/dist"
printf 'GITLAB_COM_LANE_PIPELINE=%s\n' "$pipeline" >"$ROOT/dist/gitlab-com-lane.env"

# An adopted pipeline must have been asked for exactly what this run needs.
api "pipelines/$pipeline/variables" >"$WORK/variables.json"
var() { jq -r --arg k "$1" '[.[] | select(.key == $k) | .value][0] // ""' "$WORK/variables.json"; }
[[ "$(var PACKAGE_LANE)" == 1 ]] || die "pipeline $pipeline was not a package lane run"
[[ "$(var PUBLISH_PACKAGES)" == "${PUBLISH_PACKAGES:-false}" || \
   ( -z "$(var PUBLISH_PACKAGES)" && "${PUBLISH_PACKAGES:-false}" == false ) ]] || \
    die "pipeline $pipeline ran with PUBLISH_PACKAGES=$(var PUBLISH_PACKAGES)"
[[ "$(var BUILD_MACOS_PACKAGES)" == "${BUILD_MACOS_PACKAGES:-false}" || \
   ( -z "$(var BUILD_MACOS_PACKAGES)" && "${BUILD_MACOS_PACKAGES:-false}" == false ) ]] || \
    die "pipeline $pipeline ran with BUILD_MACOS_PACKAGES=$(var BUILD_MACOS_PACKAGES)"
[[ "$(var RELEASE_VERSION)" == "${RELEASE_VERSION:-}" ]] || \
    die "pipeline $pipeline ran with RELEASE_VERSION=$(var RELEASE_VERSION)"
if [[ "${PUBLISH_PACKAGES:-false}" == true ]]; then
    [[ "$(var RELEASE_ACTION)" == "${RELEASE_ACTION:-}" ]] || \
        die "pipeline $pipeline ran with RELEASE_ACTION=$(var RELEASE_ACTION)"
fi
for narrowing in PACKAGE_ARCH PACKAGE_FORMATS; do
    [[ "$(var "$narrowing")" == "" || "$(var "$narrowing")" == all ]] || \
        die "pipeline $pipeline was narrowed by $narrowing=$(var "$narrowing")"
done
[[ -z "$(var SOURCE_REF)" || "$(var SOURCE_REF)" == "$SOURCE_REF" ]] || \
    die "pipeline $pipeline ran with SOURCE_REF=$(var SOURCE_REF)"
for forbidden in LIGHTNING_SOURCE_FROM_CHECKOUT LIGHTNING_REPOSITORY; do
    [[ -z "$(jq -r --arg k "$forbidden" '.[] | select(.key == $k) | .key' "$WORK/variables.json")" ]] || \
        die "pipeline $pipeline overrides $forbidden"
done

check_identity() {
    pipeline_sha="$(jq -er .sha "$WORK/pipeline.json")"
    [[ "$pipeline_sha" == "$SOURCE_REF" ]] || die "gitlab.com pipeline $pipeline built $pipeline_sha, not $SOURCE_REF"
    pipeline_ref="$(jq -er .ref "$WORK/pipeline.json")"
    [[ "$pipeline_ref" == "$GITLAB_COM_REF" ]] || die "gitlab.com pipeline $pipeline ran on $pipeline_ref, not $GITLAB_COM_REF"
}
api "pipelines/$pipeline" >"$WORK/pipeline.json"
check_identity

deadline=$(( $(date +%s) + GITLAB_COM_TIMEOUT_S ))
while :; do
    api "pipelines/$pipeline" >"$WORK/pipeline.json"
    status="$(jq -er .status "$WORK/pipeline.json")"
    case "$status" in
        success|failed) break ;;
        canceled|skipped) die "gitlab.com pipeline $pipeline ended $status" ;;
    esac
    (( $(date +%s) < deadline )) || die "gitlab.com pipeline $pipeline still $status after ${GITLAB_COM_TIMEOUT_S}s"
    sleep 60
done
check_identity

curl -fsS --retry 5 --retry-all-errors --max-time 120 -K "$WORK/auth" -D "$WORK/jobs.headers" \
    -o "$WORK/jobs.json" "$API/pipelines/$pipeline/jobs?per_page=100"
total="$(tr -d '\r' <"$WORK/jobs.headers" | awk -F': ' 'tolower($1) == "x-total" { print $2 }')"
[[ "$total" =~ ^[0-9]+$ ]] && (( total <= 100 )) || die "pipeline $pipeline lists '$total' jobs; expected at most one page"
(( $(jq length "$WORK/jobs.json") == total )) || die "pipeline $pipeline's job list is incomplete"
# Only the arm64 lane, and an unrequested macOS build, may have failed the
# pipeline.
publishing=false
[[ "${PUBLISH_PACKAGES:-false}" != true ]] || publishing=true
# A release or an explicit BUILD_MACOS_PACKAGES request must get a green macOS build.
macos_required=$publishing
[[ "${BUILD_MACOS_PACKAGES:-false}" != true ]] || macos_required=true
if [[ "$status" == failed ]]; then
    red="$(jq -r --argjson pub "$macos_required" '[.[] | select(.status == "failed" and .allow_failure != true
        and (.name | endswith("-arm64") | not) and ($pub or .name != "build-macos")) | .name] | join(" ")' "$WORK/jobs.json")"
    [[ -z "$red" ]] || die "gitlab.com pipeline $pipeline failed outside the optional arm64 lane: $red"
fi
# A release may not pass with any job red, canaries included; arm64 excepted.
if [[ "${PUBLISH_PACKAGES:-false}" == true ]]; then
    red="$(jq -r '[.[] | select(.status != "success" and (.name | endswith("-arm64") | not)) | .name] | join(" ")' "$WORK/jobs.json")"
    [[ -z "$red" ]] || die "gitlab.com pipeline $pipeline has jobs that did not succeed: $red"
fi

job_id() {
    jq -er --arg n "$1" '[.[] | select(.name == $n and .status == "success")][0].id' "$WORK/jobs.json"
}

fetch_artifacts() {
    rm -rf "$WORK/unpack"; mkdir -p "$WORK/unpack"
    curl -fsSL --retry 5 --retry-all-errors --max-time 1800 -K "$WORK/auth" \
        -o "$WORK/artifacts.zip" "$API/jobs/$1/artifacts"
    python3 -I -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' \
        "$WORK/artifacts.zip" "$WORK/unpack"
}

# Checks $1 against its own .sha256 and installs it into $2 (and the .sha256
# too when $3 is "with-sum").
take() {
    local pkg="$1" dest="$2" job="$3" base named
    base="$(basename "$pkg")"
    [[ -f "$pkg.sha256" ]] || die "$job: $base has no .sha256"
    read -r _ named <"$pkg.sha256"
    [[ "${named#\*}" == "$base" ]] || die "$job: $base.sha256 names '$named'"
    ( cd "$(dirname "$pkg")" && sha256sum -c --quiet "$base.sha256" ) || die "$job: $base does not match its .sha256"
    [[ ! -e "$dest/$base" ]] || die "$base was collected twice"
    install -m 0644 "$pkg" "$dest/"
    [[ "${4:-}" != with-sum ]] || install -m 0644 "$pkg.sha256" "$dest/"
    taken+=("$base")
}

# Collects one package from each job; a job without a successful run is fatal
# unless $1 is "optional", when it is recorded as missing.
collect_linux() {
    local mode="$1" group="$2"; shift 2
    local name job found pkg
    taken=() missing=()
    for name in "$@"; do
        if ! job="$(job_id "$name")"; then
            [[ "$mode" == optional ]] || die "gitlab.com pipeline $pipeline has no successful $name"
            printf 'optional: gitlab.com pipeline %s has no successful %s; its package is left out\n' "$pipeline" "$name" >&2
            missing+=("$name")
            continue
        fi
        fetch_artifacts "$job"
        found=0
        while IFS= read -r -d '' pkg; do
            [[ "$(basename "$pkg")" =~ ${ARCH_NAME[$group]} ]] || \
                die "$name left $(basename "$pkg"), which is not named as a $group package"
            take "$pkg" "$ROOT/dist" "$name" with-sum
            found=$((found + 1))
        done < <(find "$WORK/unpack/dist" -maxdepth 1 -type f -regextype posix-extended -regex ".*$PACKAGE_GLOB" -print0)
        (( found == 1 )) || die "$name left $found packages, expected one"
    done
    (( ${#taken[@]} + ${#missing[@]} == $# )) || die "$group: accounted for $(( ${#taken[@]} + ${#missing[@]} )) of $# jobs"
    record "$group"
}

record() {
    jq -n --arg id "$pipeline" --arg sha "$pipeline_sha" --arg group "$1" \
        --argjson taken "$(printf '%s\n' "${taken[@]}" | jq -R . | jq -s 'map(select(length > 0))')" \
        --argjson missing "$(printf '%s\n' "${missing[@]}" | jq -R . | jq -s 'map(select(length > 0))')" \
        '{gitlab_com_pipeline: $id, source_sha: $sha, group: $group,
          packages: $taken, missing_jobs: $missing}' >"$ROOT/dist/gitlab-com-lane-$1.json"
    printf 'Collected %d %s package(s) from gitlab.com pipeline %s\n' "${#taken[@]}" "$1" "$pipeline"
}

if [[ -n "${collect[linux-amd64]:-}" ]]; then
    collect_linux required linux-amd64 "${AMD64_JOBS[@]}"
fi
if [[ -n "${collect[linux-arm64]:-}" ]]; then
    collect_linux optional linux-arm64 "${ARM64_JOBS[@]}"
fi
if [[ -n "${collect[windows]:-}" ]]; then
    taken=() missing=()
    job="$(job_id build-windows-amd64)" || die "gitlab.com pipeline $pipeline has no successful build-windows-amd64"
    fetch_artifacts "$job"
    mkdir -p "$ROOT/dist/windows"
    for pattern in '*-windows-x86_64-portable.zip' '*-windows-x86_64.msi' '*-windows-x86_64-setup.exe'; do
        mapfile -t hits < <(find "$WORK/unpack/dist/windows" -maxdepth 1 -type f -name "$pattern")
        (( ${#hits[@]} == 1 )) || die "build-windows-amd64 left ${#hits[@]} files matching $pattern, expected one"
        take "${hits[0]}" "$ROOT/dist/windows" build-windows-amd64
    done
    (( ${#taken[@]} == 3 )) || die "collected ${#taken[@]} of 3 Windows artifacts"
    record windows
fi
if [[ -n "${collect[macos]:-}" ]]; then
    taken=() missing=()
    if job="$(job_id build-macos)"; then
        fetch_artifacts "$job"
        mkdir -p "$ROOT/dist/macos"
        mapfile -t hits < <(find "$WORK/unpack/dist/macos" -maxdepth 1 -type f -name '*-macos-arm64.zip')
        (( ${#hits[@]} == 1 )) || die "build-macos left ${#hits[@]} macOS zips, expected one"
        take "${hits[0]}" "$ROOT/dist/macos" build-macos with-sum
        install -m 0644 "$WORK/unpack/dist/macos/build-info.json" "$ROOT/dist/macos/"
    else
        [[ "$macos_required" != true ]] || die "gitlab.com pipeline $pipeline has no successful build-macos"
        printf 'optional: gitlab.com pipeline %s has no successful build-macos; no macOS bundle\n' "$pipeline" >&2
        missing=(build-macos)
    fi
    record macos
fi
printf 'gitlab.com pipeline %s (%s) accepted\n' "$pipeline" "$pipeline_sha"
