#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "state.h"

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#ifndef MAP_ANONYMOUS
#ifdef MAP_ANON
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

SharedState *state_create(void)
{
    SharedState *st;
    pthread_mutexattr_t attr;

    st = mmap(NULL, sizeof *st, PROT_READ | PROT_WRITE,
              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (st == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }

    /* 先清零再初始化锁。顺序反了会把刚建好的锁抹掉。 */
    memset(st, 0, sizeof *st);

    /* 锁和数据在同一块共享页上，PROCESS_SHARED 让 fork 出来的子进程也能用它。 */
    if (pthread_mutexattr_init(&attr) != 0) {
        munmap(st, sizeof *st);
        return NULL;
    }
    if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0 ||
        pthread_mutex_init(&st->lock, &attr) != 0) {
        fprintf(stderr, "process-shared mutex unavailable\n");
        pthread_mutexattr_destroy(&attr);
        munmap(st, sizeof *st);
        return NULL;
    }
    pthread_mutexattr_destroy(&attr);
    return st;
}

void state_destroy(SharedState *st)
{
    if (!st) {
        return;
    }
    pthread_mutex_destroy(&st->lock);
    munmap(st, sizeof *st);
}

void state_lock(SharedState *st)
{
    pthread_mutex_lock(&st->lock);
}

void state_unlock(SharedState *st)
{
    pthread_mutex_unlock(&st->lock);
}
