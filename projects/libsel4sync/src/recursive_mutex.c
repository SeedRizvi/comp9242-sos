/*
 * Copyright 2017, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <autoconf.h>
#include <sync/recursive_mutex.h>
#include <stddef.h>
#include <assert.h>
#include <limits.h>

#include <sel4/sel4.h>
#ifdef CONFIG_DEBUG_BUILD
#include <sel4debug/debug.h>
#endif

static void *thread_id(void)
{
    return (void *)seL4_GetIPCBuffer();
}

int rmutex_lock(sync_recursive_mutex_t *mutex)
{
    if (mutex == NULL) {
        ZF_LOGE("Mutex passed to sync_recursive_mutex_lock is NULL");
        return -1;
    }
    if (thread_id() != mutex->owner) {
        /* We don't already have the mutex. */
        seL4_Wait(mutex->notification, NULL);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        assert(mutex->owner == NULL);
        mutex->owner = thread_id();
        assert(mutex->held == 0);
    }
    if (mutex->held == UINT_MAX) {
        /* We would overflow if we re-acquired the mutex. Note that we can only
         * be in this branch if we already held the mutex before entering this
         * function, so we don't need to release the mutex here.
         */
        return -1;
    }
    mutex->held++;
    return 0;
}

int rmutex_unlock(sync_recursive_mutex_t *mutex)
{
    if (mutex == NULL) {
        ZF_LOGE("Mutex passed to sync_recursive_mutex_lock is NULL");
        return -1;
    }
    assert(mutex->owner == thread_id());
    assert(mutex->held > 0);
    mutex->held--;
    if (mutex->held == 0) {
        /* This was the outermost lock we held. Wake the next person up. */
        __atomic_store_n(&mutex->owner, NULL, __ATOMIC_RELEASE);
        seL4_Signal(mutex->notification);
    }
    return 0;
}
