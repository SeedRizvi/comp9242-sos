#pragma once

#include "utils.h"
#include <stdbool.h>
#include <stdlib.h>
// #include <elf.h>
#include "filesystem.h"
#include "files.h"
#include "main_includes.h"

#define MAX_PROCESSES 32
#define N_NAME 32 /* From sos.h */
/* 
----------------------------------------
Process table slot status bit masks
----------------------------------------
*/
/* Slot is running */
#define FLAG_RUNNING 0x1
/* Slot is currently being deleted */
#define FLAG_KILLING 0x2
/* Slot is "free" but cannot be reused immediately */
#define FLAG_TIMEOUT 0x4 
/* Slot is ready for reuse */
#define FLAG_READY   0x8
/* Slot is claimed by new process being spawned */
#define FLAG_STARTING 0x10

/* Process specific */
#define PROC_PRIORITY APP_PRIORITY
#define APP_EP_BADGE (101)
#define PROC_WAIT_TIME 1000000

enum PROC_OPS {
    P_CREATE = 1,
    P_DEL,
    P_WAIT,
    P_STAT
};

typedef struct {
    ut_t *tcb_ut;
    seL4_CPtr tcb;
    ut_t *vspace_ut;
    seL4_CPtr vspace;

    ut_t *ipc_buffer_ut;
    seL4_CPtr ipc_buffer;

    ut_t *sched_context_ut;
    seL4_CPtr sched_context;

    bool has_cspace;
    cspace_t cspace;

    seL4_CPtr user_ep;
    seL4_CPtr proc_ep;

    seL4_CPtr stack;
} user_proc;

typedef struct {
    user_proc proc; /* Memory details of process */
    uint64_t boot; /* Boot time in msec */
    char *name; /* Name of process */
    int flags; /* Used to determine if running, exitting, or removed but not ready to reuse */
    fd_list_t *fd_list;
    pt_meta_t *pt;
    seL4_CPtr syscall_ntfn;
    ut_t *syscall_ntfn_ut;

    int num_waiting;
    seL4_CPtr waiting_ntfn;
    ut_t *waiting_ntfn_ut;

    int is_waiting;
} process_t;


// ==== Proc and Process Table ====
// Initialise the process table. Called once during startup.
void init_proc_table(seL4_CPtr ipc_endpoint, seL4_CPtr sched_start, seL4_CPtr sched_end);
void proc_table_destroy();

// Lock / unlock operations for process table.
void proc_t_lock(void);
void proc_t_unlock(void);

// Return a process' process table in a locked state.
pt_meta_t *get_pt_from_pid(int pid);

// Return a process' fd_list.
fd_list_t *get_fd_list_from_pid(int pid);

// Signal that a system call is currently running on a pid or finished on a pid.
void proc_syscall_start(int pid);
void proc_syscall_done(int pid);


// ==== Process Operations ====
void proc_delete_local(int pid);

// Caller for process operations.
int proc_ops_caller(nfs_args_t *args, pass_data_t *pd);


// ==== Process Loading ====
/**
 * Spawns a new process from a file path stored in nfs_args, or, in the case of
 * being called from start first process, a cpio image.
 * 
 * @param 'args' nfs_args_t containing fields used in process startup.
 * @param 'ep' endpoint cap for process to mint.
 * @param 'use_cpio' whether to load via cpio (first process case).
 * @returns true on success, false otherwise.
 */
bool start_process(nfs_args_t *args, seL4_CPtr ep, bool use_cpio);

// Thread function. Spawns the first process on sos.
void start_first_process(void *data);