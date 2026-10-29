#ifndef STORE_H
#define STORE_H

#include "state.h"

int store_add(SharedState *st, const char *username, const char *text, Message *saved);

/* 返回 malloc 的 JSON，调用者 free。失败返回 NULL。 */
char *store_list_json(SharedState *st);

#endif
