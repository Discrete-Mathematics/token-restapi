#include "store.h"

#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int store_add(SharedState *st, const char *username, const char *text, Message *saved)
{
    Message *slot;
    time_t now;

    if (!st || !username || !text || !saved) {
        return -1;
    }
    if (username[0] == '\0' || text[0] == '\0') {
        return -1;
    }
    if (strlen(username) >= USER_LEN || strlen(text) >= TEXT_LEN) {
        return -1;
    }

    state_lock(st);
    slot = &st->messages[st->msg_head];
    snprintf(slot->username, sizeof slot->username, "%s", username);
    snprintf(slot->text, sizeof slot->text, "%s", text);
    now = time(NULL);
    slot->timestamp = now < 0 ? 0 : (long long)now;
    *saved = *slot;
    st->msg_head++;
    if (st->msg_head >= MAX_MESSAGES) {
        st->msg_head = 0;
    }
    if (st->msg_count < MAX_MESSAGES) {
        st->msg_count++;
    }
    state_unlock(st);
    return 0;
}

static int append_raw(char *dst, size_t cap, size_t *used, const char *s)
{
    size_t n = strlen(s);

    if (*used + n + 1 > cap) {
        return -1;
    }
    memcpy(dst + *used, s, n);
    *used += n;
    dst[*used] = '\0';
    return 0;
}

char *store_list_json(SharedState *st)
{
    size_t cap = (size_t)MAX_MESSAGES * (TEXT_LEN * 2 + USER_LEN * 2 + 80) + 32;
    char *out;
    size_t used = 0;
    int failed = 0;
    int count;
    int start;
    int i;

    if (!st) {
        return NULL;
    }
    out = malloc(cap);
    if (!out) {
        return NULL;
    }
    if (append_raw(out, cap, &used, "{\"messages\":[") != 0) {
        free(out);
        return NULL;
    }

    state_lock(st);
    count = st->msg_count;
    start = st->msg_head - count;
    if (start < 0) {
        start += MAX_MESSAGES;
    }
    for (i = 0; i < count; i++) {
        char obj[1024];
        char esc_user[USER_LEN * 2];
        char esc_text[TEXT_LEN * 2];
        int idx = (start + i) % MAX_MESSAGES;
        int n;
        Message *msg = &st->messages[idx];

        if (i > 0 && append_raw(out, cap, &used, ",") != 0) {
            failed = 1;
            break;
        }
        if (json_escape(msg->username, esc_user, sizeof esc_user) != 0 ||
            json_escape(msg->text, esc_text, sizeof esc_text) != 0) {
            failed = 1;
            break;
        }
        n = snprintf(obj, sizeof obj,
                     "{\"username\":\"%s\",\"text\":\"%s\",\"time\":%lld}",
                     esc_user, esc_text, msg->timestamp);
        if (n < 0 || (size_t)n >= sizeof obj || append_raw(out, cap, &used, obj) != 0) {
            failed = 1;
            break;
        }
    }
    state_unlock(st);

    if (failed || append_raw(out, cap, &used, "]}") != 0) {
        free(out);
        return NULL;
    }
    return out;
}
