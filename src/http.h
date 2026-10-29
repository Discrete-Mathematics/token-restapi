#ifndef HTTP_H
#define HTTP_H

#define HTTP_MAX_METHOD 16
#define HTTP_MAX_PATH 256
#define HTTP_MAX_AUTH 128
#define HTTP_MAX_HEADER 8192
#define HTTP_MAX_BODY 4096

enum {
    HTTP_READ_OK = 0,
    HTTP_READ_CLOSED = 1,
    HTTP_READ_TIMEOUT = 2,
    HTTP_READ_TOO_LARGE = 3,
    HTTP_READ_BAD = 4
};

typedef struct {
    char method[HTTP_MAX_METHOD];
    char path[HTTP_MAX_PATH];
    char authorization[HTTP_MAX_AUTH];
    int has_authorization;
    int content_length;
    int body_len;
    char body[HTTP_MAX_BODY + 1];
} HttpRequest;

/* 把一条 HTTP/1.x 请求从字节流里切出来。调用前可先设置读超时。 */
int http_read_request(int fd, HttpRequest *req);

/* 按固定模板写 JSON 响应。成功返回 0。 */
int http_send_json(int fd, int status, const char *reason, const char *json);

#endif
