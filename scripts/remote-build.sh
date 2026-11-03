#!/usr/bin/env bash
# Build the pushed main on gitlab.com's 32 vCPU runner and fetch the binary.
# Needs ~/.config/lightning/gitlab-trigger and gitlab-com-token.
# Usage: scripts/remote-build.sh [ref] [--test]; the binary lands in
# build-remote/. --test also runs ctest and cargo test as parallel jobs; logs land there too.
set -euo pipefail
cd "$(dirname "$0")/.."
ref=${1:-main}
tests=0
[ "${2:-}" = --test ] && tests=1
api=https://gitlab.com/api/v4/projects/88402223
conf=${XDG_CONFIG_HOME:-$HOME/.config}/lightning
token=$(cat "$conf/gitlab-trigger")
auth=$(mktemp)
trap 'rm -f "$auth"' EXIT
printf 'header = "PRIVATE-TOKEN: %s"\n' "$(tr -d '\r\n ' < "$conf/gitlab-com-token")" > "$auth"
id=$(curl -fsS -X POST -F "token=$token" -F "ref=$ref" -F "variables[RUN_TESTS]=$tests" "$api/trigger/pipeline" |
    python3 -c 'import sys,json;print(json.load(sys.stdin)["id"])')
echo "pipeline $id: https://gitlab.com/lightning-matrix/lightning/-/pipelines/$id"
while :; do
    status=$(curl -fsS -K "$auth" "$api/pipelines/$id" |
        python3 -c 'import sys,json;print(json.load(sys.stdin)["status"])')
    case $status in
    success) break ;;
    failed | canceled | skipped) break ;;
    esac
    sleep 30
done
mkdir -p build-remote
jobs=$(curl -fsS -K "$auth" "$api/pipelines/$id/jobs" |
    python3 -c 'import sys,json;[print(j["id"],j["name"],j["status"]) for j in json.load(sys.stdin)]')
echo "$jobs"
while read -r job name state; do
    if [ "$name" = build-nixos ] && [ "$state" = success ]; then
        curl -fsSL -K "$auth" "$api/jobs/$job/artifacts/out/lightning-matrix" -o build-remote/lightning-matrix
        chmod +x build-remote/lightning-matrix
    elif [ "$name" = test-nixos ]; then
        curl -fsSL -K "$auth" "$api/jobs/$job/artifacts/out/ctest.log" -o build-remote/ctest.log || true
        curl -fsSL -K "$auth" "$api/jobs/$job/trace" -o build-remote/test-job.log || true
    elif [ "$name" = test-rust ]; then
        curl -fsSL -K "$auth" "$api/jobs/$job/artifacts/out/cargo.log" -o build-remote/cargo.log || true
    fi
done <<< "$jobs"
[ "$status" = success ] || { echo "pipeline $status" >&2; exit 1; }
echo "run: LIGHTNING_DEV_BINARY=./build-remote/lightning-matrix scripts/run-dev.sh"
