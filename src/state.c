#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "state.h"

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#ifndef MAP_ANONYMOUS
#ifdef MAP_ANON
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

static int init_shared_mutex(SharedState *st)
{
    pthread_mutexattr_t attr;

    if (pthread_mutexattr_init(&attr) != 0) {
        return -1;
    }
    if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
        pthread_mutexattr_destroy(&attr);
        return -1;
    }
#ifdef PTHREAD_MUTEX_ROBUST
    /*
     * 子进程如果死的时候还拿着锁，普通互斥锁会永远锁死。
     * robust 锁让下一个加锁的进程得到 EOWNERDEAD，可以把锁收回来。
     */
    if (pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) != 0) {
        fprintf(stderr, "robust mutex unavailable, continuing without it\n");
        pthread_mutexattr_destroy(&attr);
        if (pthread_mutexattr_init(&attr) != 0 ||
            pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
            pthread_mutexattr_destroy(&attr);
            return -1;
        }
    }
#endif
    if (pthread_mutex_init(&st->lock, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        return -1;
    }
    pthread_mutexattr_destroy(&attr);
    return 0;
}

SharedState *state_create(void)
{
    SharedState *st;

    st = mmap(NULL, sizeof *st, PROT_READ | PROT_WRITE,
              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (st == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }

    /* 先清零再初始化锁。顺序反了会把刚建好的锁抹掉。 */
    memset(st, 0, sizeof *st);

    if (init_shared_mutex(st) == 0) {
        st->use_mutex = 1;
        return st;
    }

    /*
     * 有的环境没有进程间 pthread 锁。数据仍然在共享页上，
     * 改用同一页里的自旋锁，fork 之后各个子进程还是同一把锁。
     */
    fprintf(stderr, "process-shared mutex unavailable, using a shared spinlock\n");
    st->use_mutex = 0;
    st->spin = 0;
    return st;
}

void state_destroy(SharedState *st)
{
    if (!st) {
        return;
    }
    if (st->use_mutex) {
        pthread_mutex_destroy(&st->lock);
    }
    munmap(st, sizeof *st);
}

void state_lock(SharedState *st)
{
    int rc;

    if (!st->use_mutex) {
        while (__sync_lock_test_and_set(&st->spin, 1)) {
            sched_yield();
        }
        return;
    }

    rc = pthread_mutex_lock(&st->lock);
#ifdef EOWNERDEAD
    if (rc == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&st->lock);
#endif
        fprintf(stderr, "recovered lock after a child died holding it\n");
        return;
    }
#endif
    if (rc != 0) {
        fprintf(stderr, "pthread_mutex_lock failed: %d\n", rc);
    }
}

void state_unlock(SharedState *st)
{
    int rc;

    if (!st->use_mutex) {
        __sync_lock_release(&st->spin);
        return;
    }
    rc = pthread_mutex_unlock(&st->lock);
    if (rc != 0) {
        fprintf(stderr, "pthread_mutex_unlock failed: %d\n", rc);
    }
}
