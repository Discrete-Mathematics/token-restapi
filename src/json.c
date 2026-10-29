#include "json.h"

#include <stdio.h>
#include <string.h>

static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

int json_get_string(const char *json, const char *key, char *out, size_t outsz)
{
    char pat[64];
    const char *hit;
    const char *p;
    size_t i;
    int nw;

    if (!json || !key || !out || outsz < 2) {
        return -1;
    }
    out[0] = '\0';
    nw = snprintf(pat, sizeof pat, "\"%s\"", key);
    if (nw < 0 || nw >= (int)sizeof pat) {
        return -1;
    }

    hit = json;
    p = NULL;
    while ((hit = strstr(hit, pat)) != NULL) {
        const char *q = hit + strlen(pat);
        while (is_ws(*q)) {
            q++;
        }
        if (*q == ':') {
            p = q + 1;
            break;
        }
        hit += 1;
    }
    if (!p) {
        return -1;
    }
    while (is_ws(*p)) {
        p++;
    }
    if (*p != '"') {
        return -1;
    }
    p++;

    i = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            if (*p == '\0') {
                return -1;
            }
            c = *p++;
            if (c != '"' && c != '\\' && c != '/') {
                /* 不接受 \n \uXXXX，留言保持单行。 */
                return -1;
            }
        }
        if ((unsigned char)c < 0x20) {
            return -1;
        }
        if (i + 1 >= outsz) {
            return -2;
        }
        out[i++] = c;
    }
    if (*p != '"') {
        return -1;
    }
    out[i] = '\0';
    return 0;
}

int json_escape(const char *src, char *dst, size_t dst_cap)
{
    size_t used = 0;

    if (!src || !dst || dst_cap == 0) {
        return -1;
    }
    while (*src) {
        unsigned char c = (unsigned char)*src++;
        if (c < 0x20) {
            return -1;
        }
        if (c == '"' || c == '\\') {
            if (used + 2 >= dst_cap) {
                return -1;
            }
            dst[used++] = '\\';
            dst[used++] = (char)c;
            continue;
        }
        if (used + 1 >= dst_cap) {
            return -1;
        }
        dst[used++] = (char)c;
    }
    dst[used] = '\0';
    return 0;
}
