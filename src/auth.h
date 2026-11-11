#ifndef AUTH_H
#define AUTH_H

#include "state.h"

#include <stddef.h>

enum {
    AUTH_OK = 0,
    AUTH_BAD_CREDENTIALS = 1,
    AUTH_BAD_TOKEN = 2,
    AUTH_FULL = 3
};

int auth_login(SharedState *st, const char *username, const char *password,
               char *token_out, size_t token_cap);
int auth_lookup(SharedState *st, const char *token, char *user_out, size_t user_cap);
/* 把这枚 token 从表里删掉。找到返回 AUTH_OK，否则 AUTH_BAD_TOKEN。 */
int auth_logout(SharedState *st, const char *token);
void auth_log_accounts(void);

#endif
