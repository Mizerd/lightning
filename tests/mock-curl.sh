#!/usr/bin/env bash
set -Eeuo pipefail

output=/dev/null
method=GET
url=
while (($#)); do
    case "$1" in
        --output) output="$2"; shift 2 ;;
        --write-out) shift 2 ;;
        --request) method="$2"; shift 2 ;;
        --upload-file) shift 2 ;;
        --header|--data-urlencode) shift 2 ;;
        --silent|--show-error|--location) shift ;;
        http*) url="$1"; shift ;;
        *) shift ;;
    esac
done

printf '%s %s\n' "$method" "$url" >>"${MOCK_CURL_LOG:?}"
scenario="${MOCK_SCENARIO:-success}"
status=200
body='{}'

if [[ "$scenario" == network ]]; then
    exit 7
elif [[ "$url" == */repository/tags/* ]]; then
    if [[ "$scenario" == authentication ]]; then
        status=401
        body='{"message":"401 Unauthorized"}'
    else
        body="{\"commit\":{\"id\":\"${MOCK_SOURCE_SHA:?}\"}}"
    fi
elif [[ "$url" == */releases/*/assets/links* && "$method" == GET ]]; then
    case "$scenario" in
        duplicate-link) body="${MOCK_EXISTING_LINKS:?}" ;;
        conflicting-link) body="${MOCK_CONFLICTING_LINKS:?}" ;;
        *) body='[]' ;;
    esac
elif [[ "$url" == */releases/*/assets/links && "$method" == POST ]]; then
    status=201
    body='{"id":1}'
elif [[ "$url" == */releases/* ]]; then
    if [[ "$scenario" == missing-release ]]; then
        status=404
        body='{"message":"404 Release Not Found"}'
    else
        body='{"tag_name":"v1.2.3"}'
    fi
elif [[ "$url" == *'/packages?package_name='* ]]; then
    body='[{"id":42,"name":"lightning","package_type":"generic"}]'
elif [[ "$url" == */packages/42/package_files && "$method" == GET ]]; then
    body='[{"id":51,"file_name":"lightning_1.2.3_amd64.deb"},{"id":52,"file_name":"lightning-1.2.3-1.x86_64.rpm"}]'
elif [[ "$url" == */package_files/* && "$method" == DELETE ]]; then
    status=204
    body=''
elif [[ "$url" == */packages/generic/lightning/* && "$method" == GET ]]; then
    case "$scenario" in
        identical)
            if [[ "$url" == *.deb ]]; then cp "${MOCK_DEB:?}" "$output"; else cp "${MOCK_RPM:?}" "$output"; fi
            output=
            ;;
        conflict) body='different package bytes' ;;
        insufficient) status=403; body='{"message":"403 Forbidden"}' ;;
        *) status=404; body='{"message":"404 Not Found"}' ;;
    esac
elif [[ "$url" == */packages/generic/lightning/* && "$method" == PUT ]]; then
    if [[ "$scenario" == partial-upload && "$url" == *.rpm ]]; then
        status=500
        body='{"message":"upload failed"}'
    else
        status=201
        body='{"message":"201 Created"}'
    fi
else
    status=404
    body='{"message":"unhandled mock URL"}'
fi

if [[ -n "$output" && "$output" != /dev/null ]]; then
    printf '%s' "$body" >"$output"
fi
printf '%s' "$status"
