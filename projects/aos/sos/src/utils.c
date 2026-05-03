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
#include "utils.h"
#include "clock_sync.h"

#include <sel4runtime.h>
#include <cspace/cspace.h>
#include <aos/sel4_zf_logif.h>

#include "vmem_layout.h"

#include <stdio.h>
#include <stdarg.h>
#include "ut.h"
#include "mapping.h"
#include "threads.h"
#include "syscall_codes.h"
#include "frame_table.h"

#include <autoconf.h>
#include <stddef.h>
#include <assert.h>
#include <limits.h>

static bool std_lock = false;

// ===================================
// ==== MEMORY ALLOCATION HELPERS ====
// ===================================
/* helper to allocate a ut + cslot, and retype the ut into the cslot */
ut_t *alloc_retype(seL4_CPtr *cptr, seL4_Word type, size_t size_bits)
{
    /* Allocate the object */
    ut_t *ut = ut_alloc(size_bits, &cspace);
    if (ut == NULL) {
        ZF_LOGE("No memory for object of size %zu\n", size_bits);
        return NULL;
    }

    /* allocate a slot to retype the memory for object into */
    *cptr = cspace_alloc_slot(&cspace);
    if (*cptr == seL4_CapNull) {
        ut_free(ut);
        ZF_LOGE("Failed to allocate slot\n");
        return NULL;
    }

    /* now do the retype */
    seL4_Error err = cspace_untyped_retype(&cspace, ut->cap, *cptr, type, size_bits);
    ZF_LOGE_IFERR(err, "Failed retype untyped\n");
    if (err != seL4_NoError) {
        ut_free(ut);
        cspace_free_slot(&cspace, *cptr);
        return NULL;
    }

    return ut;
}

/* helper to allocate a frame + cslot and map frame to pt */
int alloc_map_frame(pt_meta_t *pt, seL4_CPtr vspace, seL4_CPtr *frame_cptr, uintptr_t vaddr, seL4_CapRights_t rights, seL4_ARM_VMAttributes attr)
{
    frame_ref_t frame = alloc_frame_pt(pt, vaddr);
    if (frame == NULL_FRAME) {
        ZF_LOGE("Couldn't allocate frame\n");
        return 1;
    }

    // Zero out frame prior to mapping.
    unsigned char *data = frame_data(frame);
    if (data == NULL) {
        ZF_LOGE("Frame ref not linked to frame\n");
        return 1;
    }
    memset(data, 0, PAGE_SIZE_4K);

    *frame_cptr = cspace_alloc_slot(&cspace);
    if (*frame_cptr == seL4_CapNull) {
        free_frame(frame);
        ZF_LOGE("Failed to alloc slot for frame\n");
        return 1;
    }

    seL4_Error err = cspace_copy(&cspace, *frame_cptr, &cspace, frame_page(frame), seL4_AllRights);
    if (err != seL4_NoError) {
        cspace_free_slot(&cspace, *frame_cptr);
        free_frame(frame);
        ZF_LOGE("Failed to copy cap\n");
        return 1;
    }

    err = sos_map_frame(pt, *frame_cptr, vspace, vaddr,
                    rights, attr, frame);
    if (err != 0) {
        cspace_delete(&cspace, *frame_cptr);
        cspace_free_slot(&cspace, *frame_cptr);
        free_frame(frame);
        ZF_LOGE("Unable to map frame\n");
        return 1;
    }
    
    return 0;
}


// ============================================
// ==== Kernel Object Allocation + Removal ====
// ============================================
static int num_ntfn = 0;
static int num_reply = 0;

int new_notification(seL4_CPtr *ntfn, ut_t **ntfn_ut, bool signal) {
    ZF_LOGD("New notification allocation\n");
    *ntfn_ut = alloc_retype(ntfn, seL4_NotificationObject, seL4_NotificationBits);
    if (ntfn_ut == NULL) {
        ZF_LOGE("Error: could not allocate new notification object\n");
        return 1;
    }
    if (signal) {
        ZF_LOGV("Signalling new notification on allocataion\n");
        seL4_Signal(*((seL4_CPtr *)(ntfn)));
    }
    ZF_LOGD("Num NTFN = %d\n", ++num_ntfn);
    return 0;
}

void remove_notification(unsigned long ntfn, void *ntfn_ut) {
    ZF_LOGD("Freeing notification\n");
    cspace_delete(&cspace, ntfn);
    cspace_free_slot(&cspace, ntfn);
    ut_free(ntfn_ut);
    ZF_LOGD("Num NTFN = %d\n", --num_ntfn);
}

int init_rmutex(sync_recursive_mutex_t *rec) {
    if (new_notification(&rec->notification, &rec->ntfn_ut, true)) {
        return 1;
    }
    rec->owner = NULL;
    rec->held = 0;
    return 0;
}

// Assumes non allocated memory (static).
void remove_rmutex(sync_recursive_mutex_t *rmutex) {
    remove_notification(rmutex->notification, rmutex->ntfn_ut);
    rmutex->owner = NULL;
    rmutex->held = 0;
}

static void *thread_id(void)
{
    return (void *)seL4_GetIPCBuffer();
}

int rmutex_lock(sync_recursive_mutex_t *mutex)
{
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
        ZF_LOGE("Mutex passed to sync_recursive_mutex_lock is NULL\n");
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

int new_reply(seL4_CPtr *reply, ut_t **reply_ut) {
     ZF_LOGD("New reply allocation\n");
    *reply_ut = alloc_retype(reply, seL4_ReplyObject, seL4_ReplyBits);
    if (*reply_ut == NULL) {
        ZF_LOGE("Failed to alloc reply object ut\n");
        return 1;
    }
    ZF_LOGD("Num Reply = %d\n", ++num_reply);
    return 0;
}

int copy_reply(seL4_CPtr src_reply, ut_t *src_reply_ut, seL4_CPtr *dst_reply, ut_t **dst_reply_ut) {
    ZF_LOGD("Copying existing reply\n");
    *dst_reply = cspace_alloc_slot(&cspace);
    if (*dst_reply == seL4_CapNull) {
        ZF_LOGE("Failed to alloc slot for reply copy\n");
        return 1;
    }
    if (cspace_copy(&cspace, *dst_reply, &cspace, src_reply, seL4_AllRights)) {
        ZF_LOGE("Failed to copy reply object ut\n");
        cspace_free_slot(&cspace, *dst_reply);
        return 1;
    }
    *dst_reply_ut = src_reply_ut;
    return 0;
}

void remove_reply(seL4_CPtr reply, ut_t *reply_ut) {
    ZF_LOGD("Freeing reply\n");
    cspace_delete(&cspace, reply);
    cspace_free_slot(&cspace, reply);
    if (reply_ut != NULL) {
        ut_free(reply_ut);
        ZF_LOGD("Num Reply = %d\n", --num_reply);
    }
}


// =======================
//==== THREAD HELPERS ====
// =======================
void os_exit() {
    ZF_LOGD("OS_EXIT\n");
    seL4_SetMR(0, SOS_SYSCALL_EXIT_OS_THREAD);
    seL4_SetMR(1, (seL4_Word)current_thread);
    seL4_Call(current_thread->user_ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

int os_thread_init(void *data) {
    sos_thread_t *f = (data);

    f->stack_arr = malloc(sizeof(seL4_CPtr) * SOS_STACK_PAGES);
    if (f->stack_arr == NULL) {
        return 1;
    }
    f->stack_arr_ut = malloc(sizeof(ut_t *) * SOS_STACK_PAGES);
    if (f->stack_arr_ut == NULL) {
        free(f->stack_arr);
        return 1;
    }

    for (int i = 0; i < SOS_STACK_PAGES; i++) {
        f->stack_arr[i] = seL4_CapNull;
    }
    for (int i = 0; i < SOS_STACK_PAGES; i++) {
        f->stack_arr_ut[i] = NULL;
    }

    f->tcb = seL4_CapNull;
    f->user_ep = seL4_CapNull;
    f->fault_ep = seL4_CapNull;
    f->ipc_buffer = seL4_CapNull;
    f->ipc_buffer_vaddr = seL4_CapNull;
    f->sched_context = seL4_CapNull;
    f->stack = seL4_CapNull;
    f->badge = seL4_CapNull;

    f->ipc_buffer_ut = NULL;
    f->tcb_ut = NULL;
    f->sched_context_ut = NULL;
    f->stack_ut = NULL;
    f->tls_base = 0;

    return 0;
}

void os_thread_clear(void *data) {
    sos_thread_t *f = (data);

    if (f == NULL) {
        return;
    }

    if (f->tls_base != 0) {
        free((void *)f->tls_base);
    }

    if (f->stack_arr != NULL) {
        for (int i = 0; i < SOS_STACK_PAGES; i++) {
            if ((f->stack_arr)[i] != seL4_CapNull) {
                seL4_ARM_Page_Unmap(f->stack_arr[i]);
                cspace_delete(&cspace, (f->stack_arr)[i]);
                cspace_free_slot(&cspace, (f->stack_arr)[i]);
            }
        }
    }
    
    if (f->stack_arr_ut != NULL) {
        for (int i = 0; i < SOS_STACK_PAGES; i++) {
            if ((f->stack_arr_ut)[i] != NULL) {
                ut_free((f->stack_arr_ut)[i]);
            }
        }
    }

    if (f->sched_context != seL4_CapNull) {
        cspace_delete(&cspace, f->sched_context);
        cspace_free_slot(&cspace, f->sched_context);
    }
    
    if (f->sched_context_ut != NULL) {
        ut_free(f->sched_context_ut);
    }
   
    if (f->ipc_buffer != seL4_CapNull) {
        cspace_delete(&cspace, f->ipc_buffer);
        cspace_free_slot(&cspace, f->ipc_buffer);
    }
    
    if (f->ipc_buffer_ut != NULL) {
        ut_free(f->ipc_buffer_ut);
    }

    if (f->fault_ep != seL4_CapNull) {
        cspace_delete(&cspace, f->fault_ep);
        cspace_free_slot(&cspace, f->fault_ep);
    }
   
    if (f->user_ep != seL4_CapNull) {
        cspace_delete(&cspace, f->user_ep);
        cspace_free_slot(&cspace, f->user_ep);
    }

    if (f->tcb != seL4_CapNull) {
        cspace_delete(&cspace, f->tcb);
        cspace_free_slot(&cspace, f->tcb);
    }
    
    if (f->tcb_ut != NULL) {
        ut_free(f->tcb_ut);
    }

    if (f->stack_arr != NULL) {
        free(f->stack_arr);
    }
    
    if (f->stack_arr_ut != NULL) {
        free(f->stack_arr_ut);
    }
    
    free(f);
}

// ====================
// ==== CLOCK SYNC ====
// ====================
static sync_recursive_mutex_t clock_rmutex;

static seL4_CPtr clock_test_ntfn;
static ut_t *clock_test_ntfn_ut;

int init_clock_sync() {
    ZF_LOGI("Initialising clock synchronisation notifications\n");
    if (init_rmutex(&clock_rmutex)) {
        ZF_LOGF("!!! could not allocate mutex for synchronistaion of clock !!!");
    }
    return 0;
}

void clock_wait() {
    ZF_LOGV("Clock Wait Start\n");
    rmutex_lock(&clock_rmutex);
    ZF_LOGV("Clock Wait End\n");
}

void clock_signal() {
    ZF_LOGV("Clock Signal\n");
    rmutex_unlock(&clock_rmutex);
}

int init_clock_test_sync() {
    ZF_LOGI("Initialising clock test synchronisation notifications\n");
    if (new_notification(&clock_test_ntfn, (void *)&clock_test_ntfn_ut, false) != 0) {
        ZF_LOGF("Failed to initialise clock test sync\n");
        return 1;
    }
    return 0;
}

void destroy_clock_test_sync() {
    ZF_LOGI("Destroying clock test synchronisation notifications\n");
    remove_notification(clock_test_ntfn, clock_test_ntfn_ut);
}

void clock_test_signal() {
    ZF_LOGD("Clock Test Signal\n");
    seL4_Signal(clock_test_ntfn);
}

void clock_test_wait() {
    ZF_LOGD("Clock Test Wait Start\n");
    seL4_Wait(clock_test_ntfn, NULL);
    ZF_LOGD("Clock Test Wait End\n");
}


// ==================
// ==== STD SYNC ====
// ==================
static sync_recursive_mutex_t cspace_ut_rmutex;
static sync_recursive_mutex_t nfs_rmutex;
static sync_recursive_mutex_t eth_rmutex;
static sync_recursive_mutex_t brk_rmutex;
static sync_recursive_mutex_t pico_rmutex;

void enable_std_locking() {
    if (init_rmutex(&cspace_ut_rmutex)) {
        ZF_LOGF("!!! could not allocate mutex for synchronistaion of cspace and ut resources !!!");
    }
    if (init_rmutex(&nfs_rmutex)) {
        ZF_LOGF("!!! could not allocate mutex for synchronistaion of nfs resources !!!");
    }
    if (init_rmutex(&eth_rmutex)) {
        ZF_LOGF("!!! could not allocate mutex for synchronistaion of eth resources !!!");
    }
    if (init_rmutex(&brk_rmutex)) {
        ZF_LOGF("!!! could not allocate mutex for synchronistaion of brk resources !!!");
    }
    if (init_rmutex(&pico_rmutex)) {
        ZF_LOGF("!!! could not allocate mutex for synchronistaion of pico resources !!!");
    }
    std_lock = true;
}

void cspace_ut_lock() {
    if (std_lock) {
        ZF_LOGV("cspace/ut Wait Start\n");
        rmutex_lock(&cspace_ut_rmutex);
        ZF_LOGV("cspace/ut Wait End\n");
    }
}

void cspace_ut_unlock() {
    if (std_lock) {
        ZF_LOGV("cspace/ut Signal\n");
        rmutex_unlock(&cspace_ut_rmutex);
    }
}

void nfslib_lock() {
    if (std_lock) {
        ZF_LOGD("nfslib Wait Start\n");
        rmutex_lock(&nfs_rmutex);
        ZF_LOGD("nfslib Wait End\n");
    }
}

void nfslib_unlock() {
    if (std_lock) {
        ZF_LOGD("nfslib Signal\n");
        rmutex_unlock(&nfs_rmutex);
    }
}

void eth_lock() {
    if (std_lock) {
        ZF_LOGV("eth Wait Start\n");
        rmutex_lock(&eth_rmutex);
        ZF_LOGV("eth Wait End\n");
    }
}

void eth_unlock() {
    if (std_lock) {
        ZF_LOGV("eth Signal\n");
        rmutex_unlock(&eth_rmutex);
    }
}

void brk_lock() {
    if (std_lock) {
        ZF_LOGV("brk Wait Start\n");
        rmutex_lock(&brk_rmutex);
        ZF_LOGV("brk Wait End\n");
    }
}

void brk_unlock() {
    if (std_lock) {
        ZF_LOGV("brk Signal\n");
        rmutex_unlock(&brk_rmutex);
    }
}

void pico_lock() {
    if (std_lock) {
        ZF_LOGV("pico Wait Start\n");
        rmutex_lock(&pico_rmutex);
        ZF_LOGV("pico Wait End\n");
    }
}

void pico_unlock() {
    if (std_lock) {
        ZF_LOGV("pico Signal\n");
        rmutex_unlock(&pico_rmutex);
    }
}

#define PICO_SUPPORT_MUTEX

// Existing definitions existed, remove.
/*
void *pico_mutex_init() {
    return &pico_rmutex;
}

void pico_mutex_lock(void *lock) {
    if (std_lock) {
        ZF_LOGE("pico_q Wait Start stack\n");
        sync_recursive_mutex_t *r = (lock);
        rmutex_lock(r);
        ZF_LOGE("pico_q Wait End stack\n");
    }
}

void pico_mutex_unlock(void *lock) {
    if (std_lock) {
        ZF_LOGE("pico_q Signal stack\n");
        sync_recursive_mutex_t *r = (lock);
        rmutex_unlock(r);
    }
}
*/