#!/usr/bin/env bash
# Stateful mock of the subset of the GitLab REST API used by the publication,
# verification, and release scripts. State (uploaded package files, release
# links, release existence) is kept under MOCK_STATE_DIR so that a sequence of
# invocations behaves like a real registry and release, exercising idempotent
# retries end to end.
set -Eeuo pipefail

STATE="${MOCK_STATE_DIR:?MOCK_STATE_DIR is required}"
REG="$STATE/registry"
mkdir -p "$REG"
LINKS="$STATE/links.json"
[[ -f "$LINKS" ]] || printf '%s' "${MOCK_PRESEED_LINKS:-[]}" >"$LINKS"

output=/dev/null
method=GET
url=
upload_file=
declare -a urlenc=()
while (($#)); do
    case "$1" in
        --output) output="$2"; shift 2 ;;
        --write-out) shift 2 ;;
        --request) method="$2"; shift 2 ;;
        --upload-file) upload_file="$2"; shift 2 ;;
        --data-urlencode) urlenc+=("$2"); shift 2 ;;
        --header|--data) shift 2 ;;
        --silent|--show-error|--location) shift ;;
        http*) url="$1"; shift ;;
        *) shift ;;
    esac
done

[[ -n "${MOCK_CURL_LOG:-}" ]] && printf '%s %s\n' "$method" "$url" >>"$MOCK_CURL_LOG"

status=200
body='{}'
emit_body=1

field() { # extract urlencoded field value by key
    local key="$1" kv
    for kv in "${urlenc[@]}"; do
        [[ "$kv" == "$key="* ]] && { printf '%s' "${kv#*=}"; return; }
    done
}

sha_of() { sha256sum "$1" | cut -d' ' -f1; }
size_of() { wc -c <"$1" | tr -d ' '; }

respond() { status="$1"; body="$2"; }

# --- Generic package registry: /packages/generic/<name>/<version>/<file> ---
if [[ "$url" == *"/packages/generic/"* ]]; then
    file="${url##*/}"
    dest="$REG/$file"
    case "$method" in
        GET)
            if [[ "${MOCK_CONFLICT_FILE:-}" == "$file" && ! -f "$dest" ]]; then
                respond 200 'conflicting-registry-bytes-that-differ'
            elif [[ -f "$dest" ]]; then
                cp "$dest" "$output"; emit_body=0; status=200
            else
                respond 404 '{"message":"404 Not Found"}'
            fi
            ;;
        PUT)
            if [[ "${MOCK_FAIL_UPLOAD:-}" == "$file" ]]; then
                respond 500 '{"message":"upload failed"}'
            else
                cp "$upload_file" "$dest"; respond 201 '{"message":"201 Created"}'
            fi
            ;;
    esac
# --- Packages API listing ---
elif [[ "$url" == *"/packages?"* || "$url" == *"/packages?package_name="* ]]; then
    ver="${MOCK_RELEASE_VERSION:?}"
    if compgen -G "$REG/*" >/dev/null; then
        body="[{\"id\":42,\"name\":\"lightning\",\"version\":\"${ver}\",\"package_type\":\"generic\",\"status\":\"default\"}]"
    else
        body='[]'
    fi
elif [[ "$url" == *"/packages/42/package_files/"* && "$method" == DELETE ]]; then
    fid="${url##*/}"
    n="$(awk -v id="$fid" '$1==id{print $2}' "$STATE/fileids" 2>/dev/null || true)"
    [[ -n "$n" ]] && rm -f "$REG/$n"
    respond 204 ''
elif [[ "$url" == *"/packages/42/package_files"* && "$method" == GET ]]; then
    # Build the file list and a stable id<->name map.
    : >"$STATE/fileids"
    idx=50
    for f in "$REG"/*; do
        [[ -e "$f" ]] || continue
        n="$(basename "$f")"; idx=$((idx+1))
        printf '%s %s\n' "$idx" "$n" >>"$STATE/fileids"
    done
    body='['
    first=1
    while read -r id n; do
        [[ -n "$id" ]] || continue
        f="$REG/$n"
        [[ $first == 1 ]] || body+=','
        first=0
        body+="{\"id\":${id},\"file_name\":\"${n}\",\"size\":$(size_of "$f"),\"file_sha256\":\"$(sha_of "$f")\"}"
    done <"$STATE/fileids"
    body+=']'
# --- Repository tag ---
elif [[ "$url" == *"/repository/tags/"* ]]; then
    tag_exists="${MOCK_TAG_EXISTS:-true}"
    [[ -f "$STATE/release_created" ]] && tag_exists=true
    if [[ "$tag_exists" == true ]]; then
        respond 200 "{\"name\":\"v${MOCK_RELEASE_VERSION}\",\"commit\":{\"id\":\"${MOCK_TAG_SHA:-${MOCK_SOURCE_SHA:?}}\"}}"
    else
        respond 404 '{"message":"404 Tag Not Found"}'
    fi
# --- Commit lookup / reachability ---
elif [[ "$url" == *"/repository/commits/"*"/refs"* ]]; then
    if [[ "${MOCK_COMMIT_REACHABLE:-true}" == true ]]; then
        body='[{"type":"branch","name":"main"}]'
    else
        body='[{"type":"branch","name":"feature"}]'
    fi
elif [[ "$url" == *"/repository/commits/"* ]]; then
    body="{\"id\":\"${MOCK_SOURCE_SHA:?}\"}"
# --- Release links ---
elif [[ "$url" == *"/releases/"*"/assets/links"* && "$method" == GET ]]; then
    body="$(cat "$LINKS")"
elif [[ "$url" == *"/releases/"*"/assets/links" && "$method" == POST ]]; then
    name="$(field name)"; lurl="$(field url)"
    # Append to links state.
    tmp="$(mktemp)"
    "${MOCK_JQ:?}" --arg n "$name" --arg u "$lurl" \
        '. + [{name:$n,url:$u,link_type:"package"}]' "$LINKS" >"$tmp" && mv "$tmp" "$LINKS"
    respond 201 '{"id":1}'
# --- Release object ---
elif [[ "$url" == *"/releases" && "$method" == POST ]]; then
    # create release
    printf '1' >"$STATE/release_created"
    # seed the two links from the create payload's manifest links via state file
    if [[ -n "${MOCK_CREATE_LINKS:-}" ]]; then
        printf '%s' "$MOCK_CREATE_LINKS" >"$LINKS"
    fi
    respond 201 "{\"tag_name\":\"v${MOCK_RELEASE_VERSION}\"}"
elif [[ "$url" == *"/releases/"* ]]; then
    exists="${MOCK_RELEASE_EXISTS:-true}"
    [[ -f "$STATE/release_created" ]] && exists=true
    if [[ "$exists" == true ]]; then
        body="{\"tag_name\":\"v${MOCK_RELEASE_VERSION}\",\"name\":\"Lightning ${MOCK_RELEASE_VERSION}\",\"assets\":{\"sources\":[{\"format\":\"zip\"},{\"format\":\"tar.gz\"}],\"links\":$(cat "$LINKS")}}"
    else
        respond 404 '{"message":"404 Release Not Found"}'
    fi
else
    respond 404 '{"message":"unhandled mock URL"}'
fi

if [[ "$emit_body" == 1 && -n "$output" && "$output" != /dev/null ]]; then
    printf '%s' "$body" >"$output"
fi
printf '%s' "$status"
