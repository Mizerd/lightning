#!/usr/bin/env bash
# Build the pushed main on gitlab.com's 32 vCPU runner and fetch the binary.
# Needs a pipeline trigger token in ~/.config/lightning/gitlab-trigger.
# Usage: scripts/remote-build.sh [ref]; the binary lands in build-remote/.
set -euo pipefail
cd "$(dirname "$0")/.."
ref=${1:-main}
api=https://gitlab.com/api/v4/projects/88402223
token=$(cat "${XDG_CONFIG_HOME:-$HOME/.config}/lightning/gitlab-trigger")
id=$(curl -fsS -X POST -F "token=$token" -F "ref=$ref" "$api/trigger/pipeline" |
    python3 -c 'import sys,json;print(json.load(sys.stdin)["id"])')
echo "pipeline $id: https://gitlab.com/lightning-matrix/lightning/-/pipelines/$id"
while :; do
    status=$(curl -fsS "$api/pipelines/$id" |
        python3 -c 'import sys,json;print(json.load(sys.stdin)["status"])')
    case $status in
    success) break ;;
    failed | canceled | skipped) echo "pipeline $status" >&2; exit 1 ;;
    esac
    sleep 30
done
job=$(curl -fsS "$api/pipelines/$id/jobs" | python3 -c 'import sys,json;print(json.load(sys.stdin)[0]["id"])')
mkdir -p build-remote
curl -fsSL "$api/jobs/$job/artifacts/out/lightning-matrix" -o build-remote/lightning-matrix
chmod +x build-remote/lightning-matrix
echo "run: LIGHTNING_DEV_BINARY=./build-remote/lightning-matrix scripts/run-dev.sh"
