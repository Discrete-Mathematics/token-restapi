#!/usr/bin/env bash
# 并发压测：N 个客户端同时发留言。
# 检查三件事：全部返回 201；列表里一条不少；跑完没有僵尸子进程。
# 先启动: ./bin/restapi 8080
# 再运行: bash scripts/stress.sh
# 调参数: N=30 BASE=http://127.0.0.1:8081 bash scripts/stress.sh
# 用 carol 登录，免得把演示里 alice / bob 的 token 顶掉。
set -euo pipefail

BASE=${BASE:-http://127.0.0.1:8080}
N=${N:-50}
CURL=(curl --http1.1 -sS --max-time 10)

if (( N < 1 || N > 64 )); then
  printf 'N 要在 1 到 64 之间（留言数组只保留最近 64 条）\n' >&2
  exit 2
fi

now() { perl -MTime::HiRes=time -e 'printf "%.3f", time'; }

body=$("${CURL[@]}" -X POST "$BASE/api/login" \
  -d '{"username":"carol","password":"carol123"}')
TOKEN=$(printf '%s\n' "$body" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
if [[ -z "$TOKEN" ]]; then
  printf '登录失败: %s\n' "$body" >&2
  exit 1
fi

TAG="stress-$$-$(date +%s)"
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

start=$(now)
for i in $(seq 1 "$N"); do
  "${CURL[@]}" -o /dev/null -w '%{http_code}' -X POST "$BASE/api/messages" \
    -H "Authorization: Bearer $TOKEN" \
    -d "{\"text\":\"$TAG-$i\"}" >"$DIR/$i" 2>/dev/null &
done
wait
end=$(now)

# 每个文件里只有一个状态码，没有换行，所以按文件数。
created=$(grep -lx '201' "$DIR"/* | wc -l | tr -d ' ')
list=$("${CURL[@]}" "$BASE/api/messages" -H "Authorization: Bearer $TOKEN")
found=$(printf '%s\n' "$list" | grep -o "\"$TAG-[0-9]*\"" | sort -u | wc -l | tr -d ' ')
elapsed=$(perl -e "printf '%.2f', $end - $start")

# 僵尸进程：父进程是服务器、状态里带 Z 的进程。SERVER_PID 没给就按名字找。
sleep 0.5
pid=${SERVER_PID:-$(pgrep -o -f 'bin/restapi' || true)}
zombies="?"
if [[ -n "$pid" ]]; then
  zombies=$(ps -A -o ppid=,stat= | awk -v p="$pid" '$1 == p && $2 ~ /Z/' | wc -l | tr -d ' ')
fi

printf '\n========== 并发压测 ==========\n'
printf '客户端数        %s\n' "$N"
printf '返回 201        %s\n' "$created"
printf '列表里找到      %s\n' "$found"
printf '总耗时          %s 秒\n' "$elapsed"
printf '僵尸进程        %s\n' "$zombies"

if [[ "$created" != "$N" || "$found" != "$N" || "$zombies" != 0 ]]; then
  printf 'FAIL\n' >&2
  exit 1
fi
printf 'stress ok\n'
