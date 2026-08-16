#!/usr/bin/env bash
# Stateful mock of the subset of the GitLab REST API used by the publication,
# verification, and release scripts, plus the subset of the GitHub API used by
# the release mirror. State (uploaded package files, release links, release
# existence, mirrored assets) is kept under MOCK_STATE_DIR so that a sequence of
# invocations behaves like a real registry and release, exercising idempotent
# retries end to end.
set -Eeuo pipefail

STATE="${MOCK_STATE_DIR:?MOCK_STATE_DIR is required}"
REG="$STATE/registry"
mkdir -p "$REG"
LINKS="$STATE/links.json"
[[ -f "$LINKS" ]] || printf '%s' "${MOCK_PRESEED_LINKS:-[]}" >"$LINKS"
# The GitHub mirror's state: one file per uploaded release asset.
GH="$STATE/github"
GH_ASSETS="$GH/assets"

output=/dev/null
method=GET
url=
upload_file=
config_file=
declare -a urlenc=()
while (($#)); do
    case "$1" in
        --output) output="$2"; shift 2 ;;
        --write-out) shift 2 ;;
        --request) method="$2"; shift 2 ;;
        --upload-file) upload_file="$2"; shift 2 ;;
        --data-urlencode) urlenc+=("$2"); shift 2 ;;
        # A credential file, never a credential argument. Its CONTENT is
        # deliberately never read or logged here: the only thing the mock needs
        # to know is whether a request was authenticated at all.
        --config) config_file="$2"; shift 2 ;;
        --header|--data|--data-binary|--max-redirs) shift 2 ;;
        --silent|--show-error|--location|--fail) shift ;;
        http*) url="$1"; shift ;;
        *) shift ;;
    esac
done

if [[ -n "${MOCK_CURL_LOG:-}" ]]; then
    printf '%s %s\n' "$method" "$url" >>"$MOCK_CURL_LOG"
    # Sidecar log, so appending the auth state cannot change the shape of the
    # request log the existing publication tests parse.
    if [[ -n "$config_file" ]]; then
        printf '%s %s AUTH\n' "$method" "$url" >>"${MOCK_CURL_LOG}.auth"
    else
        printf '%s %s ANON\n' "$method" "$url" >>"${MOCK_CURL_LOG}.auth"
    fi
fi

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

# JSON array of the mirrored assets, in the shape the GitHub release object
# uses. Asset ids are positional and stable for a given state directory.
gh_assets_json() {
    local out='[' first=1 idx=900 f n
    for f in "$GH_ASSETS"/*; do
        [[ -e "$f" ]] || continue
        n="$(basename "$f")"
        idx=$((idx+1))
        [[ $first == 1 ]] || out+=','
        first=0
        out+="{\"id\":${idx},\"name\":\"${n}\",\"size\":$(size_of "$f"),\"state\":\"${MOCK_GITHUB_ASSET_STATE:-uploaded}\"}"
    done
    out+=']'
    printf '%s' "$out"
}

# --- GitHub mirror API ---
#
# Matched FIRST and by host: GitHub URLs also contain "/releases/", which the
# GitLab branches below would otherwise claim.
if [[ "$url" == https://api.github.com/* || "$url" == https://uploads.github.com/* \
      || "$url" == https://github.com/* ]]; then
    mkdir -p "$GH_ASSETS"
    if [[ "$url" == *"/git/ref/tags/"* ]]; then
        if [[ "${MOCK_GITHUB_TAG_UNAUTHORIZED:-false}" == true ]]; then
            respond 401 '{"message":"Bad credentials"}'
        elif [[ "${MOCK_GITHUB_TAG_MISSING:-false}" == true ]]; then
            respond 404 '{"message":"Not Found"}'
        else
            # Default: an ANNOTATED tag, which is what finalize-release creates,
            # so the mirror's peel path is the one exercised by default.
            body="{\"object\":{\"type\":\"${MOCK_GITHUB_TAG_TYPE:-tag}\",\"sha\":\"${MOCK_GITHUB_TAG_OBJECT_SHA:-${MOCK_SOURCE_SHA:?}}\"}}"
        fi
    elif [[ "$url" == *"/git/tags/"* ]]; then
        # Peeling an annotated tag to its commit.
        body="{\"object\":{\"type\":\"commit\",\"sha\":\"${MOCK_GITHUB_TAG_COMMIT:-${MOCK_SOURCE_SHA:?}}\"}}"
    elif [[ "$url" == *"/releases/tags/"* && "$method" == GET ]]; then
        if [[ -f "$GH/release_created" ]]; then
            body="{\"id\":${MOCK_GITHUB_RELEASE_ID:-7001},\"tag_name\":\"v${MOCK_RELEASE_VERSION}\",\"assets\":$(gh_assets_json)}"
        else
            respond 404 '{"message":"Not Found"}'
        fi
    elif [[ "$url" == */releases && "$method" == POST ]]; then
        if [[ "${MOCK_GITHUB_FAIL_CREATE:-false}" == true ]]; then
            respond 422 '{"message":"Validation Failed"}'
        else
            printf '1' >"$GH/release_created"
            respond 201 "{\"id\":${MOCK_GITHUB_RELEASE_ID:-7001},\"tag_name\":\"v${MOCK_RELEASE_VERSION}\",\"assets\":[]}"
        fi
    elif [[ "$url" == *"/assets?name="* && "$method" == POST ]]; then
        name="${url##*name=}"
        if [[ "${MOCK_GITHUB_FAIL_UPLOAD:-}" == "$name" ]]; then
            respond 500 '{"message":"upload failed"}'
        else
            cp "$upload_file" "$GH_ASSETS/$name"
            respond 201 "{\"name\":\"${name}\",\"state\":\"uploaded\"}"
        fi
    elif [[ "$url" == *"/releases/download/"* && "$method" == GET ]]; then
        name="${url##*/}"
        if [[ -f "$GH_ASSETS/$name" ]]; then
            cp "$GH_ASSETS/$name" "$output"; emit_body=0; status=200
        else
            respond 404 '{"message":"Not Found"}'
        fi
    else
        respond 404 '{"message":"unhandled GitHub mock URL"}'
    fi
# --- Generic package registry: /packages/generic/<name>/<version>/<file> ---
elif [[ "$url" == *"/packages/generic/"* ]]; then
    gen_path="${url##*/packages/generic/}"   # <name>/<version>/<file>
    gen_name="${gen_path%%/*}"
    file="${url##*/}"
    if [[ "$gen_name" == "${MOCK_GENERIC_FLAT_PACKAGE:-lightning}" ]]; then
        # Historic flat layout for the release package: its files live directly
        # in $REG so the package_files listing below can enumerate them, and so
        # the existing publication tests keep working unchanged.
        dest="$REG/$file"
    else
        # Any other generic package (today: the update manifest) keeps its full
        # <name>/<version>/ path, so the immutable per-release copy and the
        # mutable "latest" slot are genuinely distinct destinations.
        dest="$STATE/registry-$gen_path"
        mkdir -p "$(dirname "$dest")"
    fi
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
