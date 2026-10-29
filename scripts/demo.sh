#!/usr/bin/env bash
# 课设演示。先在另一个终端启动: ./bin/restapi 8080
# 幻灯片上的三条命令是：登录、发表留言、读取留言。
# 这里多加了 --max-time，避免服务器卡死时演示脚本一直等。
set -euo pipefail

BASE=${BASE:-http://127.0.0.1:8080}
CURL=(curl --http1.1 -sS --max-time 5)
CODE=""
RESP=""
TMPS=()

fail() {
  printf 'FAIL: %s\n' "$*" >&2
  if [[ -n "${RESP:-}" ]]; then
    printf 'body: %s\n' "$RESP" >&2
  fi
  if [[ -n "${CODE:-}" ]]; then
    printf 'HTTP %s\n' "$CODE" >&2
  fi
  exit 1
}

cleanup() {
  if [[ ${#TMPS[@]} -gt 0 ]]; then
    rm -f "${TMPS[@]}"
  fi
}
trap cleanup EXIT

banner() {
  printf '\n========== %s ==========\n' "$1"
}

quiet() {
  local tmp
  tmp=$(mktemp)
  TMPS+=("$tmp")
  CODE=$("${CURL[@]}" -o "$tmp" -w '%{http_code}' "$@") || fail "curl 失败"
  RESP=$(cat "$tmp")
}

expect() {
  local want=$1
  local title=$2
  if [[ "$CODE" != "$want" ]]; then
    fail "$title：期望 HTTP $want，得到 $CODE"
  fi
  printf '[pass] %s -> %s\n' "$title" "$CODE"
}

if ! code=$("${CURL[@]}" -o /dev/null -w '%{http_code}' "$BASE/health" || true); then
  code=""
fi
if [[ "$code" != "200" ]]; then
  fail "连不上 $BASE 。请先在另一个终端运行 ./bin/restapi 8080"
fi

banner "预热  GET /health"
cat <<EOF
\$ curl --http1.1 -sS ${BASE}/health
EOF
quiet "$BASE/health"
printf '%s\nHTTP %s\n' "$RESP" "$CODE"
[[ "$CODE" == "200" && "$RESP" == '{"ok":true}' ]] || fail "health 响应不对"

banner "1/3  登录  POST /api/login"
cat <<EOF
\$ curl --http1.1 -sS -X POST ${BASE}/api/login \\
    -H 'Content-Type: application/json' \\
    -d '{"username":"alice","password":"alice123"}'
EOF
quiet -X POST "$BASE/api/login" \
  -H 'Content-Type: application/json' \
  -d '{"username":"alice","password":"alice123"}'
printf '%s\nHTTP %s\n' "$RESP" "$CODE"
[[ "$CODE" == "200" ]] || fail "登录应该返回 200"
TOKEN=$(printf '%s\n' "$RESP" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
[[ -n "$TOKEN" && "$TOKEN" == tk_* ]] || fail "登录响应里没有 token"
printf 'TOKEN=%s\n' "$TOKEN"

banner "2/3  发表留言  POST /api/messages"
cat <<EOF
\$ curl --http1.1 -sS -X POST ${BASE}/api/messages \\
    -H 'Authorization: Bearer \$TOKEN' \\
    -H 'Content-Type: application/json' \\
    -d '{"text":"你好，这是一条留言"}'
EOF
quiet -X POST "$BASE/api/messages" \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"text":"你好，这是一条留言"}'
printf '%s\nHTTP %s\n' "$RESP" "$CODE"
[[ "$CODE" == "201" ]] || fail "发表留言应该返回 201"
printf '%s\n' "$RESP" | grep -F '"username":"alice"' >/dev/null || fail "作者应该是 alice"
printf '%s\n' "$RESP" | grep -F '"text":"你好，这是一条留言"' >/dev/null || fail "留言正文丢了"

banner "3/3  读取留言  GET /api/messages"
cat <<EOF
\$ curl --http1.1 -sS ${BASE}/api/messages \\
    -H 'Authorization: Bearer \$TOKEN'
EOF
quiet "$BASE/api/messages" -H "Authorization: Bearer $TOKEN"
printf '%s\nHTTP %s\n' "$RESP" "$CODE"
[[ "$CODE" == "200" ]] || fail "读取留言应该返回 200"
printf '%s\n' "$RESP" | grep -F '你好，这是一条留言' >/dev/null || fail "列表里没有刚发的留言"

banner "额外检查"
quiet "$BASE/api/messages"
expect 401 "不带 token 读留言"
printf '%s\n' "$RESP" | grep -F '你好，这是一条留言' >/dev/null && fail "401 响应里不该带留言" || true

quiet "$BASE/api/messages" -H 'Authorization: Bearer tk_deadbeefdeadbeef'
expect 401 "错误 token"

quiet -X POST "$BASE/api/login" \
  -H 'Content-Type: application/json' \
  -d '{"username":"carol","password":"wrong"}'
expect 401 "错误密码"
printf '%s\n' "$RESP" | grep -F '"token":' >/dev/null && fail "错误密码不应发 token" || true

quiet -X POST "$BASE/api/login" \
  -H 'Content-Type: application/json' \
  -d '{bad'
expect 400 "坏掉的 JSON"

quiet -X POST "$BASE/health"
expect 405 "对 /health 用 POST"

quiet "$BASE/no-such-path"
expect 404 "未知路径"

quiet "$BASE/api/ping"
expect 200 "心跳 /api/ping"
[[ "$RESP" == '{"pong":true}' ]] || fail "ping 正文不对: $RESP"

quiet -X POST "$BASE/api/messages" \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"username":"bob","text":"spoof-check"}'
expect 201 "带着 alice 的 token 发留言"
printf '%s\n' "$RESP" | grep -F '"username":"alice","text":"spoof-check"' >/dev/null \
  || fail "作者被 body 里的 username 改掉了"

quiet -X POST "$BASE/api/login" \
  -H 'Content-Type: application/json' \
  -d '{"username":"bob","password":"bob123"}'
expect 200 "bob 登录"
TOKEN_B=$(printf '%s\n' "$RESP" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
[[ -n "$TOKEN_B" && "$TOKEN_B" != "$TOKEN" ]] || fail "bob 应该拿到另一枚 token"

quiet -X POST "$BASE/api/login" \
  -H 'Content-Type: application/json' \
  -d '{"username":"carol","password":"carol123"}'
expect 200 "carol 登录"

body_a=$(mktemp)
body_b=$(mktemp)
code_a=$(mktemp)
code_b=$(mktemp)
TMPS+=("$body_a" "$body_b" "$code_a" "$code_b")
"${CURL[@]}" -o "$body_a" -w '%{http_code}' -X POST "$BASE/api/messages" \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"text":"from-alice-parallel"}' >"$code_a" &
pid_a=$!
"${CURL[@]}" -o "$body_b" -w '%{http_code}' -X POST "$BASE/api/messages" \
  -H "Authorization: Bearer $TOKEN_B" \
  -H 'Content-Type: application/json' \
  -d '{"text":"from-bob-parallel"}' >"$code_b" &
pid_b=$!
set +e
wait "$pid_a"
st_a=$?
wait "$pid_b"
st_b=$?
set -e
[[ "$st_a" -eq 0 && "$st_b" -eq 0 ]] || fail "并发 curl 没有正常结束"
[[ "$(cat "$code_a")" == "201" && "$(cat "$code_b")" == "201" ]] || fail "并发留言应该都是 201"

quiet "$BASE/api/messages" -H "Authorization: Bearer $TOKEN"
expect 200 "并发之后再读列表"
printf '%s\n' "$RESP" | grep -F 'from-alice-parallel' >/dev/null || fail "列表里没有 alice 的并发留言"
printf '%s\n' "$RESP" | grep -F 'from-bob-parallel' >/dev/null || fail "列表里没有 bob 的并发留言"
printf '%s\n' "$RESP" | grep -F '"username":"bob","text":"from-bob-parallel"' >/dev/null \
  || fail "bob 的留言作者不对"

printf '\n全部通过。\n'
printf '课设要求的三条命令是：登录、发表留言、读取留言。\n'
