#ifndef JSON_H
#define JSON_H

#include <stddef.h>

/*
 * 只取一层对象里的字符串字段，够登录和留言用。
 * 返回 0 成功，-1 缺失或格式不对，-2 超过 outsz。
 */
int json_get_string(const char *json, const char *key, char *out, size_t outsz);

/* 写成 JSON 字符串的内容，不含两侧引号。返回 0 成功。 */
int json_escape(const char *src, char *dst, size_t dst_cap);

#endif
