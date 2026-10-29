# 带 Token 鉴权的迷你 REST API

用 POSIX socket 实现的一个很小的 HTTP 服务：登录发 token，之后读写留言都要带 `Authorization: Bearer <token>`。不使用现成 HTTP 库。

多客户端沿用「一个连接一个进程」。token 表和留言数组放在共享内存里，否则 `fork` 之后每个子进程各有一份副本，登录和留言会对不上。

## 环境

Linux 或 WSL。需要 `gcc`、`make`、`curl`。Windows 上的 MinGW 没有 `fork`，不能用来编译这个服务。

幻灯片需要 `xelatex` 和 `ctex`。

## 编译和运行

```bash
make
./bin/restapi 8080
```

第二个参数是读超时秒数，默认 8。填 `0` 表示一直等。

另开一个终端：

```bash
bash scripts/demo.sh
```

换端口时：

```bash
./bin/restapi 8081
BASE=http://127.0.0.1:8081 bash scripts/demo.sh
```

`make check` 会在 18080 端口启动服务、跑完演示、再关掉。

## 账号

| 用户名 | 密码 |
| --- | --- |
| alice | alice123 |
| bob | bob123 |
| carol | carol123 |

## 课设要求的三条命令

```bash
curl --http1.1 -sS -X POST http://127.0.0.1:8080/api/login \
  -H 'Content-Type: application/json' \
  -d '{"username":"alice","password":"alice123"}'

curl --http1.1 -sS -X POST http://127.0.0.1:8080/api/messages \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"text":"你好，这是一条留言"}'

curl --http1.1 -sS http://127.0.0.1:8080/api/messages \
  -H "Authorization: Bearer $TOKEN"
```

## 接口

| 方法 | 路径 | 鉴权 | 成功 |
| --- | --- | --- | --- |
| GET | `/health` | 否 | 200 `{"ok":true}` |
| GET | `/api/ping` | 否 | 200 `{"pong":true}` |
| POST | `/api/login` | 否 | 200，body 里有 `token` |
| GET | `/api/messages` | 是 | 200，留言列表 |
| POST | `/api/messages` | 是 | 201，写入一条留言 |

登录 body：`{"username":"...","password":"..."}`。留言 body：`{"text":"..."}`。作者以 token 为准，body 里的用户名不会被采纳。

没带 token、token 不对、密码不对，都是 401。请求没在空闲时限内读完，返回 408 并关闭连接。只支持带 `Content-Length` 的一个请求，不支持分块传输，也不保持长连接。

读超时演示（服务端超时要小于 20 秒）：

```bash
./bin/restapi 8080 3
PORT=8080 bash scripts/timeout_demo.sh
```

## 设计要点

- 父进程 `socket / bind / listen / accept`，然后 `fork`。子进程关掉监听套接字，父进程关掉连接套接字。
- `SIGCHLD` 里 `waitpid(WNOHANG)` 回收子进程。父进程经常阻塞在 `accept`，不能只在主循环里回收。
- TCP 没有消息边界。先 `read` 到 `\r\n\r\n`，再按 `Content-Length` 读满 body，然后才处理。
- 重复的 `Content-Length` 直接 400。
- 登录成功用 `sprintf` 生成 `tk_` 加 16 位十六进制。子进程会继承 `rand` 的状态，所以按 pid 重新播种。
- 共享内存在 `mmap(MAP_SHARED)` 上，在 `fork` 之前建好。读写都用一把 `PTHREAD_PROCESS_SHARED` 的互斥锁保护。
- 同一账号再次登录会换一枚新 token，旧的随之作废，所以 token 表最多只有三条记录。
- 留言是长度 64 的循环数组，满了覆盖最旧的一条。重启后 token 和留言都消失。

## 幻灯片

```bash
make slides
```

生成 `presentation/slides.pdf`。全屏播放即可。需要讲稿时，打开 `presentation/slides.tex`，取消 `\setbeameroption{show notes on second screen=right}` 那一行的注释再编译。

## 目录

```text
src/server.c    接入、fork、路由
src/http.c      读满一条请求，按模板写响应
src/json.c      取一层 JSON 字符串
src/auth.c      账号和 token 表
src/store.c     留言循环数组
src/state.c     共享内存和进程间锁
scripts/demo.sh 三条 curl，外加鉴权和并发检查
presentation/slides.tex
```
