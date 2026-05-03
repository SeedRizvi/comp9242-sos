#include <elf/elf.h>
#include <cpio/cpio.h>
#include <aos/debug.h>
#include <fcntl.h>
#include <clock/clock.h>
#include <string.h>

#include "process.h"
#include "mapping.h"
#include "elfload.h"
#include "threads.h"
#include "vmem_layout.h"
#include "utils.h"
#include "serverside.h"
#include "vmem.h"
#include "filesystem.h"
#include "files.h"
#include "../../libsosapi/include/sos.h"

#include "elf_file.h"

//#define APP_NAME             "console_test"
#define APP_NAME             "sosh"

static seL4_CPtr ipc_ep;
static seL4_CPtr sched_ctrl_start;
static seL4_CPtr sched_ctrl_end;
static process_t processes[MAX_PROCESSES];
static sync_recursive_mutex_t proc_t_rmutex;
static int pid_idx = 0;


// ===============================
// ==== Proc and Proc Table ======
// ===============================
void init_proc_table(seL4_CPtr ipc_endpoint, seL4_CPtr sched_start, seL4_CPtr sched_end) {
    ipc_ep = ipc_endpoint;

    // Start/End Cptrs to Schedule Control object, which sets scheduling contexts
    sched_ctrl_start = sched_start;
    sched_ctrl_end = sched_end;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        processes[i].flags = FLAG_READY;
        processes[i].boot = 0;
        processes[i].name = NULL;
        processes[i].pt = NULL;
        processes[i].fd_list = NULL;
    }

    if (init_rmutex(&proc_t_rmutex)) {
        ZF_LOGE("Could not allocate ntfn for process table lock");
        return;
    }
}

void proc_table_destroy() {
    remove_rmutex(&proc_t_rmutex);
}

void proc_t_lock(void) {
    ZF_LOGD("ProcT Wait Start\n");
    rmutex_lock(&proc_t_rmutex);
    ZF_LOGD("ProcT Wait End\n");
}

void proc_t_unlock(void) {
    ZF_LOGD("ProcT Signal\n");
    rmutex_unlock(&proc_t_rmutex);
}

pt_meta_t *get_pt_from_pid(int pid) {
    ZF_LOGD("Get pt from pid %d\n", pid);
    proc_t_lock();
    pt_meta_t *ret = processes[pid].pt;
    pt_lock(ret);
    proc_t_unlock();
    return ret;
}

fd_list_t *get_fd_list_from_pid(int pid) {
    ZF_LOGD("Get fd_list from pid %d\n", pid);
    proc_t_lock();
    fd_list_t *ret = processes[pid].fd_list;
    proc_t_unlock();
    return ret;
}

void proc_syscall_start(int pid) {
    ZF_LOGD("Syscall start on PID = %d\n", pid);
    proc_t_lock();
    ZF_LOGD("Locking syscall mutex for pid %d\n", pid);
    seL4_Wait(processes[pid].syscall_ntfn, NULL);
    proc_t_unlock();
}

void proc_syscall_done(int pid) {
    ZF_LOGD("Syscall done on PID = %d\n", pid);
    proc_t_lock();
    ZF_LOGD("Unlocking syscall mutex for pid %d\n", pid);
    seL4_Signal(processes[pid].syscall_ntfn);
    proc_t_unlock();
}

int get_free_idx() {
    int ret = -1;
    int loops = 0;
    proc_t_lock();
    for (; pid_idx < MAX_PROCESSES; pid_idx++, loops++) {
        if (processes[pid_idx].flags & FLAG_TIMEOUT) processes[pid_idx].flags = FLAG_READY;
        else if (processes[pid_idx].flags & FLAG_READY) {
            // Indicate this pid as taken
            processes[pid_idx].flags = FLAG_STARTING;
            ret = pid_idx;
            break;
        }
        if (pid_idx == MAX_PROCESSES - 1) {
            pid_idx = -1;
        }
        if (loops == (MAX_PROCESSES - 1) * 2) {
            ZF_LOGE("Could not find free space in process table to spawn process\n");
            break;
        }
    }
    proc_t_unlock();
    return ret;
}

void init_user_process(user_proc *user_process) {
    user_process->tcb_ut = NULL;
    user_process->tcb = seL4_CapNull;
    user_process->vspace_ut = NULL;
    user_process->vspace = seL4_CapNull;
    user_process->ipc_buffer_ut = NULL;
    user_process->ipc_buffer = seL4_CapNull;
    user_process->sched_context_ut = NULL;
    user_process->sched_context = seL4_CapNull;
    user_process->has_cspace = false;
    user_process->stack = seL4_CapNull;
    user_process->user_ep = seL4_CapNull;
    user_process->proc_ep = seL4_CapNull;
}

// NOTE: Expects non-allocated user_process, despite pointer.
// Frees pt, fd_list, proc name, and the proc struct.
void release_user_process(user_proc *user_process, int pid) {
    if (pid >= 0  && pid < MAX_PROCESSES) {

        ZF_LOGE("Freeing page table for process %d\n", pid);
        if (processes[pid].pt != NULL) {
            pt_destroy(processes[pid].pt);
            proc_t_lock();
            processes[pid].pt = NULL;
            proc_t_unlock();
        }

        ZF_LOGE("Freeing fd_list for process %d\n", pid);
        if (processes[pid].fd_list != NULL) {
            fd_list_destroy(processes[pid].fd_list);
            proc_t_lock();
            processes[pid].fd_list = NULL;
            proc_t_unlock();
        }

        ZF_LOGE("Freeing name for process %d\n", pid);
        if (processes[pid].name != NULL) {
            free(processes[pid].name);
            proc_t_lock();
            processes[pid].name = NULL;
            proc_t_unlock();
        }
    }
    
    ZF_LOGE("Freeing tcb for process %d\n", pid);
    if (user_process->tcb != seL4_CapNull) {
        cspace_delete(&cspace, user_process->tcb);
        cspace_free_slot(&cspace, user_process->tcb);
    }

    ZF_LOGE("Freeing vspace for process %d\n", pid);
    if (user_process->vspace != seL4_CapNull) {
        cspace_delete(&cspace, user_process->vspace);
        cspace_free_slot(&cspace, user_process->vspace);
    }

    ZF_LOGE("Freeing ipc buffer for process %d\n", pid);
    if (user_process->ipc_buffer != seL4_CapNull) {
        cspace_delete(&cspace, user_process->ipc_buffer);
        cspace_free_slot(&cspace, user_process->ipc_buffer);
    }

    ZF_LOGE("Freeing scheduling context for process %d\n", pid);
    if (user_process->sched_context != seL4_CapNull) {
        cspace_delete(&cspace, user_process->sched_context);
        cspace_free_slot(&cspace, user_process->sched_context);
    }

    ZF_LOGE("Freeing stack for process %d\n", pid);
    if (user_process->stack != seL4_CapNull) {
        cspace_delete(&cspace, user_process->stack);
        cspace_free_slot(&cspace, user_process->stack);
    }

    ZF_LOGE("Freeing user_ep for process %d\n", pid);
    if (user_process->user_ep != seL4_CapNull) {
        cspace_delete(&user_process->cspace, user_process->user_ep);
        cspace_free_slot(&user_process->cspace, user_process->user_ep);
    }

    ZF_LOGE("Freeing process_ep for process %d\n", pid);
    if (user_process->proc_ep != seL4_CapNull) {
        cspace_delete(&cspace, user_process->proc_ep);
        cspace_free_slot(&cspace, user_process->proc_ep);
    }

    ZF_LOGE("Freeing ut memory for process %d\n", pid);
    if (user_process->tcb_ut != NULL) ut_free(user_process->tcb_ut);
    if (user_process->vspace_ut != NULL) ut_free(user_process->vspace_ut);
    if (user_process->ipc_buffer_ut != NULL) ut_free(user_process->ipc_buffer_ut);
    if (user_process->sched_context_ut != NULL) ut_free(user_process->sched_context_ut);

    ZF_LOGE("Freeing csapce for process %d\n", pid);
    if (user_process->has_cspace) {
        cspace_destroy(&user_process->cspace);
    }

    ZF_LOGE("Finished freeing in release for %d\n", pid);

    /* If the flag was set to TIMEOUT, we must be called from proc_delete,
    so we dont want to override timeout period */
    proc_t_lock();
    if (pid >= 0 && !(processes[pid].flags & FLAG_TIMEOUT)) {
        processes[pid].flags = FLAG_READY;
    }
    proc_t_unlock();
}


// ===========================
// ==== OS STAT HELPERS ======
// ===========================
nfs_args_t *new_os_nfs_stat_args(nfs_args_t *args) {
    nfs_args_t *ret = new_nfs_blank_arg();
    if (ret == NULL) {
        ZF_LOGE("Error: could not allocate stat args in proc_create\n");
        return NULL;
    }
    ret->operation = FS_STAT;
    ret->pathlen = args->pathlen;
    ret->name = args->name;
    ret->extra_buf = malloc(sizeof(sos_stat_t));
    if (ret->extra_buf == NULL) {
        ZF_LOGE("Error: could not allocate extra_buf for stat in proc_create\n");
        return NULL;
    }
    return ret;
}

void free_os_nfs_stat_args(nfs_args_t *args) {
    args->name = NULL;
    free_nfs_args(args);
}


// ===========================
// ==== Proc Operations ======
// ===========================
void proc_create(nfs_args_t *args) {
    ZF_LOGI("Process create started\n");
    
    nfs_args_t* stat = new_os_nfs_stat_args(args);
    fs_ops_caller(stat);
    sos_stat_t *stats = (sos_stat_t *)stat->extra_buf;

    if (!(stats->st_fmode & FM_EXEC)) {
        ZF_LOGE("Process 'path' is not executable");
        args->result = -1;
        return;
    }
    ZF_LOGI("Process 'path' is valid executable");
    
    args->usr_buf = stats->st_size;
    free_os_nfs_stat_args(stat);
    
    int pid = get_free_idx();
    if (pid < 0) {
        ZF_LOGE("No free slot in process table\n");
        args->result = -1;
        return;
    }

    ZF_LOGI("Assigning pid: %d", pid);
    args->pid = pid;
    
    // Try spawn process in its own thread
    if (!start_process(args, ipc_ep, false)) {
        ZF_LOGE("Error: could not start process");
        args->result = -1;
        return;
    }
}

int proc_delete_logic(int pid, pass_data_t *pd) {
ZF_LOGI("Process deletion started for pid: %d\n", pid);
    proc_t_lock();
    if (pid >= MAX_PROCESSES || pid < 0 || processes[pid].flags != FLAG_RUNNING) {
        proc_t_unlock();
        return -1;
    }

    seL4_TCB_Suspend(processes[pid].proc.tcb);
    processes[pid].flags = FLAG_KILLING;
    
    // Logic to handle if we are waiting on another process.
    int waiting_on_pid = processes[pid].is_waiting;
    if (waiting_on_pid != -1) {
        processes[waiting_on_pid].num_waiting--;
        processes[pid].is_waiting = -1;
    }
    proc_t_unlock();

    seL4_Wait(processes[pid].syscall_ntfn, NULL);

    // Signal waiting processes to continue.
    while (1) {
        seL4_Signal(processes[pid].waiting_ntfn);
        proc_t_lock();
        if (processes[pid].num_waiting == 0) {
            proc_t_unlock();
            break;
        }
        proc_t_unlock();
    }

    processes[pid].flags = FLAG_TIMEOUT;
    if (pd != NULL) {
        send_result(pd, 0, 0);
    }

    release_user_process(&processes[pid].proc, pid);
    processes[pid].boot = 0;
    processes[pid].pt = NULL;
    processes[pid].fd_list = NULL;
    processes[pid].name = NULL;
    processes[pid].num_waiting = 0;
    processes[pid].is_waiting = -1;
    remove_notification(processes[pid].syscall_ntfn, processes[pid].syscall_ntfn_ut);
    remove_notification(processes[pid].waiting_ntfn, processes[pid].waiting_ntfn_ut);
    ZF_LOGE("Process deletion completed for pid: %d\n", pid);
    return 0;
}

// Wrapper for proc delete used for calls within sos.
void proc_delete_local(int pid) {
    proc_delete_logic(pid, NULL);
}

// Kill a running process.
void proc_delete(nfs_args_t *args, pass_data_t *pd) {
    int err = proc_delete_logic(args->pos, pd);
    if (err) {
        send_result(pd, 1, 0);
    }
    os_exit();
}

// TODO: Track wait threads to avoid lingering wait threads.
void wait_logic(int our_pid, int pid, bool *waited) {
    proc_t_lock();
    int status = processes[pid].flags;
    if (status == FLAG_RUNNING) {
        processes[our_pid].is_waiting = pid;
        processes[pid].num_waiting++;
        proc_t_unlock();
        seL4_Wait(processes[pid].waiting_ntfn, NULL);
        *waited = true;
        proc_t_lock();
        if (processes[our_pid].is_waiting != -1) {
            processes[pid].num_waiting--;
            processes[our_pid].is_waiting = -1;
        }
    }
    proc_t_unlock();
}

int looping_increment(int pid) {
    pid++;
    if (pid >= MAX_PROCESSES) {
        pid = 0;
    }
    return pid;
}

/**
 * Waits for process of pid "args->pos" to exit. If this pid is in use, blocks
 * until it has exited or is exiting. If the PID is exactly '-1', chooses any PID
 * and blocks until it has exited or is exiting. For completeness, if neither of 
 * the above are satisfied, the PID is treated as if it were '-1'.
 * @param 'id' unused, needed for compatability with timer_callback. 
 * @param 'args', pointer to nfs_args_t in form of void* for timer_callback.
 * @returns -1 if waiting on proc exit, pid of exited process otherwise.
 */
void proc_wait(void *data) {
    nfs_args_t *args = (nfs_args_t *)data;
    if (args->pos == args->pid) return;
    ZF_LOGI("Process %d waiting on pid: %d\n", args->pid, args->pos);

    proc_t_lock();
    if (args->pos >= MAX_PROCESSES || args->pos < 0 || processes[args->pos].flags != FLAG_RUNNING) {
        args->result = -1;
        proc_t_unlock();
        return;
    }
    proc_t_unlock();

    bool waited = false;
    if (args->pos == -1) {
        int pid = 0;
        while (!waited) {
            if (pid != args->pid) wait_logic(args->pid, pid, &waited);
            pid = looping_increment(pid);
        }
    } else {
        wait_logic(args->pid, args->pos, &waited);
    }
}

void proc_status(nfs_args_t *args) {
    int max = args->pos;
    int cur = 0;
    sos_process_t *buf = args->extra_buf;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        proc_t_lock();
        int status = processes[i].flags;
        if (cur == max) {
            proc_t_unlock();
            break;
        }
        if (status & FLAG_RUNNING) {
            buf[cur].pid = i;
            pt_meta_t *temp = get_pt_from_pid(i);
            buf[cur].size = temp->total_pages;
            pt_unlock(temp);
            buf[cur].stime = processes[i].boot;
            memcpy(buf[cur].command, processes[i].name, 12);
            cur++;
        }
        proc_t_unlock();
    }
    args->result = cur;
}

int proc_ops_caller(nfs_args_t *args, pass_data_t *pd) {
    switch (args->operation) {
        case P_CREATE:
            proc_create(args);
            break;
        case P_DEL:
            proc_delete(args, pd);
            break;
        case P_WAIT:
            proc_wait(args);
            break;
        case P_STAT:
            proc_status(args);
            break;
    }
    return args->result;
}


// =========================
// ==== PROCESS LOADING ====
// =========================
bool init_elf_file(nfs_args_t *args, bool use_cpio, elf_t *elf_file) {
    elf_file->read = !use_cpio;
    elf_file->pid = args->pid;

    /* parse the cpio image */
    if (use_cpio) {
        unsigned long elf_size;
        size_t cpio_len = _cpio_archive_end - _cpio_archive;
        const char *elf_base = cpio_get_file(_cpio_archive, cpio_len, args->name, &elf_size);
        if (elf_base == NULL) {
            ZF_LOGE("Unable to locate cpio header for %s\n", args->name);
            return false;
        }
        /* Ensure that the file is an elf file. */
        if (elf_newFile(elf_base, elf_size, elf_file)) {
            ZF_LOGE("Invalid elf file\n"); 
            return false;
        }
    }
    else {
        // Opens executable file.
        nfs_args_t *open_args = new_nfs_blank_arg();
        open_args->operation = FS_OPEN;
        open_args->devtype = FILE_DEVTYPE;
        open_args->flags = O_RDONLY;
        open_args->name = args->name;
        open_args->pathlen = args->pathlen;
        open_args->pid = args->pid;
        fs_ops_caller(open_args);
        if (open_args->result != ELF_FILE_FD) {
            ZF_LOGE("Error: elf file opened on unexpected fd = %d\n", open_args->result);
            free_nfs_args(open_args);
            return false;
        }
        open_args->name = NULL;
        free_nfs_args(open_args);
        
        // Read first page (header) into buffer.
        char *buf = (char *)read_fd_to_buf(args->pid, ELF_FILE_FD, ELF_HEADER_OFFSET, PAGE_SIZE_4K);
        if (buf == NULL) {
            ZF_LOGE("Error: could not read from elf file\n");
            if (!use_cpio) close_file_by_fd(args->pid, ELF_FILE_FD, true);
            return false;
        }
        /* Ensure that the file is an elf file. */
        if (elf_newFile(buf, (size_t)args->usr_buf, elf_file)) {
            ZF_LOGE("Invalid elf file\n");
            if (!use_cpio) close_file_by_fd(args->pid, ELF_FILE_FD, true);
            return false;
        }
    }
    
    return true;
}

int stack_write(seL4_Word *mapped_stack, int index, uintptr_t val)
{
    mapped_stack[index] = val;
    return index - 1;
}

/* set up System V ABI compliant stack, so that the process can
 * start up and initialise the C library */
uintptr_t init_process_stack(pt_meta_t *pt, cspace_t *cspace, seL4_CPtr local_vspace, elf_t *elf_file, user_proc *user_process)
{
    /* virtual addresses in the target process' address space */
    uintptr_t stack_top = PROCESS_STACK_TOP;
    uintptr_t stack_bottom = PROCESS_STACK_TOP - PAGE_SIZE_4K;
    /* virtual addresses in the SOS's address space */
    void *local_stack_top  = (seL4_Word *) SOS_SCRATCH;
    uintptr_t local_stack_bottom = SOS_SCRATCH - PAGE_SIZE_4K;

    /* find the vsyscall table */
    uintptr_t *sysinfo = (uintptr_t *) elf_getSectionNamed(elf_file, "__vsyscall", NULL);
    if (!sysinfo || !*sysinfo) {
        ZF_LOGE("could not find syscall table for c library");
        return 0;
    }

    /* Allocate and map stack into process adress space */
    ft_lock();
    if (alloc_map_frame(pt, user_process->vspace, &user_process->stack, stack_bottom, seL4_AllRights, seL4_ARM_Default_VMAttributes) != 0) {
        ft_unlock();
        ZF_LOGE("Failed to create and map stack frame");
        return false;
    }
    ft_unlock();

    /* allocate a slot to duplicate the stack frame cap so we can map it into our address space */
    seL4_CPtr local_stack_cptr = cspace_alloc_slot(cspace);
    if (local_stack_cptr == seL4_CapNull) {
        ZF_LOGE("Failed to alloc slot for stack");
        return 0;
    }

    /* copy the stack frame cap into the slot */
    seL4_Error err = cspace_copy(cspace, local_stack_cptr, cspace, user_process->stack, seL4_AllRights);
    if (err != seL4_NoError) {
        cspace_free_slot(cspace, local_stack_cptr);
        ZF_LOGE("Failed to copy cap");
        return 0;
    }

    /* map it into the sos address space */
    err = map_frame(cspace, local_stack_cptr, local_vspace, local_stack_bottom, seL4_AllRights,
                    seL4_ARM_Default_VMAttributes);
    if (err != seL4_NoError) {
        cspace_delete(cspace, local_stack_cptr);
        cspace_free_slot(cspace, local_stack_cptr);
        return 0;
    }

    int index = -2;

    /* null terminate the aux vectors */
    index = stack_write(local_stack_top, index, 0);
    index = stack_write(local_stack_top, index, 0);

    /* write the aux vectors */
    index = stack_write(local_stack_top, index, PAGE_SIZE_4K);
    index = stack_write(local_stack_top, index, AT_PAGESZ);

    index = stack_write(local_stack_top, index, *sysinfo);
    if (elf_file->read) free(sysinfo);

    index = stack_write(local_stack_top, index, AT_SYSINFO);

    index = stack_write(local_stack_top, index, PROCESS_IPC_BUFFER);
    index = stack_write(local_stack_top, index, AT_SEL4_IPC_BUFFER_PTR);

    /* null terminate the environment pointers */
    index = stack_write(local_stack_top, index, 0);

    /* we don't have any env pointers - skip */

    /* null terminate the argument pointers */
    index = stack_write(local_stack_top, index, 0);

    /* no argpointers - skip */

    /* set argc to 0 */
    stack_write(local_stack_top, index, 0);

    /* adjust the initial stack top */
    stack_top += (index * sizeof(seL4_Word));

    /* the stack *must* remain aligned to a double word boundary,
     * as GCC assumes this, and horrible bugs occur if this is wrong */
    assert(index % 2 == 0);
    assert(stack_top % (sizeof(seL4_Word) * 2) == 0);

    /* unmap our copy of the stack */
    err = seL4_ARM_Page_Unmap(local_stack_cptr);
    assert(err == seL4_NoError);

    /* delete the copy of the stack frame cap */
    err = cspace_delete(cspace, local_stack_cptr);
    assert(err == seL4_NoError);

    /* mark the slot as free */
    cspace_free_slot(cspace, local_stack_cptr);

    /* Exend the stack with extra pages */
    for (int page = 0; page < INITIAL_PROCESS_EXTRA_STACK_PAGES; page++) {
        stack_bottom -= PAGE_SIZE_4K;

        seL4_CPtr frame_cptr;
        ft_lock();
        if (alloc_map_frame(pt, user_process->vspace, &frame_cptr, stack_bottom, seL4_AllRights, seL4_ARM_Default_VMAttributes) != 0) {
            ft_unlock();
            ZF_LOGE("Failed to create and map additional stack frame");
            return false;
        }
        ft_unlock();
    }

    return stack_top;
}

void free_elf_file(elf_t *elf_file) {
    if (elf_file != NULL && elf_file->elfFile != NULL) {
        free(elf_file->elfFile);
    }
}

bool start_process(nfs_args_t *args, seL4_CPtr ep, bool use_cpio) {
    proc_t_lock();
    user_proc *user_process = &processes[args->pid].proc;
    proc_t_unlock();
    init_user_process(user_process);
    proc_t_lock();
    processes[args->pid].pt = NULL;
    processes[args->pid].fd_list = NULL;
    processes[args->pid].name = NULL;
    proc_t_unlock();

    /* Create a VSpace */
    user_process->vspace_ut = alloc_retype(&user_process->vspace, seL4_ARM_PageGlobalDirectoryObject,
                                              seL4_PGDBits);
    if (user_process->vspace_ut == NULL) {
        release_user_process(user_process, args->pid);
        return false;
    }
    
    /* assign the vspace to an asid pool */
    seL4_Word err = seL4_ARM_ASIDPool_Assign(seL4_CapInitThreadASIDPool, user_process->vspace);
    if (err != seL4_NoError) {
        ZF_LOGE("Failed to assign asid pool\n");
        release_user_process(user_process, args->pid);
        return false;
    }

    // Initialise fd list for process.
    fd_list_t *fdl = fd_list_init();
    if (fdl == NULL) {
        ZF_LOGE("Failed to initialise fd list for process\n");
        release_user_process(user_process, args->pid);
        return false;
    }
    proc_t_lock();
    processes[args->pid].fd_list = fdl;
    proc_t_unlock();
    add_local_fd(args->pid, NULL, true, O_RDONLY);
    add_local_fd(args->pid, NULL, true, O_WRONLY);
    add_local_fd(args->pid, NULL, true, O_WRONLY);

    ZF_LOGI("\nStarting \"%s\"...\n", args->name);
    elf_t elf_file = {};
    if (!init_elf_file(args, use_cpio, &elf_file)) {
        release_user_process(user_process, args->pid);
        return false;
    }
    
    /* initialise page table (non-hardware) */
    pt_meta_t *pt = pt_init(args->pid, &user_process->vspace, user_process->vspace_ut, &elf_file);
    if (pt == NULL) {
        ZF_LOGE("Failed to create process table\n");
        release_user_process(user_process, args->pid);
        if (!use_cpio) free_elf_file(&elf_file);
        return false;
    }

    proc_t_lock();
    processes[args->pid].pt = pt;
    proc_t_unlock();

    /* Create a simple 1 level CSpace */
    err = cspace_create_one_level(&cspace, &user_process->cspace);
    if (err != CSPACE_NOERROR) {
        ZF_LOGE("Failed to create cspace\n");
        release_user_process(user_process, args->pid);
        if (!use_cpio) free_elf_file(&elf_file);
        return false;
    }
    user_process->has_cspace = true;

    /* Create, map and pin the IPC buffer */
    ft_lock();
    if (alloc_map_frame(pt, user_process->vspace, &user_process->ipc_buffer, PROCESS_IPC_BUFFER, seL4_AllRights, seL4_ARM_Default_VMAttributes) != 0) {
        ft_unlock();
        ZF_LOGE("Failed to create and map ipc_buffer\n");
        release_user_process(user_process, args->pid);
        if (!use_cpio) free_elf_file(&elf_file);
        return false;
    }
    ft_unlock();
    ZF_LOGD("PINNING IPC BUFFER %p\n", PROCESS_IPC_BUFFER);
    vm_fault_logic(args->pid, PROCESS_IPC_BUFFER, true);

    /* allocate a new slot in the target cspace which we will mint a badged endpoint cap into --
     * the badge is used to identify the process, which will come in handy when you have multiple
     * processes. */
    user_process->user_ep = cspace_alloc_slot(&user_process->cspace);
    if (user_process->user_ep == seL4_CapNull) {
        ZF_LOGE("Failed to alloc user ep slot\n");
        release_user_process(user_process, args->pid);
        if (!use_cpio) free_elf_file(&elf_file);
        return false;
    }

    /* now mutate the cap, thereby setting the badge */
    err = cspace_mint(&user_process->cspace, user_process->user_ep, &cspace, ep, seL4_AllRights, args->pid);
    if (err) {
        ZF_LOGE("Failed to mint user ep\n");
        release_user_process(user_process, args->pid);
        if (!use_cpio) free_elf_file(&elf_file);
        return false;
    }

    /* Create a new TCB object */
    user_process->tcb_ut = alloc_retype(&user_process->tcb, seL4_TCBObject, seL4_TCBBits);
    if (user_process->tcb_ut == NULL) {
        ZF_LOGE("Failed to alloc tcb ut\n");
        release_user_process(user_process, args->pid);
        if (!use_cpio) free_elf_file(&elf_file);
        return false;
    }

    /* Configure the TCB */
    err = seL4_TCB_Configure(user_process->tcb,
                             user_process->cspace.root_cnode, seL4_NilData,
                             user_process->vspace, seL4_NilData, PROCESS_IPC_BUFFER,
                             user_process->ipc_buffer);
    if (err != seL4_NoError) {
        ZF_LOGE("Unable to configure new TCB\n");
        release_user_process(user_process, args->pid);
        if (!use_cpio) free_elf_file(&elf_file);
        return false;
    }

    /* Create scheduling context */
    user_process->sched_context_ut = alloc_retype(&user_process->sched_context, seL4_SchedContextObject,
                                                     seL4_MinSchedContextBits);
    if (user_process->sched_context_ut == NULL) {
        ZF_LOGE("Failed to alloc sched context ut\n");
        release_user_process(user_process, args->pid);
        if (!use_cpio) free_elf_file(&elf_file);
        return false;
    }

    /* Configure the scheduling context to use the first core with budget equal to period */
    err = seL4_SchedControl_Configure(sched_ctrl_start, user_process->sched_context, US_IN_MS, US_IN_MS, 0, 0);
    if (err != seL4_NoError) {
        ZF_LOGE("Unable to configure scheduling context\n");
        release_user_process(user_process, args->pid);
        if (!use_cpio) free_elf_file(&elf_file);
        return false;
    }

    /* bind sched context, set fault endpoint and priority
     * In MCS, fault end point needed here should be in current thread's cspace.
     * NOTE this will use the unbadged ep unlike above, you might want to mint it with a badge
     * so you can identify which thread faulted in your fault handler */

    user_process->proc_ep = cspace_alloc_slot(&cspace);
    if (user_process->proc_ep == seL4_CapNull) {
        ZF_LOGE("Failed to alloc user ep slot\n");
        release_user_process(user_process, args->pid);
        if (!use_cpio) free_elf_file(&elf_file);
        return false;
    }
    
    err = cspace_mint(&cspace, user_process->proc_ep, &cspace, ep, seL4_AllRights, args->pid);
    if (err) {
        ZF_LOGE("Failed to mint user ep\n");
        release_user_process(user_process, args->pid);
        if (!use_cpio) free_elf_file(&elf_file);
        return false;
    }

    err = seL4_TCB_SetSchedParams(user_process->tcb, seL4_CapInitThreadTCB, seL4_MinPrio, PROC_PRIORITY,
                                  user_process->sched_context, user_process->proc_ep);
    if (err != seL4_NoError) {
        ZF_LOGE("Unable to set scheduling params\n");
        release_user_process(user_process, args->pid);
        if (!use_cpio) free_elf_file(&elf_file);
        return false;
    }

    /* Provide a name for the thread -- Helpful for debugging */
    NAME_THREAD(user_process->tcb, args->name);

    /* set up the stack */
    seL4_Word sp = init_process_stack(pt, &cspace, seL4_CapInitThreadVSpace, &elf_file, user_process);
    if (!sp) {
        ZF_LOGE("Failed to initialise stack for new process\n");
        release_user_process(user_process, args->pid);
        if (!use_cpio) free_elf_file(&elf_file);
        return false;
    }

    /* load the elf from file */
    err = elf_load(pt, user_process->vspace, &elf_file);
    if (err) {
        ZF_LOGE("Failed to load elf image\n");
        release_user_process(user_process, args->pid);
        if (!use_cpio) free_elf_file(&elf_file);
        return false;
    }

    /* Start the new process */
    seL4_UserContext context = {
        .pc = elf_getEntryPoint(&elf_file),
        .sp = sp,
    };

    // Free the entire elf file that was read into memory
    if (!use_cpio) close_file_by_fd(args->pid, ELF_FILE_FD, true);
    if (!use_cpio) free_elf_file(&elf_file);

    proc_t_lock();
    processes[args->pid].is_waiting = -1;
    processes[args->pid].num_waiting = 0;
    processes[args->pid].boot = get_time() / 1000;
    processes[args->pid].flags = FLAG_RUNNING;
    processes[args->pid].name = malloc(MIN( (size_t)N_NAME, args->pathlen) );
    memcpy(processes[args->pid].name, args->name, MIN((size_t)N_NAME, args->pathlen));
    if (new_notification(&processes[args->pid].syscall_ntfn, &processes[args->pid].syscall_ntfn_ut, true)) {
        ZF_LOGE("Failed to create ntfn\n");
        release_user_process(user_process, args->pid);
        proc_t_unlock();
        return false;
    }
    if (new_notification(&processes[args->pid].waiting_ntfn, &processes[args->pid].waiting_ntfn_ut, false)) {
        ZF_LOGE("Failed to create ntfn\n");
        release_user_process(user_process, args->pid);
        proc_t_unlock();
        return false;
    }
    
    ZF_LOGI("Starting process %d %s at pc %p\n", args->pid, args->name, (void *) context.pc);
    err = seL4_TCB_WriteRegisters(user_process->tcb, 1, 0, 2, &context);
    if (err) {
        ZF_LOGE("Failed to write registers\n");
        release_user_process(user_process, args->pid);
        proc_t_unlock();
        return false;
    }
    proc_t_unlock();
    
    args->result = args->pid;
    ZF_LOGD("RESULT | PID: %d | %d\n", args->result, args->pid);

    return err == seL4_NoError;
}

void start_first_process(void *data) {
    (void)data;
    nfs_args_t args = {};
    args.name = APP_NAME;
    args.pathlen = strlen(APP_NAME);
    args.pid = get_free_idx();

    // Assumes first process always exists and is executable
    nfs_args_t* stat = new_os_nfs_stat_args(&args);
    ZF_LOGD("Calling stat in start first process\n");
    fs_ops_caller(stat);
    sos_stat_t *stats = (sos_stat_t *)stat->extra_buf;

    args.usr_buf = stats->st_size;
    free_os_nfs_stat_args(stat);

    if (!start_process(&args, ipc_ep, true)) {
        ZF_LOGF("Error: could not start first process\n");
    }
    os_exit();
}