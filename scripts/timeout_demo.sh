#!/usr/bin/env bash
# 只发半截请求头，不发空行。服务器空闲超过读超时后应返回 408。
# 先启动: ./bin/restapi 8080 3
# 再运行: PORT=8080 bash scripts/timeout_demo.sh
set -euo pipefail

PORT=${PORT:-8080}

exec 3<>"/dev/tcp/127.0.0.1/${PORT}"
printf 'GET /health HTTP/1.1\r\nHost: localhost\r\n' >&3

line=""
if ! IFS= read -r -t 20 line <&3; then
  printf '20 秒内没有收到响应。确认服务已启动，且读超时小于 20 秒。\n' >&2
  exit 1
fi
printf '%s\n' "$line"
case "$line" in
  *408*) printf 'timeout demo ok\n' ;;
  *) printf '期望 408，实际是上面这一行\n' >&2; exit 1 ;;
esac
