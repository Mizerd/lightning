#!/usr/bin/env bash
set -Eeuo pipefail

# gitlab-com-package-lane.sh against a fake gitlab.com API: which lane runs it
# accepts, what it collects from each, and that the optional arm64 packages
# never fail it.

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
LANE="$ROOT/scripts/gitlab-com-package-lane.sh"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SHA=0123456789abcdef0123456789abcdef01234567
VER=0.10.5
READ_TOKEN=read-secret-value
TRIGGER_TOKEN=trigger-secret-value
fail=0
note() { printf '  ok: %s\n' "$1"; }
bad() { printf '  FAIL: %s\n' "$1" >&2; fail=1; }

mkdir -p "$WORK/bin"
cat >"$WORK/bin/curl" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$FAKE_DIR/argv.log"
out="" hdr="" url=""
while (( $# )); do
    case "$1" in
        -o) out="$2"; shift 2 ;;
        -D) hdr="$2"; shift 2 ;;
        -K|--retry|--max-time|-X) shift 2 ;;
        --data-urlencode)
            if [[ "$2" == *@* && "$2" != *=* ]]; then
                printf '%s=%s\n' "${2%%@*}" "$(cat "${2#*@}")" >>"$FAKE_DIR/post.log"
            else
                printf '%s\n' "$2" >>"$FAKE_DIR/post.log"
            fi
            shift 2 ;;
        http*) url="$1"; shift ;;
        *) shift ;;
    esac
done
path="${url#https://gitlab.example/api/v4/projects/88402223/}"
emit() { if [[ -n "$out" ]]; then cat "$1" >"$out"; else cat "$1"; fi; }
case "$path" in
    repository/branches/main) emit "$FAKE_DIR/branch.json" ;;
    trigger/pipeline) printf '{"id": 777}\n' ;;
    pipelines/777/variables) emit "$FAKE_DIR/variables.json" ;;
    pipelines/777) emit "$FAKE_DIR/pipeline.json" ;;
    'pipelines/777/jobs?per_page=100')
        printf 'HTTP/2 200\r\nX-Total: %s\r\n\r\n' "$(jq length "$FAKE_DIR/jobs.json")" >"$hdr"
        emit "$FAKE_DIR/jobs.json" ;;
    jobs/*/artifacts)
        id="${path#jobs/}"; id="${id%/artifacts}"
        [[ -f "$FAKE_DIR/artifacts/$id.zip" ]] || exit 22
        emit "$FAKE_DIR/artifacts/$id.zip" ;;
    *) exit 22 ;;
esac
EOF
chmod +x "$WORK/bin/curl"

AMD64=(deb:lightning_${VER}_amd64.deb deb-ubuntu:lightning_${VER}_ubuntu2604_amd64.deb
       rpm:lightning-${VER}-1.x86_64.rpm appimage:Lightning-${VER}-x86_64.AppImage
       snap:lightning_${VER}_amd64.snap flatpak:lightning_${VER}_amd64.flatpak)
ARM64=(deb:lightning_${VER}_arm64.deb deb-ubuntu:lightning_${VER}_ubuntu2604_arm64.deb
       rpm:lightning-${VER}-1.aarch64.rpm appimage:Lightning-${VER}-aarch64.AppImage
       snap:lightning_${VER}_arm64.snap)
WINDOWS=(Lightning-${VER}-${SHA:0:7}-windows-x86_64-portable.zip
         Lightning-${VER}-${SHA:0:7}-windows-x86_64.msi
         Lightning-${VER}-${SHA:0:7}-windows-x86_64-setup.exe)

zipdir() {
    python3 -I -c 'import os, sys, zipfile
z = zipfile.ZipFile(sys.argv[2], "w")
for d, _, fs in os.walk(sys.argv[1]):
    for f in fs:
        p = os.path.join(d, f)
        z.write(p, os.path.relpath(p, sys.argv[1]))' "$1" "$2"
}

# A green lane for $SHA on main; individual cases then break one thing.
setup() {
    FAKE_DIR="$WORK/fake.$RANDOM.$RANDOM"
    export FAKE_DIR
    mkdir -p "$FAKE_DIR/artifacts" "$FAKE_DIR/project"
    printf '{"commit": {"id": "%s"}}\n' "$SHA" >"$FAKE_DIR/branch.json"
    printf '{"id": 777, "status": "success", "sha": "%s", "ref": "main"}\n' "$SHA" >"$FAKE_DIR/pipeline.json"
    jq -n '[{key: "PACKAGE_LANE", value: "1"}, {key: "RUN_TESTS", value: "0"},
            {key: "PUBLISH_PACKAGES", value: "false"}]' >"$FAKE_DIR/variables.json"
    local id=100 entry fmt file stage jobs='[]'
    add_job() { jobs="$(jq -c --argjson id "$1" --arg n "$2" '. + [{id: $id, name: $n, status: "success", allow_failure: false}]' <<<"$jobs")"; }
    local -a list
    for arch in amd64 arm64; do
        if [[ "$arch" == amd64 ]]; then list=("${AMD64[@]}"); else list=("${ARM64[@]}"); fi
        for entry in "${list[@]}"; do
            fmt="${entry%%:*}" file="${entry#*:}"
            id=$((id + 1)); stage="$FAKE_DIR/stage/$id/dist"; mkdir -p "$stage"
            printf '%s package bytes\n' "$file" >"$stage/$file"
            printf 'log\n' >"$stage/$fmt-validate.log"
            ( cd "$stage" && sha256sum "$file" >"$file.sha256" )
            zipdir "$FAKE_DIR/stage/$id" "$FAKE_DIR/artifacts/$id.zip"
            add_job "$id" "validate-$fmt-$arch"
            id=$((id + 1)); add_job "$id" "build-$fmt-$arch"
        done
    done
    id=$((id + 1)); stage="$FAKE_DIR/stage/$id/dist/windows"; mkdir -p "$stage/reports"
    for file in "${WINDOWS[@]}"; do
        printf '%s bytes\n' "$file" >"$stage/$file"
        ( cd "$stage" && sha256sum "$file" >"$file.sha256" )
    done
    zipdir "$FAKE_DIR/stage/$id" "$FAKE_DIR/artifacts/$id.zip"
    add_job "$id" build-windows-amd64
    for name in resolve-source copr-srpm build-rpm-opensuse-amd64 validate-rpm-leap-amd64 validate-rpm-opensuse-amd64; do
        id=$((id + 1)); add_job "$id" "$name"
    done
    printf '%s\n' "$jobs" >"$FAKE_DIR/jobs.json"
    : >"$FAKE_DIR/argv.log"; : >"$FAKE_DIR/post.log"
}
set_job() { # name status [allow_failure]
    jq --arg n "$1" --arg s "$2" --argjson a "${3:-false}" \
        'map(if .name == $n then .status = $s | .allow_failure = $a else . end)' \
        "$FAKE_DIR/jobs.json" >"$FAKE_DIR/jobs.tmp" && mv "$FAKE_DIR/jobs.tmp" "$FAKE_DIR/jobs.json"
}
set_pipeline() { jq "$1" "$FAKE_DIR/pipeline.json" >"$FAKE_DIR/p.tmp" && mv "$FAKE_DIR/p.tmp" "$FAKE_DIR/pipeline.json"; }
add_variable() {
    jq --arg k "$1" --arg v "$2" '. + [{key: $k, value: $v}]' "$FAKE_DIR/variables.json" >"$FAKE_DIR/v.tmp" \
        && mv "$FAKE_DIR/v.tmp" "$FAKE_DIR/variables.json"
}
# Runs the collector in a fresh project dir; extra NAME=VALUE arguments go to env.
lane() {
    rm -rf "$FAKE_DIR/project"; mkdir -p "$FAKE_DIR/project"
    env -u GITLAB_COM_PIPELINE_ID -u GITLAB_COM_COLLECT -u GITLAB_COM_REF \
        PATH="$WORK/bin:$PATH" CI_PROJECT_DIR="$FAKE_DIR/project" \
        GITLAB_COM_API=https://gitlab.example/api/v4 GITLAB_COM_PROJECT_ID=88402223 \
        GITLAB_COM_READ_TOKEN="$READ_TOKEN" GITLAB_COM_TRIGGER_TOKEN="$TRIGGER_TOKEN" \
        SOURCE_REF="$SHA" PUBLISH_PACKAGES=false RELEASE_ACTION= RELEASE_VERSION= \
        "$@" "$LANE" >"$FAKE_DIR/out.log" 2>&1
}
dist() { printf '%s/project/dist' "$FAKE_DIR"; }
no_leak() {
    ! grep -qF -e "$READ_TOKEN" -e "$TRIGGER_TOKEN" "$FAKE_DIR/argv.log" "$FAKE_DIR/out.log"
}

printf '== a green lane: every group collected ==\n'
setup
if lane GITLAB_COM_PIPELINE_ID=777; then note "collects from a green lane"; else bad "green lane refused: $(tail -1 "$FAKE_DIR/out.log")"; fi
[[ "$(find "$(dist)" -maxdepth 1 -regextype posix-extended -regex '.*\.(deb|rpm|AppImage|snap|flatpak)$' | wc -l)" == 11 ]] \
    && note "eleven Linux packages in dist/" || bad "Linux package count"
[[ "$(find "$(dist)/windows" -maxdepth 1 -type f | wc -l)" == 3 ]] && note "three Windows artifacts in dist/windows/" || bad "Windows artifact count"
( cd "$(dist)" && sha256sum -c --quiet ./*.sha256 ) && note "each Linux package arrives with its own .sha256" || bad "Linux .sha256 files"
[[ -z "$(find "$(dist)" -name '*.log')" ]] && note "validator logs stay behind" || bad "a validator log was collected"
[[ "$(cat "$(dist)/gitlab-com-lane.env")" == GITLAB_COM_LANE_PIPELINE=777 ]] && note "the lane pipeline id is handed on" || bad "gitlab-com-lane.env"
[[ "$(jq '.packages | length' "$(dist)/gitlab-com-lane-linux-amd64.json")" == 6 \
   && "$(jq '.packages | length' "$(dist)/gitlab-com-lane-linux-arm64.json")" == 5 \
   && "$(jq '.packages | length' "$(dist)/gitlab-com-lane-windows.json")" == 3 ]] \
    && note "one record per group, counting its packages" || bad "group records"
no_leak && note "no token on a command line or in the log" || bad "a token leaked"

printf '== one group per call ==\n'
setup
lane GITLAB_COM_PIPELINE_ID=777 GITLAB_COM_COLLECT=windows || bad "windows-only collection failed"
[[ -z "$(find "$(dist)" -maxdepth 1 -name 'lightning*')" && "$(find "$(dist)/windows" -type f | wc -l)" == 3 ]] \
    && note "windows collects the Windows artifacts and nothing else" || bad "windows collected the wrong files"
lane GITLAB_COM_PIPELINE_ID=777 GITLAB_COM_COLLECT=linux-arm64 || bad "arm64-only collection failed"
[[ "$(find "$(dist)" -maxdepth 1 -name '*.sha256' | wc -l)" == 5 ]] && ! ls "$(dist)" | grep -qE 'amd64|x86_64' \
    && note "linux-arm64 collects only arm64 packages" || bad "linux-arm64 collected the wrong files"
lane GITLAB_COM_PIPELINE_ID=777 GITLAB_COM_COLLECT=none || bad "none failed"
[[ -z "$(find "$(dist)" -type f ! -name gitlab-com-lane.env)" ]] && note "none collects nothing" || bad "none collected files"
if lane GITLAB_COM_PIPELINE_ID=777 GITLAB_COM_COLLECT=none,windows; then bad "none combined with a group accepted"; else note "none cannot be combined with a group"; fi
if lane GITLAB_COM_PIPELINE_ID=777 GITLAB_COM_COLLECT=linux; then bad "unknown group accepted"; else note "an unknown group is refused"; fi

printf '== arm64 is optional ==\n'
setup
set_job validate-snap-arm64 failed; set_job validate-rpm-arm64 skipped; set_pipeline '.status = "failed"'
if lane GITLAB_COM_PIPELINE_ID=777; then note "a lane failed only by arm64 is accepted"; else bad "arm64 failure refused: $(tail -1 "$FAKE_DIR/out.log")"; fi
[[ "$(jq -c '.missing_jobs' "$(dist)/gitlab-com-lane-linux-arm64.json")" == '["validate-rpm-arm64","validate-snap-arm64"]' \
   && "$(jq '.packages | length' "$(dist)/gitlab-com-lane-linux-arm64.json")" == 3 ]] \
    && note "the arm64 record names what is missing and keeps what validated" || bad "arm64 record"
grep -q 'optional: .* has no successful validate-snap-arm64' "$FAKE_DIR/out.log" && note "a missing arm64 package is logged" || bad "missing arm64 not logged"
if lane GITLAB_COM_PIPELINE_ID=777 PUBLISH_PACKAGES=true RELEASE_ACTION=create RELEASE_VERSION=$VER; then
    bad "a release lane whose variables differ was accepted"
else
    note "a release cannot adopt a build-only lane"
fi
jq 'map(if .key == "PUBLISH_PACKAGES" then .value = "true" else . end) + [{key: "RELEASE_VERSION", value: "'$VER'"}]' \
    "$FAKE_DIR/variables.json" >"$FAKE_DIR/v.tmp" && mv "$FAKE_DIR/v.tmp" "$FAKE_DIR/variables.json"
if lane GITLAB_COM_PIPELINE_ID=777 PUBLISH_PACKAGES=true RELEASE_ACTION=create RELEASE_VERSION=$VER GITLAB_COM_REF=main; then
    note "a release accepts a lane whose only red jobs are arm64"
else
    bad "release refused an arm64-only failure: $(tail -1 "$FAKE_DIR/out.log")"
fi

printf '== everything but arm64 is required ==\n'
setup
set_job validate-deb-amd64 failed; set_pipeline '.status = "failed"'
if lane GITLAB_COM_PIPELINE_ID=777; then bad "a failed amd64 validator was accepted"; else
    grep -q 'failed outside the optional arm64 lane: validate-deb-amd64' "$FAKE_DIR/out.log" \
        && note "a failed amd64 validator fails the collection" || bad "amd64 failure refused for the wrong reason"
fi
setup
set_job build-windows-amd64 failed; set_pipeline '.status = "failed"'
if lane GITLAB_COM_PIPELINE_ID=777 GITLAB_COM_COLLECT=windows; then bad "a failed Windows build was accepted"; else note "a failed Windows build fails the collection"; fi
setup
set_job copr-srpm failed true
lane GITLAB_COM_PIPELINE_ID=777 && note "a build-only lane tolerates a red canary" || bad "build-only refused a red canary"
jq 'map(if .key == "PUBLISH_PACKAGES" then .value = "true" else . end) + [{key: "RELEASE_VERSION", value: "'$VER'"}]' \
    "$FAKE_DIR/variables.json" >"$FAKE_DIR/v.tmp" && mv "$FAKE_DIR/v.tmp" "$FAKE_DIR/variables.json"
if lane GITLAB_COM_PIPELINE_ID=777 PUBLISH_PACKAGES=true RELEASE_ACTION=create RELEASE_VERSION=$VER; then
    bad "a release accepted a red canary"
else
    grep -q 'jobs that did not succeed: copr-srpm' "$FAKE_DIR/out.log" && note "a release refuses a red canary" || bad "red canary refused for the wrong reason"
fi
setup
set_pipeline '.status = "canceled"'
if lane GITLAB_COM_PIPELINE_ID=777; then bad "a canceled lane was accepted"; else note "a canceled lane is refused"; fi

printf '== the lane must be the one asked for ==\n'
setup; set_pipeline '.sha = "ffffffffffffffffffffffffffffffffffffffff"'
if lane GITLAB_COM_PIPELINE_ID=777; then bad "a lane for another commit was accepted"; else note "another commit is refused"; fi
setup; set_pipeline '.ref = "feature"'
if lane GITLAB_COM_PIPELINE_ID=777; then bad "a lane on another branch was accepted"; else note "another branch is refused"; fi
lane GITLAB_COM_PIPELINE_ID=777 GITLAB_COM_REF=feature && note "a build-only run may name another branch" || bad "build-only branch override refused"
if lane GITLAB_COM_PIPELINE_ID=777 GITLAB_COM_REF=feature PUBLISH_PACKAGES=true RELEASE_ACTION=create RELEASE_VERSION=$VER; then
    bad "a release built off main was accepted"
else
    grep -q "protected main" "$FAKE_DIR/out.log" && note "a release builds only from gitlab.com main" || bad "off-main release refused for the wrong reason"
fi
setup; add_variable PACKAGE_ARCH amd64
if lane GITLAB_COM_PIPELINE_ID=777; then bad "a narrowed lane was accepted"; else note "a narrowed lane is refused"; fi
setup; add_variable LIGHTNING_REPOSITORY https://example.invalid/x.git
if lane GITLAB_COM_PIPELINE_ID=777; then bad "a lane with a substituted repository was accepted"; else note "a substituted repository is refused"; fi
setup; jq 'map(select(.key != "PACKAGE_LANE"))' "$FAKE_DIR/variables.json" >"$FAKE_DIR/v.tmp" && mv "$FAKE_DIR/v.tmp" "$FAKE_DIR/variables.json"
if lane GITLAB_COM_PIPELINE_ID=777; then bad "a non-lane pipeline was accepted"; else note "a pipeline that is not a lane run is refused"; fi

printf '== bytes ==\n'
setup
id="$(jq -r '.[] | select(.name == "validate-rpm-amd64") | .id' "$FAKE_DIR/jobs.json")"
printf 'tampered\n' >>"$FAKE_DIR/stage/$id/dist/lightning-${VER}-1.x86_64.rpm"
zipdir "$FAKE_DIR/stage/$id" "$FAKE_DIR/artifacts/$id.zip.new" && mv "$FAKE_DIR/artifacts/$id.zip.new" "$FAKE_DIR/artifacts/$id.zip"
if lane GITLAB_COM_PIPELINE_ID=777; then bad "a package that does not match its .sha256 was accepted"; else note "a checksum mismatch is refused"; fi
setup
id="$(jq -r '.[] | select(.name == "validate-deb-amd64") | .id' "$FAKE_DIR/jobs.json")"
printf 'second\n' >"$FAKE_DIR/stage/$id/dist/extra.deb"; ( cd "$FAKE_DIR/stage/$id/dist" && sha256sum extra.deb >extra.deb.sha256 )
zipdir "$FAKE_DIR/stage/$id" "$FAKE_DIR/artifacts/$id.zip.new" && mv "$FAKE_DIR/artifacts/$id.zip.new" "$FAKE_DIR/artifacts/$id.zip"
if lane GITLAB_COM_PIPELINE_ID=777; then bad "a validator with two packages was accepted"; else note "exactly one package per validator"; fi

printf '== triggering ==\n'
setup
lane GITLAB_COM_COLLECT=none || bad "triggered run failed: $(tail -1 "$FAKE_DIR/out.log")"
grep -qx "token=$TRIGGER_TOKEN" "$FAKE_DIR/post.log" && note "the trigger token goes in the request body" || bad "trigger token not posted"
for kv in ref=main "variables[PACKAGE_LANE]=1" "variables[SOURCE_REF]=$SHA" "variables[PUBLISH_PACKAGES]=false"; do
    grep -qxF "$kv" "$FAKE_DIR/post.log" && note "the trigger asks for $kv" || bad "trigger lacks $kv"
done
no_leak && note "neither token reaches a command line" || bad "a token leaked when triggering"
setup
if lane GITLAB_COM_COLLECT=linux-amd64 GITLAB_COM_TRIGGER_TOKEN=; then
    bad "a collector without a pipeline id and trigger token ran"
else
    [[ ! -s "$FAKE_DIR/post.log" ]] && note "without a pipeline id or trigger token nothing is triggered" || bad "a pipeline was triggered"
fi
if lane SOURCE_REF=main GITLAB_COM_PIPELINE_ID=777; then bad "a branch name was accepted as SOURCE_REF"; else note "SOURCE_REF must be a commit"; fi

if (( fail )); then
    printf 'gitlab.com lane collector tests FAILED\n' >&2
    exit 1
fi
printf 'gitlab.com lane collector tests passed\n'
