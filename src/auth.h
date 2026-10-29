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
void auth_log_accounts(void);

#endif
