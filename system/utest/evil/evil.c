// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>

#include <magenta/syscalls.h>
#include <magenta/types.h>

#define TICKS 0

#define USE_PTHREAD_MUTEXES 0
#define USE_SPINLOCKS 0
#define USE_FUTEXES 1

// malloc may behave differently for larger allocations (e.g., using mmap).
// Using up to 512k allocations will likely trigger this behavior.
#define LARGE_MALLOC 0
#define LARGE_MALLOC_SIZE (512 * 1024)
#define SMALL_MALLOC_SIZE 1024
#if LARGE_MALLOC
#define MALLOC_SIZE LARGE_MALLOC_SIZE
#else
#define MALLOC_SIZE SMALL_MALLOC_SIZE
#endif

static volatile int xlock = 0;

static void _lock(volatile int* lock) {
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE) != 0)
        ;
}
static void _unlock(volatile int* lock) {
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

static void _ftxlock(volatile int* lock) {
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE) != 0) {
        mx_futex_wait((int*)lock, 1, MX_TIME_INFINITE);
    }
}
static void _ftxunlock(volatile int* lock) {
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
    mx_futex_wake((int*)lock, 1);
}

#if USE_PTHREAD_MUTEXES
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()                         \
    do {                               \
        if (info->lock)                \
            pthread_mutex_lock(&lock); \
    } while (0)
#define UNLOCK()                         \
    do {                                 \
        if (info->lock)                  \
            pthread_mutex_unlock(&lock); \
    } while (0)
#endif

#if USE_SPINLOCKS
#define LOCK()             \
    do {                   \
        if (info->lock)    \
            _lock(&xlock); \
    } while (0)
#define UNLOCK()             \
    do {                     \
        if (info->lock)      \
            _unlock(&xlock); \
    } while (0)
#endif

#if USE_FUTEXES
#define LOCK()                \
    do {                      \
        if (info->lock)       \
            _ftxlock(&xlock); \
    } while (0)
#define UNLOCK()                \
    do {                        \
        if (info->lock)         \
            _ftxunlock(&xlock); \
    } while (0)
#endif

#define THREADS 8
#define BUCKETS 16

typedef struct info {
    pthread_t t;
    int n;
    int lock;
    int size[BUCKETS];
    void* bucket[BUCKETS];
} info_t;

int rnum(int m) {
    return (random() & 0x7FFFFFFFU) % m;
}

void* blaster(void* arg) {
    info_t* info = arg;
#if TICKS
    int tick = rnum(5000);
#endif

    for (;;) {
#if TICKS
        tick++;
        if (tick == 10000) {
            printf("(%d)\n", info->n);
            tick = rnum(5000);
        }
#endif
        int n = rnum(BUCKETS);
        if (info->bucket[n] == NULL) {
        allocnew:
            info->size[n] = 7 + rnum(MALLOC_SIZE);
            LOCK();
            info->bucket[n] = malloc(info->size[n]);
            UNLOCK();
            if (info->bucket[n] == NULL) {
                printf("blaster %d malloc failed %d\n", info->n, n);
                *((unsigned*)0) = 1;
            }
            memset(info->bucket[n], info->n * n, info->size[n]);
        } else {
            int sz = info->size[n];
            uint8_t* x = info->bucket[n];
            int val = n * info->n;
            for (int i = 0; i < sz; i++) {
                if (x[i] != val) {
                    printf("blaster %d bad bucket %d\n", info->n, n);
                    *((unsigned*)0) = 1;
                }
            }
            if (rnum(1000) < 750) {
                LOCK();
                free(info->bucket[n]);
                UNLOCK();
                goto allocnew;
            } else {
                memset(x, val, sz);
            }
        }
    }

    return NULL;
}

int heapblaster(int count, int locking) {
    info_t info[THREADS];
    memset(info, 0, sizeof(info));
    if (count < 1)
        count = 1;
    if (count >= THREADS)
        count = THREADS;
    printf("heapblaster: starting %d threads... (%s)\n",
           count, locking ? "locking" : "not locking");
    for (int n = 0; n < count; n++) {
        info[n].lock = locking;
        info[n].n = n;
        if (count == 1) {
            blaster(info + n);
            return 0;
        } else {
            pthread_create(&info[n].t, NULL, blaster, info + n);
        }
    }
    for (;;)
        sleep(1000);
    return 0;
}

static uint8_t data[65534];

int writespam(int opt) {
    mx_handle_t p[2];
    mx_status_t r;
    uint64_t count = 0;

    if ((r = mx_msgpipe_create(p, 0)) < 0) {
        printf("cleanup-test: pipe create 0 failed: %d\n", r);
        return -1;
    }

    printf("evil-tests: about to spam data into a pipe\n");
    for (;;) {
        count++;
        if ((r = mx_msgpipe_write(p[0], data, sizeof(data), NULL, 0, 0)) < 0) {
            printf("evil-tests: SUCCESS, writespammer error %d after only %llu writes\n", r, count);
            return 0;
        }
        if ((count % 1000) == 0) {
            printf("evil-tests: wrote %llu messages (%llu bytes).\n", count, count * sizeof(data));
        }
    }
    if (opt == 0) {
        printf("evil-tests: closing the pipe (full of messages)\n");
        mx_handle_close(p[0]);
        mx_handle_close(p[1]);
    } else {
        printf("evil-tests: leaving the pipe open (full of messages)\n");
    }
    return 0;
}

int handlespam(void) {
    mx_handle_t p[2];
    uint64_t count = 0;

    printf("evil-tests: about to create all the handles\n");
    for (;;) {
        mx_status_t status;
        if ((status = mx_msgpipe_create(p, 0)) < 0) {
            printf("evil-tests: SUCCESS, pipe create failed %d after %llu created\n", status, count);
            return 0;
        }
        count++;
        if ((count % 1000) == 0) {
            printf("evil-tests: created %llu message pipes\n", count);
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf(
            "usage: evil-tests spam1        spam writes into pipe\n"
            "       evil-tests spam2        spam writes, don't close pipe after\n"
            "       evil-tests spam3        spam handle creation\n"
            "       evil-tests heap1 <n>    heap stress test, locking\n"
            "       evil-tests heap2 <n>    heap stress test, no locking\n");
        return -1;
    } else if (!strcmp(argv[1], "spam1")) {
        return writespam(0);
    } else if (!strcmp(argv[1], "spam2")) {
        return writespam(1);
    } else if (!strcmp(argv[1], "spam3")) {
        return handlespam();
    } else if (!strcmp(argv[1], "heap1")) {
        int n = (argc > 2) ? strtoul(argv[2], 0, 10) : THREADS;
        return heapblaster(n, 1);
    } else if (!strcmp(argv[1], "heap2")) {
        int n = (argc > 2) ? strtoul(argv[2], 0, 10) : THREADS;
        return heapblaster(n, 0);
    } else {
        printf("unknown sub-command '%s'\n", argv[1]);
        return -1;
    }
    return 0;
}
