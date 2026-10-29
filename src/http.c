#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "http.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

int http_send_json(int fd, int status, const char *reason, const char *json)
{
    char header[512];
    size_t body_len;
    int n;

    if (!reason || !json) {
        return -1;
    }
    body_len = strlen(json);
    /*
     * 课设要求的响应模板是状态行、Content-Type、Content-Length、空行、body。
     * 多加 Connection: close，因为一个连接只处理一个请求。
     * 不写这一行的话，HTTP/1.1 默认 keep-alive，curl 会一直等下一个响应。
     */
    n = snprintf(header, sizeof header,
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 status, reason, body_len);
    if (n < 0 || (size_t)n >= sizeof header) {
        return -1;
    }
    if (write_all(fd, header, (size_t)n) != 0) {
        return -1;
    }
    return write_all(fd, json, body_len);
}

static int next_line(const char *base, size_t end, size_t *cursor,
                     const char **line, size_t *llen)
{
    size_t i = *cursor;
    size_t start = i;

    if (i >= end) {
        return 0;
    }
    while (i + 1 < end) {
        if (base[i] == '\r' && base[i + 1] == '\n') {
            *line = base + start;
            *llen = i - start;
            *cursor = i + 2;
            return 1;
        }
        i++;
    }
    *line = base + start;
    *llen = end - start;
    *cursor = end;
    return 1;
}

static int name_eq(const char *s, size_t n, const char *lower)
{
    size_t i;
    size_t m = strlen(lower);

    if (n != m) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != lower[i]) {
            return 0;
        }
    }
    return 1;
}

static int parse_content_length(const char *s, size_t n, int *out)
{
    char tmp[32];
    char *end = NULL;
    long v;
    size_t i = 0;
    size_t j;

    while (i < n && (s[i] == ' ' || s[i] == '\t')) {
        i++;
    }
    j = n;
    while (j > i && (s[j - 1] == ' ' || s[j - 1] == '\t')) {
        j--;
    }
    if (j == i || j - i >= sizeof tmp) {
        return -1;
    }
    memcpy(tmp, s + i, j - i);
    tmp[j - i] = '\0';
    errno = 0;
    v = strtol(tmp, &end, 10);
    if (errno != 0 || end == tmp || *end != '\0' || v < 0) {
        return -1;
    }
    if (v > HTTP_MAX_BODY) {
        return -2;
    }
    *out = (int)v;
    return 0;
}

static int parse_request_line(const char *s, size_t n, HttpRequest *req)
{
    size_t i = 0;
    size_t start;
    size_t mlen;
    size_t plen;
    size_t vlen;
    char *cut;

    if (n == 0) {
        return -1;
    }
    while (i < n && s[i] != ' ') {
        i++;
    }
    mlen = i;
    if (mlen == 0 || mlen >= sizeof req->method || i >= n || s[i] != ' ') {
        return -1;
    }
    memcpy(req->method, s, mlen);
    req->method[mlen] = '\0';

    i++;
    start = i;
    while (i < n && s[i] != ' ') {
        i++;
    }
    plen = i - start;
    if (plen == 0 || plen >= sizeof req->path || i >= n || s[i] != ' ') {
        return -1;
    }
    memcpy(req->path, s + start, plen);
    req->path[plen] = '\0';
    cut = strpbrk(req->path, "?#");
    if (cut) {
        *cut = '\0';
    }
    if (req->path[0] != '/') {
        return -1;
    }

    i++;
    vlen = n - i;
    if (!((vlen == 8 && memcmp(s + i, "HTTP/1.0", 8) == 0) ||
          (vlen == 8 && memcmp(s + i, "HTTP/1.1", 8) == 0))) {
        return -1;
    }
    return 0;
}

/*
 * header_end 指向 \r\n\r\n 的第一个字节。
 * 头部文本是 [0, header_end)，最后一行可能没有自己的 CRLF，
 * 因为它的 CRLF 正好是空行的开头。
 */
static int parse_headers(const char *buf, size_t header_end, HttpRequest *req,
                         int *content_length)
{
    size_t cursor = 0;
    int first = 1;
    int saw_length = 0;

    *content_length = 0;
    while (cursor < header_end) {
        const char *line = NULL;
        size_t llen = 0;
        const char *colon;
        const char *val;
        size_t nlen;
        size_t vlen;
        int pr;
        int cl = 0;

        if (!next_line(buf, header_end, &cursor, &line, &llen)) {
            break;
        }
        if (first) {
            first = 0;
            if (parse_request_line(line, llen, req) != 0) {
                return HTTP_READ_BAD;
            }
            continue;
        }
        if (llen == 0) {
            return HTTP_READ_BAD;
        }
        colon = memchr(line, ':', llen);
        if (!colon) {
            return HTTP_READ_BAD;
        }
        nlen = (size_t)(colon - line);
        while (nlen > 0 && (line[nlen - 1] == ' ' || line[nlen - 1] == '\t')) {
            nlen--;
        }
        val = colon + 1;
        vlen = (size_t)(line + llen - val);
        while (vlen > 0 && (*val == ' ' || *val == '\t')) {
            val++;
            vlen--;
        }
        while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t')) {
            vlen--;
        }

        if (name_eq(line, nlen, "content-length")) {
            if (saw_length) {
                /* 两份长度是请求走私的经典手法，直接拒绝。 */
                return HTTP_READ_BAD;
            }
            saw_length = 1;
            pr = parse_content_length(val, vlen, &cl);
            if (pr == -2) {
                return HTTP_READ_TOO_LARGE;
            }
            if (pr != 0) {
                return HTTP_READ_BAD;
            }
            *content_length = cl;
            continue;
        }
        if (name_eq(line, nlen, "authorization")) {
            if (req->has_authorization) {
                continue;
            }
            if (vlen >= sizeof req->authorization) {
                return HTTP_READ_BAD;
            }
            memcpy(req->authorization, val, vlen);
            req->authorization[vlen] = '\0';
            req->has_authorization = 1;
        }
    }
    if (first || req->method[0] == '\0') {
        return HTTP_READ_BAD;
    }
    return HTTP_READ_OK;
}

int http_read_request(int fd, HttpRequest *req)
{
    char buf[HTTP_MAX_HEADER + HTTP_MAX_BODY + 1];
    size_t len = 0;
    size_t header_end = (size_t)-1;
    int content_length = 0;
    int headers_done = 0;

    memset(req, 0, sizeof *req);

    for (;;) {
        char *sep;
        int rc;

        buf[len] = '\0';
        if (len > 0 && memchr(buf, '\0', len) != NULL) {
            return HTTP_READ_BAD;
        }

        if (!headers_done) {
            sep = strstr(buf, "\r\n\r\n");
            if (sep) {
                header_end = (size_t)(sep - buf);
                if (header_end + 4 > HTTP_MAX_HEADER) {
                    return HTTP_READ_TOO_LARGE;
                }
                rc = parse_headers(buf, header_end, req, &content_length);
                if (rc != HTTP_READ_OK) {
                    return rc;
                }
                headers_done = 1;
            } else if (len >= HTTP_MAX_HEADER) {
                return HTTP_READ_TOO_LARGE;
            }
        }

        if (headers_done) {
            size_t need = header_end + 4 + (size_t)content_length;
            if (len >= need) {
                break;
            }
        }
        if (len >= sizeof buf - 1) {
            return HTTP_READ_TOO_LARGE;
        }

        {
            ssize_t n = read(fd, buf + len, sizeof buf - 1 - len);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                    return HTTP_READ_TIMEOUT;
                }
                return HTTP_READ_CLOSED;
            }
            if (n == 0) {
                return len == 0 ? HTTP_READ_CLOSED : HTTP_READ_BAD;
            }
            len += (size_t)n;
        }
    }

    if (content_length > 0) {
        if (memchr(buf + header_end + 4, '\0', (size_t)content_length) != NULL) {
            return HTTP_READ_BAD;
        }
        memcpy(req->body, buf + header_end + 4, (size_t)content_length);
    }
    req->body[content_length] = '\0';
    req->body_len = content_length;
    req->content_length = content_length;
    return HTTP_READ_OK;
}
