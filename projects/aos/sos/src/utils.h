/*
 * Copyright 2019, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(DATA61_GPL)
 */
#pragma once

#include <sel4runtime.h>
#include "ut.h"
#include "frame_table.h"
#include <stdbool.h>

#define SOS_THREAD_BADGE 10000

extern cspace_t cspace;

/* helper to allocate a ut + cslot, and retype the ut into the cslot */
ut_t *alloc_retype(seL4_CPtr *cptr, seL4_Word type, size_t size_bits);

/**
 * Allocate and map a new frame into the page table provided.
 *
 * @param pt            page table to map frame into.
 * @param vspace        vspace to map frame to.
 * @param frame_cptr    cptr to assign frame_cptr value to.
 * @param vaddr         vaddr to map frame to.
 * @param rights        caprights to apply to frame cap.
 * @param attr          attributes to pass to mapping.
 * @return 1 on failure, 0 on success.
 */
int alloc_map_frame(pt_meta_t *pt, seL4_CPtr vspace, seL4_CPtr *frame_cptr, uintptr_t vaddr, seL4_CapRights_t rights, seL4_ARM_VMAttributes attr);

int new_notification(seL4_CPtr *ntfn, ut_t **ntfn_ut, bool signal);
void remove_notification(unsigned long ntfn, void *ntfn_ut);

int new_reply(seL4_CPtr *reply, ut_t **reply_ut);
int copy_reply(seL4_CPtr src_reply, ut_t *src_reply_ut, seL4_CPtr *dst_reply, ut_t **dst_reply_ut);
void remove_reply(seL4_CPtr reply, ut_t *reply_ut);

// Makes a system call to the main system call loop. The system call is handled in handle_os_thread_exit()
// in serverside, safely cleaning up the sos thread.
void os_exit();
int os_thread_init(void *data);
void os_thread_clear(void *data);

// Initialise and enable locking for misc libraries and files such as ut.c and libsel4cspace.
void enable_std_locking();

typedef struct sync_recursive_mutex {
    seL4_CPtr notification;
    ut_t *ntfn_ut;
    void *owner;
    unsigned int held;
} sync_recursive_mutex_t;

int init_rmutex(sync_recursive_mutex_t *rec);
void remove_rmutex(sync_recursive_mutex_t *rmutex);

/* Acquire a recursive mutex
 * @param mutex         An initialised recursive mutex to acquire.
 * @return              0 on success, an error code on failure. */
int rmutex_lock(sync_recursive_mutex_t *mutex);

/* Release a recursive mutex
 * @param mutex         An initialised recursive mutex to release.
 * @return              0 on success, an error code on failure. */
int rmutex_unlock(sync_recursive_mutex_t *mutex);