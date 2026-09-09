#!/bin/bash


pass=0
fail=0

check() {
    local url="$1"
    local expected="$2"
    local label="$3"
    local code

    code=$(curl -s -o /dev/null -w "%{http_code}" --path-as-is "$url")

    if [ "$code" = "$expected" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL: $label - expected $expected, got $code"
    fi
} 

check_ctype() {
    local url="$1"
    local expected="$2"
    local label="$3"
    local ctype

    ctype=$(curl -s -o /dev/null -D - --path-as-is "$url" \
        | grep -i '^content-type:' \
        | tr -d '\r' \
        | cut -d' ' -f2)

    if [ "$ctype" = "$expected" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL: $label — expected content-type $expected, got '$ctype'"
    fi
}

check "localhost:8080/"            200 "GET /"
check_ctype "localhost:8080/"          text/html      "content-type /"
check_ctype "localhost:8080/style.css" text/css       "content-type /style.css"
check_ctype "localhost:8080/logo.svg"  image/svg+xml  "content-type /logo.svg"
check "localhost:8080/style.css"   200 "GET /style.css"
check "localhost:8080/nope"        404 "GET /nope (missing)"
check "localhost:8080/../Makefile" 403 "traversal /../Makefile"

resp=$(printf 'GARBAGE\r\n\r\n' | nc -q1 localhost 8080)
code=$(printf '%s' "$resp" | head -1 | cut -d' ' -f2)
if [ "$code" = "400" ]; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
    echo "FAIL: garbage request - expected 400, got $code"
fi

echo "---"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]