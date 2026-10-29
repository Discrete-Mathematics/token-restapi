/*
 * 迷你 REST API。
 *
 * 父进程：socket -> bind -> listen -> accept，每个连接 fork 一次。
 * 子进程：read 到空行，再按 Content-Length 读完 body，处理完就退出。
 * token 表和留言数组放在 mmap 共享内存里。普通全局变量会被 fork 复制，
 * 下一个连接就看不见刚签发的 token。
 *
 * 用法: ./bin/restapi [端口] [读超时秒数]
 * 编译: make
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "auth.h"
#include "http.h"
#include "json.h"
#include "state.h"
#include "store.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t listen_fd = -1;
static volatile sig_atomic_t stop_flag = 0;
static volatile sig_atomic_t is_parent = 1;
static int io_timeout_sec = 8;

static void arm_stop(int sig)
{
    int fd;

    (void)sig;
    if (!is_parent) {
        _exit(0);
    }
    stop_flag = 1;
    fd = (int)listen_fd;
    if (fd >= 0) {
        listen_fd = -1;
        close(fd);
    }
}

static void reap_children(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
}

static void log_status(const HttpRequest *req, int status)
{
    fprintf(stderr, "[pid %d] %s %s -> %d\n",
            (int)getpid(), req->method, req->path, status);
}

static void reply(int fd, const HttpRequest *req, int status,
                  const char *reason, const char *json)
{
    if (http_send_json(fd, status, reason, json) != 0) {
        fprintf(stderr, "[pid %d] send failed\n", (int)getpid());
    }
    log_status(req, status);
}

static void reply_error(int fd, const HttpRequest *req, int status,
                        const char *reason, const char *msg)
{
    char body[192];
    int n;

    n = snprintf(body, sizeof body, "{\"error\":\"%s\"}", msg);
    if (n < 0 || (size_t)n >= sizeof body) {
        reply(fd, req, status, reason, "{\"error\":\"服务器内部错误\"}");
        return;
    }
    reply(fd, req, status, reason, body);
}

static int extract_bearer(const char *value, char *out, size_t cap)
{
    size_t i = 0;

    while (*value == ' ' || *value == '\t') {
        value++;
    }
    if (strncmp(value, "Bearer ", 7) != 0) {
        return -1;
    }
    value += 7;
    while (*value == ' ' || *value == '\t') {
        value++;
    }
    if (*value == '\0') {
        return -1;
    }
    while (*value && *value != ' ' && *value != '\t') {
        if (i + 1 >= cap) {
            return -1;
        }
        out[i++] = *value++;
    }
    out[i] = '\0';
    return 0;
}

static int require_user(int fd, SharedState *st, const HttpRequest *req,
                        char *user, size_t user_cap)
{
    char token[TOKEN_LEN];

    if (!req->has_authorization) {
        reply_error(fd, req, 401, "Unauthorized", "缺少 token");
        return -1;
    }
    if (extract_bearer(req->authorization, token, sizeof token) != 0 ||
        auth_lookup(st, token, user, user_cap) != AUTH_OK) {
        reply_error(fd, req, 401, "Unauthorized", "token 无效");
        return -1;
    }
    return 0;
}

static void handle_login(int fd, SharedState *st, const HttpRequest *req)
{
    char user[USER_LEN];
    char pass[PASS_LEN];
    char token[TOKEN_LEN];
    char body[160];
    int ur;
    int pr;
    int ar;
    int n;

    ur = json_get_string(req->body, "username", user, sizeof user);
    pr = json_get_string(req->body, "password", pass, sizeof pass);
    if (ur == -2 || pr == -2) {
        memset(pass, 0, sizeof pass);
        reply_error(fd, req, 400, "Bad Request", "字段太长");
        return;
    }
    if (ur != 0 || pr != 0) {
        memset(pass, 0, sizeof pass);
        reply_error(fd, req, 400, "Bad Request", "请求格式不对");
        return;
    }

    ar = auth_login(st, user, pass, token, sizeof token);
    memset(pass, 0, sizeof pass);
    if (ar == AUTH_BAD_CREDENTIALS) {
        reply_error(fd, req, 401, "Unauthorized", "用户名或密码错误");
        return;
    }
    if (ar == AUTH_FULL) {
        reply_error(fd, req, 503, "Service Unavailable", "token 表已满");
        return;
    }
    if (ar != AUTH_OK) {
        reply_error(fd, req, 500, "Internal Server Error", "服务器内部错误");
        return;
    }

    n = snprintf(body, sizeof body,
                 "{\"token\":\"%s\",\"username\":\"%s\"}", token, user);
    if (n < 0 || (size_t)n >= sizeof body) {
        reply_error(fd, req, 500, "Internal Server Error", "服务器内部错误");
        return;
    }
    reply(fd, req, 200, "OK", body);
}

static void handle_get_messages(int fd, SharedState *st, const HttpRequest *req)
{
    char user[USER_LEN];
    char *json;

    if (require_user(fd, st, req, user, sizeof user) != 0) {
        return;
    }
    /* 留言板是全局的。token 只证明你是谁，不按人拆成多份列表。 */
    json = store_list_json(st);
    if (!json) {
        reply_error(fd, req, 500, "Internal Server Error", "服务器内部错误");
        return;
    }
    reply(fd, req, 200, "OK", json);
    free(json);
}

static void handle_post_message(int fd, SharedState *st, const HttpRequest *req)
{
    char user[USER_LEN];
    char text[TEXT_LEN];
    char esc_user[USER_LEN * 2];
    char esc_text[TEXT_LEN * 2];
    char body[TEXT_LEN * 2 + 160];
    Message saved;
    int jr;
    int n;

    if (require_user(fd, st, req, user, sizeof user) != 0) {
        return;
    }
    jr = json_get_string(req->body, "text", text, sizeof text);
    if (jr == -2) {
        reply_error(fd, req, 400, "Bad Request", "留言太长");
        return;
    }
    if (jr != 0) {
        reply_error(fd, req, 400, "Bad Request", "请求格式不对");
        return;
    }
    if (text[0] == '\0') {
        reply_error(fd, req, 400, "Bad Request", "需要留言内容");
        return;
    }
    /*
     * 作者以 token 对应的用户名为准。body 里即使写了别人的 username 也忽略，
     * 避免拿着自己的 token 冒充别人留言。
     */
    if (store_add(st, user, text, &saved) != 0) {
        reply_error(fd, req, 500, "Internal Server Error", "服务器内部错误");
        return;
    }
    if (json_escape(saved.username, esc_user, sizeof esc_user) != 0 ||
        json_escape(saved.text, esc_text, sizeof esc_text) != 0) {
        reply_error(fd, req, 500, "Internal Server Error", "服务器内部错误");
        return;
    }
    n = snprintf(body, sizeof body,
                 "{\"ok\":true,\"username\":\"%s\",\"text\":\"%s\",\"time\":%lld}",
                 esc_user, esc_text, saved.timestamp);
    if (n < 0 || (size_t)n >= sizeof body) {
        reply_error(fd, req, 500, "Internal Server Error", "服务器内部错误");
        return;
    }
    reply(fd, req, 201, "Created", body);
}

static void route(int fd, SharedState *st, const HttpRequest *req)
{
    if (strcmp(req->path, "/health") == 0) {
        if (strcmp(req->method, "GET") != 0) {
            reply_error(fd, req, 405, "Method Not Allowed", "方法不对");
            return;
        }
        reply(fd, req, 200, "OK", "{\"ok\":true}");
        return;
    }
    if (strcmp(req->path, "/api/ping") == 0) {
        if (strcmp(req->method, "GET") != 0) {
            reply_error(fd, req, 405, "Method Not Allowed", "方法不对");
            return;
        }
        reply(fd, req, 200, "OK", "{\"pong\":true}");
        return;
    }
    if (strcmp(req->path, "/api/login") == 0) {
        if (strcmp(req->method, "POST") != 0) {
            reply_error(fd, req, 405, "Method Not Allowed", "方法不对");
            return;
        }
        handle_login(fd, st, req);
        return;
    }
    if (strcmp(req->path, "/api/messages") == 0) {
        if (strcmp(req->method, "GET") == 0) {
            handle_get_messages(fd, st, req);
            return;
        }
        if (strcmp(req->method, "POST") == 0) {
            handle_post_message(fd, st, req);
            return;
        }
        reply_error(fd, req, 405, "Method Not Allowed", "方法不对");
        return;
    }
    reply_error(fd, req, 404, "Not Found", "没有这个路径");
}

static void handle_client(int fd, SharedState *st)
{
    HttpRequest req;
    int rc = http_read_request(fd, &req);

    if (rc == HTTP_READ_CLOSED) {
        fprintf(stderr, "[pid %d] client closed\n", (int)getpid());
        return;
    }
    if (rc == HTTP_READ_TIMEOUT) {
        http_send_json(fd, 408, "Request Timeout", "{\"error\":\"读超时\"}");
        fprintf(stderr, "[pid %d] read timeout -> 408\n", (int)getpid());
        return;
    }
    if (rc == HTTP_READ_TOO_LARGE) {
        http_send_json(fd, 413, "Payload Too Large", "{\"error\":\"请求过大\"}");
        fprintf(stderr, "[pid %d] too large -> 413\n", (int)getpid());
        return;
    }
    if (rc != HTTP_READ_OK) {
        http_send_json(fd, 400, "Bad Request", "{\"error\":\"请求格式不对\"}");
        fprintf(stderr, "[pid %d] bad request -> 400\n", (int)getpid());
        return;
    }
    route(fd, st, &req);
}

static void set_timeouts(int fd)
{
    struct timeval tv;

    if (io_timeout_sec <= 0) {
        return;
    }
    memset(&tv, 0, sizeof tv);
    tv.tv_sec = io_timeout_sec;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0) {
        perror("SO_RCVTIMEO");
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) != 0) {
        perror("SO_SNDTIMEO");
    }
}

static int install_signals(void)
{
    struct sigaction sa_chld;
    struct sigaction sa_stop;

    memset(&sa_chld, 0, sizeof sa_chld);
    sa_chld.sa_handler = reap_children;
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa_chld, NULL) != 0) {
        perror("sigaction SIGCHLD");
        return -1;
    }

    memset(&sa_stop, 0, sizeof sa_stop);
    sa_stop.sa_handler = arm_stop;
    if (sigaction(SIGINT, &sa_stop, NULL) != 0 ||
        sigaction(SIGTERM, &sa_stop, NULL) != 0) {
        perror("sigaction stop");
        return -1;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("SIGPIPE");
        return -1;
    }
    return 0;
}

static int listen_on(int port)
{
    int fd;
    int opt = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt) != 0) {
        perror("SO_REUSEADDR");
        close(fd);
        return -1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 16) != 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

static int parse_number(const char *s, long *out)
{
    char *end = NULL;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return -1;
    }
    *out = v;
    return 0;
}

static int parse_args(int argc, char **argv, int *port)
{
    long value;

    if (argc > 3) {
        return -1;
    }
    if (argc >= 2) {
        if (parse_number(argv[1], &value) != 0 || value < 1 || value > 65535) {
            fprintf(stderr, "端口要在 1 到 65535 之间\n");
            return -1;
        }
        *port = (int)value;
    }
    if (argc >= 3) {
        if (parse_number(argv[2], &value) != 0 || value < 0 || value > 120) {
            fprintf(stderr, "读超时要在 0 到 120 秒之间，0 表示一直等\n");
            return -1;
        }
        io_timeout_sec = (int)value;
    }
    return 0;
}

static void fill_blockset(sigset_t *set)
{
    if (sigemptyset(set) != 0 ||
        sigaddset(set, SIGINT) != 0 ||
        sigaddset(set, SIGTERM) != 0 ||
        sigaddset(set, SIGCHLD) != 0) {
        perror("sigset");
        exit(1);
    }
}

static void accept_loop(SharedState *state)
{
    sigset_t blockset;
    sigset_t oldset;

    fill_blockset(&blockset);
    while (!stop_flag) {
        int conn;
        pid_t pid;

        while (waitpid(-1, NULL, WNOHANG) > 0) {
        }
        errno = 0;
        conn = accept((int)listen_fd, NULL, NULL);
        if (conn < 0) {
            if (stop_flag || errno == EBADF) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        if (sigprocmask(SIG_BLOCK, &blockset, &oldset) != 0) {
            perror("sigprocmask");
            close(conn);
            continue;
        }
        pid = fork();
        if (pid < 0) {
            sigprocmask(SIG_SETMASK, &oldset, NULL);
            perror("fork");
            http_send_json(conn, 503, "Service Unavailable",
                           "{\"error\":\"服务器忙\"}");
            close(conn);
            continue;
        }
        if (pid == 0) {
            int lfd = (int)listen_fd;

            is_parent = 0;
            listen_fd = -1;
            if (lfd >= 0) {
                close(lfd);
            }
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            sigprocmask(SIG_SETMASK, &oldset, NULL);
            set_timeouts(conn);
            handle_client(conn, state);
            close(conn);
            fflush(NULL);
            _exit(0);
        }
        close(conn);
        sigprocmask(SIG_SETMASK, &oldset, NULL);
        fprintf(stderr, "[parent %d] connection -> child %d\n",
                (int)getpid(), (int)pid);
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr, "用法: %s [端口] [读超时秒数]\n", argv0);
    fprintf(stderr, "默认端口 8080，默认读超时 8 秒。超时填 0 表示一直等。\n");
}

int main(int argc, char **argv)
{
    int port = 8080;
    int fd;
    SharedState *state;

    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    if (parse_args(argc, argv, &port) != 0) {
        usage(argv[0]);
        return 2;
    }
    state = state_create();
    if (!state) {
        return 1;
    }
    if (install_signals() != 0) {
        state_destroy(state);
        return 1;
    }
    fd = listen_on(port);
    if (fd < 0) {
        state_destroy(state);
        return 1;
    }
    listen_fd = fd;

    fprintf(stderr, "restapi listening on 0.0.0.0:%d (timeout %ds)\n",
            port, io_timeout_sec);
    auth_log_accounts();

    accept_loop(state);

    fprintf(stderr, "shutting down\n");
    while (waitpid(-1, NULL, 0) > 0) {
    }
    if (listen_fd >= 0) {
        close((int)listen_fd);
        listen_fd = -1;
    }
    state_destroy(state);
    return 0;
}
