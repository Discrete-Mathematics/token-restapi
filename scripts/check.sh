#!/usr/bin/env bash
# 启动服务、跑 demo.sh、再停掉。给 make check 用。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
# 服务器按相对路径找 web/index.html，所以从仓库根目录启动。
cd "$ROOT"
PORT=${PORT:-18080}
BIN="$ROOT/bin/restapi"
LOG=$(mktemp)

cleanup() {
  if [[ -n "${PID:-}" ]]; then
    kill "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
  fi
  rm -f "$LOG"
}
trap cleanup EXIT

if [[ ! -x "$BIN" ]]; then
  printf '请先 make，找不到 %s\n' "$BIN" >&2
  exit 1
fi

"$BIN" "$PORT" >"$LOG" 2>&1 &
PID=$!

ok=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  if curl -sf --http1.1 --max-time 1 "http://127.0.0.1:${PORT}/health" >/dev/null; then
    ok=1
    break
  fi
  if ! kill -0 "$PID" 2>/dev/null; then
    printf '服务器启动失败\n' >&2
    cat "$LOG" >&2
    exit 1
  fi
  sleep 0.1
done
if [[ "$ok" != 1 ]]; then
  printf '健康检查超时\n' >&2
  cat "$LOG" >&2
  exit 1
fi

if ! BASE="http://127.0.0.1:${PORT}" bash "$ROOT/scripts/demo.sh"; then
  printf '\n---- server log ----\n' >&2
  cat "$LOG" >&2
  exit 1
fi

# 同一账号再次登录会换发 token，旧的必须失效。
old_body=$(curl -sS --http1.1 --max-time 5 \
  -X POST "http://127.0.0.1:${PORT}/api/login" \
  -d '{"username":"bob","password":"bob123"}')
old_token=$(printf '%s\n' "$old_body" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
new_body=$(curl -sS --http1.1 --max-time 5 \
  -X POST "http://127.0.0.1:${PORT}/api/login" \
  -d '{"username":"bob","password":"bob123"}')
new_token=$(printf '%s\n' "$new_body" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
if [[ -z "$old_token" || -z "$new_token" || "$old_token" == "$new_token" ]]; then
  printf '重新登录没有换发 token\n' >&2
  printf 'old=%s\nnew=%s\n' "$old_body" "$new_body" >&2
  cat "$LOG" >&2
  exit 1
fi
old_code=$(curl -s --http1.1 --max-time 5 -o /dev/null -w '%{http_code}' \
  -H "Authorization: Bearer ${old_token}" \
  "http://127.0.0.1:${PORT}/api/messages")
new_code=$(curl -s --http1.1 --max-time 5 -o /dev/null -w '%{http_code}' \
  -H "Authorization: Bearer ${new_token}" \
  "http://127.0.0.1:${PORT}/api/messages")
if [[ "$old_code" != 401 || "$new_code" != 200 ]]; then
  printf '旧 token HTTP %s（期望 401），新 token HTTP %s（期望 200）\n' "$old_code" "$new_code" >&2
  cat "$LOG" >&2
  exit 1
fi

# token 表只有 32 个位置。反复登录不能把它占满。
for i in $(seq 1 40); do
  code=$(curl -s --http1.1 --max-time 5 -o /dev/null -w '%{http_code}' \
    -X POST "http://127.0.0.1:${PORT}/api/login" \
    -d '{"username":"alice","password":"alice123"}')
  if [[ "$code" != 200 ]]; then
    printf '第 %d 次登录得到 HTTP %s，期望 200\n' "$i" "$code" >&2
    cat "$LOG" >&2
    exit 1
  fi
done

# 网页界面：GET / 要返回 HTML。
page_type=$(curl -s --http1.1 --max-time 5 -o /dev/null -w '%{http_code} %{content_type}' \
  "http://127.0.0.1:${PORT}/")
if [[ "$page_type" != "200 text/html"* ]]; then
  printf 'GET / 得到 %s，期望 200 text/html\n' "$page_type" >&2
  cat "$LOG" >&2
  exit 1
fi

# 登出：登出后 token 失效，再登出一次也是 401。
body=$(curl -sS --http1.1 --max-time 5 -X POST "http://127.0.0.1:${PORT}/api/login" \
  -d '{"username":"carol","password":"carol123"}')
token=$(printf '%s\n' "$body" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
out_code=$(curl -s --http1.1 --max-time 5 -o /dev/null -w '%{http_code}' \
  -X POST -H "Authorization: Bearer ${token}" "http://127.0.0.1:${PORT}/api/logout")
after_code=$(curl -s --http1.1 --max-time 5 -o /dev/null -w '%{http_code}' \
  -H "Authorization: Bearer ${token}" "http://127.0.0.1:${PORT}/api/messages")
again_code=$(curl -s --http1.1 --max-time 5 -o /dev/null -w '%{http_code}' \
  -X POST -H "Authorization: Bearer ${token}" "http://127.0.0.1:${PORT}/api/logout")
if [[ -z "$token" || "$out_code" != 200 || "$after_code" != 401 || "$again_code" != 401 ]]; then
  printf '登出 HTTP %s（期望 200），之后读留言 %s（期望 401），再登出 %s（期望 401）\n' \
    "$out_code" "$after_code" "$again_code" >&2
  cat "$LOG" >&2
  exit 1
fi

# 并发压测，顺带检查有没有僵尸进程。
if ! SERVER_PID="$PID" BASE="http://127.0.0.1:${PORT}" bash "$ROOT/scripts/stress.sh"; then
  cat "$LOG" >&2
  exit 1
fi

if ! grep -q "connection -> child" "$LOG"; then
  printf '日志里没有 fork 记录\n' >&2
  cat "$LOG" >&2
  exit 1
fi

printf 'check ok\n'
printf '\n---- server log ----\n'
cat "$LOG"
