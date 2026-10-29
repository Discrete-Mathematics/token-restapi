#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const struct {
    const char *user;
    const char *pass;
} accounts[] = {
    {"alice", "alice123"},
    {"bob", "bob123"},
    {"carol", "carol123"},
};

void auth_log_accounts(void)
{
    size_t i;

    fprintf(stderr, "accounts:");
    for (i = 0; i < sizeof accounts / sizeof accounts[0]; i++) {
        fprintf(stderr, " %s/%s", accounts[i].user, accounts[i].pass);
    }
    fprintf(stderr, "\n");
}

static int valid_account(const char *user, const char *pass)
{
    size_t i;

    for (i = 0; i < sizeof accounts / sizeof accounts[0]; i++) {
        if (strcmp(user, accounts[i].user) == 0 &&
            strcmp(pass, accounts[i].pass) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * fork 会把 rand 的内部状态一起复制。
 * 每个子进程用 pid、时间和 salt 重新播种，再 sprintf 成 token。
 */
static void make_token(char out[TOKEN_LEN], unsigned salt)
{
    unsigned seed = (unsigned)time(NULL) ^ (unsigned)getpid()
                    ^ (unsigned)clock() ^ salt;
    srand(seed);
    sprintf(out, "tk_%08x%08x",
            (unsigned)rand() ^ (unsigned)getpid(),
            (unsigned)rand() ^ salt);
}

static int token_exists(SharedState *st, const char *token)
{
    int i;

    for (i = 0; i < MAX_TOKENS; i++) {
        if (st->tokens[i].used && strcmp(st->tokens[i].token, token) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * 同一个用户重新登录时，复用他原来的位置，旧 token 随之作废。
 * 否则表只增不减，登录满 MAX_TOKENS 次之后谁都登不上。
 */
static int find_slot(SharedState *st, const char *username)
{
    int i;
    int free_slot = -1;

    for (i = 0; i < MAX_TOKENS; i++) {
        if (st->tokens[i].used && strcmp(st->tokens[i].username, username) == 0) {
            return i;
        }
        if (!st->tokens[i].used && free_slot < 0) {
            free_slot = i;
        }
    }
    return free_slot;
}

int auth_login(SharedState *st, const char *username, const char *password,
               char *token_out, size_t token_cap)
{
    char token[TOKEN_LEN];
    int slot;
    int ok = 0;
    unsigned salt;

    if (!st || !username || !password || !token_out) {
        return AUTH_BAD_CREDENTIALS;
    }
    if (username[0] == '\0' || password[0] == '\0') {
        return AUTH_BAD_CREDENTIALS;
    }
    if (strlen(username) >= USER_LEN || strlen(password) >= PASS_LEN) {
        return AUTH_BAD_CREDENTIALS;
    }
    if (token_cap < TOKEN_LEN) {
        return AUTH_FULL;
    }
    if (!valid_account(username, password)) {
        return AUTH_BAD_CREDENTIALS;
    }

    state_lock(st);
    slot = find_slot(st, username);
    if (slot < 0) {
        state_unlock(st);
        return AUTH_FULL;
    }
    for (salt = 1; salt <= 8; salt++) {
        make_token(token, salt);
        if (!token_exists(st, token)) {
            ok = 1;
            break;
        }
    }
    if (!ok) {
        state_unlock(st);
        return AUTH_FULL;
    }
    snprintf(st->tokens[slot].token, sizeof st->tokens[slot].token, "%s", token);
    snprintf(st->tokens[slot].username, sizeof st->tokens[slot].username, "%s", username);
    st->tokens[slot].used = 1;
    state_unlock(st);

    snprintf(token_out, token_cap, "%s", token);
    return AUTH_OK;
}

int auth_lookup(SharedState *st, const char *token, char *user_out, size_t user_cap)
{
    int i;

    if (!st || !token || token[0] == '\0' || !user_out || user_cap < USER_LEN) {
        return AUTH_BAD_TOKEN;
    }

    state_lock(st);
    for (i = 0; i < MAX_TOKENS; i++) {
        if (st->tokens[i].used && strcmp(st->tokens[i].token, token) == 0) {
            memcpy(user_out, st->tokens[i].username, USER_LEN);
            state_unlock(st);
            return AUTH_OK;
        }
    }
    state_unlock(st);
    return AUTH_BAD_TOKEN;
}
