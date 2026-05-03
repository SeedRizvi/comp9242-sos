/*
 * Copyright 2017, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* An implementation of recursive mutexes. Assumptions are similar to pthread
 * recursive mutexes. That is, the thread that locked the mutex needs to be the
 * one who unlocks the mutex, mutexes must be unlocked as many times as they
 * are locked, etc.
 *
 * Note that the address of your IPC buffer is used as a thread ID so if you
 * have a situation where threads share an IPC buffer or do not have a valid
 * IPC buffer, these locks will not work for you.
 */

#pragma once

#include <sel4/sel4.h>

/* This struct is intended to be opaque, but is left here so you can
 * stack-allocate mutexes. Callers should not touch any of its members.
 */
typedef struct {
    seL4_CPtr notification;
    void *ntfn_ut;
    void *owner;
    unsigned int held;
} sync_recursive_mutex_t;

/* Acquire a recursive mutex
 * @param mutex         An initialised recursive mutex to acquire.
 * @return              0 on success, an error code on failure. */
int srmutex_lock(sync_recursive_mutex_t *mutex);

/* Release a recursive mutex
 * @param mutex         An initialised recursive mutex to release.
 * @return              0 on success, an error code on failure. */
int rmutex_unlock(sync_recursive_mutex_t *mutex);

