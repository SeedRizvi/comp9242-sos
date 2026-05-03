#include "serverside.h"

#include <clock/clock.h>
#include <fcntl.h>

#include "utils.h"
#include "threads.h"
#include "netcon.h"
#include "vmem.h"
#include "vmem_layout.h"
#include "syscall_codes.h"
#include "network.h"
#include "pagefile.h"
#include "filesystem.h"
#include "files.h"
#include "process.h"
#include "../../libsosapi/include/sos.h"


// ===========================
// ==== Syscall Framework ====
// ===========================
void free_pass_data(pass_data_t *data) {
    remove_reply(data->reply, data->reply_ut);
    data->free_args_func(data->data);
    free(data);
}

void send_result_not_done(pass_data_t *pd, int status, seL4_Word result) {
    ZF_LOGD("Sending thread result\n");
    seL4_SetMR(0, status);
    seL4_SetMR(1, result);
    seL4_NBSend(pd->reply, seL4_MessageInfo_new(0, 0, 0, 2));
    free_pass_data(pd);
}

void send_result(pass_data_t *pd, int status, seL4_Word result) {
    ZF_LOGD("Sending thread result\n");
    seL4_SetMR(0, status);
    seL4_SetMR(1, result);
    seL4_NBSend(pd->reply, seL4_MessageInfo_new(0, 0, 0, 2));
    proc_syscall_done(pd->pid);
    free_pass_data(pd);
}

// Handles errors that occur before a syscall thread is started.
// To properly handle errors within a syscall thread, use send_result
// with an apporpriate status followed by os_exit.
seL4_MessageInfo_t handler_err(char *err_str, char *syscall_name) {
    ZF_LOGE("%s %s\n", err_str, syscall_name);
    seL4_SetMR(0, 1);
    return seL4_MessageInfo_new(0, 0, 0, 1);
}

seL4_MessageInfo_t syscall_thread_spawner(int pid, syscall_new_args_t *new_args_func, syscall_free_args_t *free_args_func, syscall_thread_t *thread_func, char *syscall_name, seL4_CPtr *reply, ut_t **reply_ut, void *data, bool *have_reply) {

    // Get arguments.
    void *args = new_args_func(data);
    if (args == NULL) {
        return handler_err("Error: failed to create arguments for syscall ", syscall_name);
    }

    // Set up pass data and reply
    ZF_LOGD("Creating pass_data\n");
    pass_data_t *pd = malloc(sizeof(pass_data_t));
    if (pd == NULL) {
        free_args_func(args);
        return handler_err("Error: failed to create pass_data for syscall ", syscall_name);
    }
    if (copy_reply(*reply, *reply_ut, &pd->reply, &pd->reply_ut)) {
        free_args_func(args);
        free(pd);
        return handler_err("Error: failed to create reply copy for syscall ", syscall_name);
    }
    pd->pid = pid;
    pd->data = args;
    pd->free_args_func = free_args_func;

    // Spawn thread.
    ZF_LOGD("Spawning syscall thread\n");
    proc_syscall_start(pd->pid);
    sos_thread_t *t = spawn(thread_func, pd, SOS_THREAD_BADGE, false);
    if (t == NULL) {
        proc_syscall_done(pd->pid);
        cspace_delete(&cspace, pd->reply);
        cspace_free_slot(&cspace, pd->reply);
        free_pass_data(pd);
        return handler_err("failed to spawn thread for ", syscall_name);
    }

    // Reset original reply and return.
    *reply_ut = NULL;
    remove_reply(*reply, *reply_ut);
    new_reply(reply, reply_ut);
    *have_reply = false;
    
    return seL4_MessageInfo_new(0, 0, 0, 0);
}


// ========================================
// ==== Vaddr | PhysAddr Data Transfer ====
// ========================================
size_t frame_loop(int pid, char *buf, size_t nbyte, frame_loop_f *callback, void *data) {
    size_t bytes_written = 0;
    bool flag_end = false;

    while (bytes_written < nbyte) {
        
        frame_ref_t frame = vm_fault_logic(pid, (uintptr_t)buf, true);

        if (frame == INVALID_FRAME) {
            ZF_LOGE("Error: could not pin IO frame\n");
            return bytes_written;
        }

        char *phys_addr = (char *)frame_data(frame);
        size_t frame_offset = (uintptr_t)buf - ROUND_DOWN((uintptr_t)buf, PAGE_SIZE_4K);
        size_t frame_bytes = PAGE_SIZE_4K - frame_offset;

        size_t temp_bytes = 0;
        if (nbyte - bytes_written <= frame_bytes) {
            temp_bytes = nbyte - bytes_written;
        } else {
            temp_bytes = frame_bytes;
        }
        int temp_bytes_op = callback((char *)phys_addr + frame_offset, temp_bytes, &flag_end, data);
        if (temp_bytes_op < 0) {
            vm_unpin(pid, (uintptr_t)buf);
            return bytes_written;
        }

        vm_unpin(pid, (uintptr_t)buf);
        bytes_written += temp_bytes_op;
        buf += frame_bytes;

        if (flag_end) {   
            return bytes_written;
        }
    }
    return bytes_written;
}

buf_data_t *new_buf_data(char *buf, size_t bufsize) {
    buf_data_t *ret = malloc(sizeof(buf_data_t));
    ret->buf = buf;
    ret->bufsize = bufsize;
    ret->index = 0;
    return ret;
}

void free_buf_data(buf_data_t *buf_data) {
    free(buf_data);
}

// Callback for buffer writing to be used by frame loop in process_to_buf. See frame_loop_f function type in serverside.h for more details.
int buf_write_callback(char *buf, size_t nbyte, bool *flag_end, void *data) {
    buf_data_t *buf_data = (buf_data_t *)data;
    size_t i = 0;
    for (; i < nbyte; i++) {
        if (buf_data->index >= buf_data->bufsize) {
            *flag_end = true;
            return i;
        }
        (buf_data->buf)[buf_data->index] = buf[i];
        buf_data->index++;
    }
    return i;
}

// Callback for buffer reading to be used by frame loop in buf_to_process. See frame_loop_f function type in serverside.h for more details.
int buf_read_callback(char *buf, size_t nbyte, bool *flag_end, void *data) {
    buf_data_t *buf_data = (buf_data_t *)data;
    size_t i = 0;
    for (; i < nbyte; i++) {
        if (buf_data->index >= buf_data->bufsize) {
            *flag_end = true;
            return i;
        }
        buf[i] = (buf_data->buf)[buf_data->index];
        buf_data->index++;
    }
    return i;
}

size_t process_to_buf(int pid, char *vaddr, size_t v_nbyte, char *buf, size_t b_nbyte) {
    ZF_LOGD("PROCESS TO BUF TRANSFER: %p %u %p %u\n", vaddr, v_nbyte, buf, b_nbyte);
    buf_data_t *buf_data = new_buf_data(buf, b_nbyte);
    size_t bytes_read = frame_loop(pid, vaddr, v_nbyte, &buf_write_callback, buf_data);
    free_buf_data(buf_data);
    return bytes_read;
}

size_t buf_to_process(int pid, char *vaddr, size_t v_nbyte, char *buf, size_t b_nbyte) {
    ZF_LOGD("BUF TO PROCESS TRANSFER: %p %u %p %u\n", vaddr, v_nbyte, buf, b_nbyte);
    buf_data_t *buf_data = new_buf_data(buf, b_nbyte);
    size_t bytes_read = frame_loop(pid, vaddr, v_nbyte, &buf_read_callback, buf_data);
    free_buf_data(buf_data);
    return bytes_read;
}

bool buffer_range_check(int pid, char *buf, size_t nbyte, bool write_to) {
    bool res = false;
    if (write_to) { // No memory permissions right now, do the same thing.
        res = (valid_mem_range_from(pid, (uintptr_t)buf) && valid_mem_range_from(pid, (uintptr_t)((char *)buf + nbyte)));
    } else {
        res = (valid_mem_range_from(pid, (uintptr_t)buf) && valid_mem_range_from(pid, (uintptr_t)((char *)buf + nbyte))); 
    }
    if (!res) {
        ZF_LOGE("Error: buffer range not valid");
    }
    return res;
}


// ==========================
// ==== Private Helpers  ====
// ==========================
void update_nfs_args_pd(pass_data_t *pd) {
    if (update_nfs_args((nfs_args_t *)(pd->data))) {
        send_result(pd, 1, -1);
        os_exit();
    }
}

void enforce_read_perm(pass_data_t *pd) {
    if (!has_read_perm((nfs_args_t *)(pd->data))) {
        send_result(pd, 1, -1);
        os_exit();
    }
}

void enforce_write_perm(pass_data_t *pd) {
    if (!has_write_perm((nfs_args_t *)(pd->data))) {
        send_result(pd, 1, -1);
        os_exit();
    }
}

void load_name(pass_data_t *pd) {
    nfs_args_t *args = (pd->data);
    char *path = args->name;
    args->name = malloc(args->pathlen);
    if (args->name == NULL) {
        ZF_LOGE("Error: could not allocate name for stat\n");
        send_result(pd, 1, -1);
        os_exit();
        return;
    }
    process_to_buf(args->pid, path, args->pathlen, args->name, args->pathlen);
}


// =======================
// ==== Dummy Handler ====
// =======================
seL4_MessageInfo_t handle_dummy() {
    ZF_LOGD("syscall: thread example made syscall [dummy]!\n");
    seL4_SetMR(0, 0);
    return seL4_MessageInfo_new(0, 0, 0, 1);
}


// ======================
// ==== Open Handler ====
// ======================
void *new_nfs_open_args(void *data) {
    seL4_Word *tdata = (data);
    int pid = (int)tdata[0];
    char *path = (char *)tdata[1];
    size_t pathlen = tdata[2];
    int flags = tdata[3];
    int devtype = tdata[4];

    nfs_args_t *ret = new_nfs_blank_arg();
    if (ret == NULL) return NULL;

    ret->operation = FS_OPEN;
    ret->pathlen = pathlen;
    ret->flags = flags;
    ret->devtype = devtype;
    ret->pid = pid;
    ret->name = path;

    return ret;
}

void handle_open_thread(void *data) {
    ZF_LOGD("Starting open thread\n");
    pass_data_t *pd = (data);
    nfs_args_t *open_args = (pd->data);

    if (!buffer_range_check(open_args->pid, open_args->name, open_args->pathlen, false)) {
        send_result(pd, 1, -1);
        os_exit();
        return;
    }
    load_name(pd);

    if (is_pagefile(open_args->name)) {
        ZF_LOGE("Error: cannot open pagefile\n");
        send_result(pd, 1, -1);
        os_exit();
        return;
    }

    fs_ops_caller(open_args);

    send_result(pd, 0, (seL4_Word)(open_args->result));
    os_exit();
}


// =======================
// ==== Close Handler ====
// =======================
void *new_nfs_close_args(void *data) {
    seL4_Word *tdata = (data);
    int pid = (int)tdata[0];
    int fd = tdata[1];

    if (!is_valid_fd(fd)) return NULL;

    nfs_args_t *ret = new_nfs_blank_arg();
    if (ret == NULL) return NULL;

    ret->operation = FS_CLOSE;
    ret->fd = fd;
    ret->pid = pid;
    return ret;
}

void handle_close_thread(void *data) {
    ZF_LOGD("Starting close thread\n");
    pass_data_t *pd = (data);
    nfs_args_t *close_args = (pd->data);

    update_nfs_args_pd(pd);
    fs_ops_caller(close_args);

    send_result(pd, 0, (seL4_Word)(close_args->result));
    os_exit();
}


// ======================
// ==== Read Handler ====
// ======================
void *new_nfs_read_args(void *data) {
    seL4_Word *tdata = (data);
    int pid = (int)tdata[0];
    int fd = tdata[1];
    uintptr_t usr_buf = tdata[2];
    size_t nbyte = tdata[3];

    if (!is_valid_fd(fd)) return NULL;

    nfs_args_t *ret = new_nfs_blank_arg();
    if (ret == NULL) return NULL;

    ret->operation = FS_READ;
    ret->fd = fd;
    ret->usr_buf = usr_buf;
    ret->nbyte = nbyte;
    ret->pid = pid;
    return ret;
}

int term_read_callback(char *buf, size_t nbyte, bool *flag_end, void *data) {
    (void)data;
    int bytes_read = read_into_frame(buf, nbyte);
    if (buf[bytes_read - 1] == '\n') {
        *flag_end = true;
    }
    return bytes_read;
}

int file_read_loop(int pid, nfs_args_t *read_args, size_t *bytes_read) {
    // Store original nbyte and set amount to be read.
    size_t total_nbyte = read_args->nbyte;
    read_args->nbyte = (read_args->nbyte < READ_MAX_BUF) ? read_args->nbyte : READ_MAX_BUF;

    // Read in chunks of at most READ_MAX_BUF.
    while (*bytes_read < total_nbyte) {

        fs_ops_caller(read_args);
        if (read_args->result > 0) {
            *bytes_read += buf_to_process(pid, (char *)(read_args->usr_buf), read_args->nbyte, read_args->extra_buf, read_args->result);
            free(read_args->extra_buf);
            read_args->extra_buf = NULL;

        } else if (read_args->result < 0) {
            return read_args->result;
        } else {
            free(read_args->extra_buf);
            read_args->extra_buf = NULL;
            break;
        }

        // Update variables for next chunk.
        read_args->nbyte = (total_nbyte - *bytes_read < READ_MAX_BUF) ? read_args->nbyte - *bytes_read : READ_MAX_BUF;
        read_args->usr_buf = (uintptr_t)(((char *)read_args->usr_buf) + read_args->result);
    }
    return 0;
}

void handle_read_thread(void *data) {
    ZF_LOGD("Starting read thread\n");
    pass_data_t *pd = (data);
    nfs_args_t *read_args = (pd->data);

    if (!buffer_range_check(read_args->pid, read_args->usr_buf, read_args->nbyte, false)) {
        send_result(pd, 1, -1);
        os_exit();
        return;
    }

    update_nfs_args_pd(pd);
    enforce_read_perm(pd);
    
    size_t bytes_read = 0;

    // Console read case.
    if (read_args->devtype == 1) {

        term_read_lock();
        bytes_read = frame_loop(pd->pid, (char *)(read_args->usr_buf), read_args->nbyte, &term_read_callback, NULL);
        term_read_unlock();
    
    // File read case.
    } else if (read_args->devtype == 0) {
        file_read_loop(pd->pid, read_args, &bytes_read);
    }

    send_result(pd, 0, (seL4_Word)(bytes_read));
    os_exit();
}


// =======================
// ==== Write Handler ====
// =======================
void *new_nfs_write_args(void *data) {
    seL4_Word *tdata = (data);
    int pid = (int)tdata[0];
    int fd = tdata[1];
    uintptr_t usr_buf = tdata[2];
    size_t nbyte = tdata[3];

    if (!is_valid_fd(fd)) return NULL;

    nfs_args_t *ret = new_nfs_blank_arg();
    if (ret == NULL) return NULL;

    ret->operation = FS_WRITE;
    ret->fd = fd;
    ret->usr_buf = usr_buf;
    ret->nbyte = nbyte;
    ret->pid = pid;
    return ret;
}

int term_write_callback(char *buf, size_t nbyte, bool *flag_end, void *data) {
    (void)data;
    (void)flag_end;
    return netcon_write(buf, nbyte);
}

int nfs_write_callback(char *buf, size_t nbyte, bool *flag_end, void *data) {
    (void)flag_end;
    nfs_args_t *args = (data);
    args->usr_buf = (uintptr_t)buf;
    args->nbyte = nbyte;
    fs_ops_caller(args);
    return args->result;
}

void handle_write_thread(void *data) {
    ZF_LOGD("Starting write thread\n");
    pass_data_t *pd = (data);
    nfs_args_t *write_args = (pd->data);

    if (!buffer_range_check(write_args->pid, write_args->usr_buf, write_args->nbyte, true)) {
        send_result(pd, 1, -1);
        os_exit();
        return;
    }

    update_nfs_args_pd(pd);
    enforce_write_perm(pd);

    size_t bytes_written = 0;

    // Write to console case.
    if (write_args->devtype == 1) {
        bytes_written = frame_loop(pd->pid, (char *)(write_args->usr_buf), write_args->nbyte, &term_write_callback, NULL);

    // Write to file case.
    } else if (write_args->devtype == 0) {
        bytes_written = frame_loop(pd->pid, (char *)(write_args->usr_buf), write_args->nbyte, &nfs_write_callback, write_args);
    }

    send_result(pd, 0, (seL4_Word)(bytes_written));
    os_exit();
}


// ========================
// ==== Dirent Handler ====
// ========================
void *new_nfs_dirent_args(void *data) {
    seL4_Word *tdata = (data);
    int pid = (int)tdata[0];
    int pos = tdata[1];
    size_t nbyte = tdata[2];
    uintptr_t usr_buf = tdata[3];

    nfs_args_t *ret = new_nfs_blank_arg();
    if (ret == NULL) return NULL;

    ret->operation = FS_DIRENT;
    ret->pos = pos;
    ret->nbyte = nbyte;
    ret->usr_buf = usr_buf;
    ret->pid = pid;

    ret->extra_buf = malloc(sizeof(char) * ret->nbyte);
    if (ret->extra_buf == NULL) {
        ZF_LOGE("Error: could not allocate extra_buf for dirent\n");
        return NULL;
    }

    return ret;
}

void handle_dirent_thread(void *data) {
    ZF_LOGD("Starting dirent thread\n");
    pass_data_t *pd = (data);
    nfs_args_t *dirent_args = (pd->data);

    if (!buffer_range_check(dirent_args->pid, dirent_args->usr_buf, dirent_args->nbyte, false)) {
        send_result(pd, 1, -1);
        os_exit();
        return;
    }

    fs_ops_caller(dirent_args);
    if (dirent_args->result > 0) {
        buf_to_process(pd->pid, (char *)dirent_args->usr_buf, dirent_args->nbyte, dirent_args->extra_buf, dirent_args->result);
    }
    
    send_result(pd, 0, (seL4_Word)(dirent_args->result));
    os_exit();
}


// ======================
// ==== Stat Handler ====
// ======================
void *new_nfs_stat_args(void *data) {
    seL4_Word *tdata = (data);
    int pid = (int)tdata[0];
    char *path = (char *)tdata[1];
    size_t pathlen = tdata[2];
    uintptr_t usr_buf = tdata[3];

    nfs_args_t *ret = new_nfs_blank_arg();
    if (ret == NULL) return NULL;

    ret->pid = pid;
    ret->operation = FS_STAT;
    ret->pathlen = pathlen;
    ret->usr_buf = usr_buf;
    ret->name = path;

    ret->extra_buf = malloc(sizeof(sos_stat_t));
    if (ret->extra_buf == NULL) {
        ZF_LOGE("Error: could not allocate extra_buf for stat\n");
        return NULL;
    }
    return ret;
}

void handle_stat_thread(void *data) {
    ZF_LOGD("Starting stat thread\n");
    pass_data_t *pd = (data);
    nfs_args_t *stat_args = (pd->data);

    if (!buffer_range_check(stat_args->pid, stat_args->name, stat_args->pathlen, false)) {
        send_result(pd, 1, -1);
        os_exit();
        return;
    }
    load_name(pd);

    fs_ops_caller(stat_args);
    if (stat_args->result == 0) {
        buf_to_process(pd->pid, (char *)stat_args->usr_buf, sizeof(sos_stat_t), stat_args->extra_buf, sizeof(sos_stat_t));
    }
    
    send_result(pd, 0, (seL4_Word)(stat_args->result));
    os_exit();
}

// ======================
// ==== Proc Handler ====
// ======================
void *new_proc_create_args(void *data) {
    seL4_Word *tdata = (data);
    int pid = (int)tdata[0];
    char *path = (char *)tdata[1];
    size_t pathlen = tdata[2];

    nfs_args_t *ret = new_nfs_blank_arg();
    if (ret == NULL) return NULL;

    ret->pathlen = pathlen;
    ret->name = path;
    ret->pid = pid;
    ret->operation = P_CREATE;
    return ret;
}

void *new_proc_wait_args(void *data) {
    seL4_Word *tdata = (data);
    int pid = (int)tdata[0];
    size_t pos = tdata[1];

    nfs_args_t *ret = new_nfs_blank_arg();
    if (ret == NULL) return NULL;

    ret->pid = pid;
    ret->operation = P_WAIT;
    ret->pos = pos;
    return ret;
}

void *new_proc_del_args(void *data) {
    seL4_Word *tdata = (data);
    int pid = (int)tdata[0];
    size_t pos = tdata[1];

    nfs_args_t *ret = new_nfs_blank_arg();
    if (ret == NULL) return NULL;

    ret->pid = pid;
    ret->operation = P_DEL;
    ret->pos = pos;
    return ret;
}

void *new_proc_stat_args(void *data) {
    seL4_Word *tdata = (data);
    int pid = (int)tdata[0];
    uintptr_t proc_buf = (uintptr_t)tdata[1];
    size_t max = tdata[2];

    nfs_args_t *ret = new_nfs_blank_arg();
    if (ret == NULL) return NULL;

    ret->pid = pid;
    ret->operation = P_STAT;
    ret->usr_buf = proc_buf;
    ret->pos = max;
    ret->pathlen = sizeof(sos_process_t)*ret->pos;
    ret->extra_buf = malloc(ret->pathlen);
    return ret;
}

void handle_proc_thread(void *data) {
    ZF_LOGD("Starting proc thread\n");
    pass_data_t *pd = (data);
    nfs_args_t *proc_args = (pd->data);

    if (proc_args->operation == P_CREATE) {
        if (!buffer_range_check(proc_args->pid, proc_args->name, proc_args->pathlen, false)) {
            send_result(pd, 1, -1);
            os_exit();
            return;
        }
        load_name(pd);
    }

    if (proc_args->operation == P_DEL || proc_args->operation == P_WAIT) {
        proc_syscall_done(pd->pid);
    }
    
    // Spawns a thread responsible for starting new process
    proc_ops_caller(proc_args, pd);
    if (proc_args->operation == P_STAT && proc_args->result >= 0) {
        buf_to_process(proc_args->pid, (char *)proc_args->usr_buf, proc_args->pathlen, proc_args->extra_buf, proc_args->pathlen);
    }
    
    if (proc_args->operation != P_DEL && proc_args->operation != P_WAIT) {
        send_result(pd, 0, (seL4_Word)(proc_args->result));
        os_exit();
    } else if (proc_args->operation == P_WAIT) {
        send_result_not_done(pd, 0, (seL4_Word)(proc_args->result));
        os_exit();
    }
}


// =======================
// ==== Sleep Handler ====
// =======================
void *new_sleep_args(void *data) {
    seL4_Word *tdata = (data);
    sleep_args_t *ret = malloc(sizeof(sleep_args_t));
    ret->delay = tdata[1];
    if (new_notification(&ret->ntfn, (void *)&ret->ntfn_ut, false) != 0) {
        return NULL;
    }
    return ret;
}

void free_sleep_args(void *data) {
    sleep_args_t *sleep_args = (data);
    remove_notification(sleep_args->ntfn, sleep_args->ntfn_ut);
    free(sleep_args);
}

void sleep_callback(uint32_t id, void *data) {
    (void)id;
    seL4_CPtr ntfn = *((seL4_CPtr *)(data));
    seL4_Signal(ntfn);
}

void handle_sleep_thread(void *data) {
    ZF_LOGD("Starting sleep thread\n");
    pass_data_t *pd = (data);
    sleep_args_t *sleep_args = (pd->data);
    
    // Start timer to wait on.
    register_timer(sleep_args->delay, &sleep_callback, &sleep_args->ntfn);
    seL4_Wait(sleep_args->ntfn, NULL);

    ZF_LOGD("Sleep completes nomrally\n");
    send_result(pd, 0, 0);
    os_exit();
}


// ===========================
// ==== Timestamp Handler ====
// ===========================
seL4_MessageInfo_t handle_timestamp() {
    seL4_SetMR(0, (seL4_Word)get_time());
    return seL4_MessageInfo_new(0, 0, 0, 1);
}


// ======================
// ==== Exit Handler ====
// ======================
void handle_os_thread_exit() {
    sos_thread_t *f = (sos_thread_t *)seL4_GetMR(1);
    thread_suspend(f);
    os_thread_clear(f);
}