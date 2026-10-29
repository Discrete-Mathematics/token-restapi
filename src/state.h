#ifndef STATE_H
#define STATE_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <pthread.h>

#define MAX_TOKENS 32
#define MAX_MESSAGES 64
#define TOKEN_LEN 40
#define USER_LEN 32
#define PASS_LEN 32
#define TEXT_LEN 256

typedef struct {
    int used;
    char token[TOKEN_LEN];
    char username[USER_LEN];
} TokenSlot;

typedef struct {
    char username[USER_LEN];
    char text[TEXT_LEN];
    long long timestamp;
} Message;

/*
 * 这块结构放在 mmap(MAP_SHARED) 里，fork 之后父子进程看到的是同一份。
 * 如果放在普通全局变量里，每个子进程只会改自己的副本。
 */
typedef struct {
    pthread_mutex_t lock;
    int use_mutex;     /* 1：进程间互斥锁；0：下面这把自旋锁 */
    volatile int spin; /* 共享页上的自旋锁，给不支持进程间 mutex 的平台 */
    TokenSlot tokens[MAX_TOKENS];
    Message messages[MAX_MESSAGES];
    int msg_head;  /* 下一条留言写入的位置 */
    int msg_count; /* 当前保留的条数，不超过 MAX_MESSAGES */
} SharedState;

SharedState *state_create(void);
void state_destroy(SharedState *st);
void state_lock(SharedState *st);
void state_unlock(SharedState *st);

#endif
