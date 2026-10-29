#!/usr/bin/env bash
# 启动服务、跑 demo.sh、再停掉。给 make check 用。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
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

if ! grep -q "connection -> child" "$LOG"; then
  printf '日志里没有 fork 记录\n' >&2
  cat "$LOG" >&2
  exit 1
fi

printf 'check ok\n'
printf '\n---- server log ----\n'
cat "$LOG"
