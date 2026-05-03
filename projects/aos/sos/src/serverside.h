#pragma once

#include <cspace/cspace.h>
#include "ut.h"
#include <networkconsole/networkconsole.h>
#include <stdbool.h>
#include "vmem.h"

#define READ_MAX_BUF 4096

// ===================================
// ==== IMPLEMENTED IN SERVERSIDE ====
// ===================================
// ==== Argument Passing ====
typedef void *syscall_new_args_t(void *);
typedef void syscall_free_args_t(void *);
typedef void syscall_thread_t(void *);

// Data passed to syscall thread functions.
typedef struct pass_data {
    int pid;            // Process id of process calling syscall.
    seL4_CPtr reply;    // Copy of reply cap from initial syscall.
    ut_t *reply_ut;
    void *data;         // Typically contains syscall argument struct.
    syscall_free_args_t *free_args_func; // Function pointer to free function used to free syscall argument struct in data.
} pass_data_t;


// ==== Syscall Framework ====
// Free's pass data, cleaning up the reply cap and calling free_args_func on the pass_data_t data field.
void free_pass_data(pass_data_t *data);

/**
 * Send back a result to an initial system call. To be used within
 * a system call handling thread. Additionally handles the freeing
 * of pass_data.
 * 
 * @param 'pd' pass_data_t that was given to system call thread by syscall_thread_spawner.
 * @param 'status' status to return to syscall caller.
 * @param 'result' result value to return to syscall caller.
 */
void send_result(pass_data_t *pd, int status, seL4_Word result);

/**
 * Spawns a thread to handle a system call. Creates a pass_data_t struct
 * to give to a thread function for the system call.
 * 
 * @param 'pid' pid of process making syscall.
 * @param 'new_args_func' function to create arguments struct that is 
 * then stored in pass_data_t data field.
 * @param 'free_args_func' function to free arguments struct.
 * @param 'thread_func' function for thread to run to handle syscall.
 * Will be given the new pass_data_t struct when called.
 * @param 'syscall_name' name of system call for error output.
 * @param 'reply' reply capability to copy so that thread can reply
 * directly to initial system call.
 * @param 'reply_ut' ut memory of reply cap so that thread can properly
 * clean up reply object copy.
 * @param 'data' data that is passed into the new_args_func, typically
 * system call arguments from message registers of initial call, in the
 * form of a static array of seL4_Words.
 * @param 'have_reply' set to false once a thread has successfully
 * started, as it is then the threads responsibility to reply to the
 * system call.
 * @returns seL4_MessageInfo_t, only used if the spawning of a system call failed and the main syscall loop must be responsible for sending back a reply to the caller, in which case this message info will
 */
seL4_MessageInfo_t syscall_thread_spawner(int pid, syscall_new_args_t *new_args_func, syscall_free_args_t *free_args_func, syscall_thread_t *thread_func, char *syscall_name, seL4_CPtr *reply, ut_t **reply_ut, void *data, bool *have_reply);


// ==== Vaddr | PhysAddr Data Transfer ====
// Generic buffer struct used for process_to_buf and buf_to_process functions.
typedef struct buf_data {
    char *buf;
    size_t bufsize;
    size_t index;
} buf_data_t;

// Callback function type to give frame loop to perform on each frame:
// - buf: physical address of frame
// - nbyte: number of bytes to read / write.
// - flag_end: set to indicate that frame loop should stop after this iteration.
// - data: misc data passed from frame_loop.
typedef int frame_loop_f(char *buf, size_t nbyte, bool *flag_end, void *data);

/**
 * Given a buffer vaddr and number of bytes, translate the buffer into
 * a series of physical frames and run a callback function on each, 
 * pinning each frame into memory for the duration of its use.
 * 
 * @param 'pid' process id of process with buffer.
 * @param 'buf' buffer vaddr.
 * @param 'nbyte' length of buffer to process.
 * @param 'callback' callback function to run on each frame.
 * @param 'data' misc data to be passed to the callback.
 * @returns bytes processed
 */
size_t frame_loop(int pid, char *buf, size_t nbyte, frame_loop_f *callback, void *data);

/**
 * Transfer data from a process buffer at vaddr vaddr of size v_nbyte
 * to an sos buffer buf of size b_nbyte.
 * 
 * @param 'pid' process id of process with vaddr buffer.
 * @param 'vaddr' process buffer vaddr.
 * @param 'v_nbyte' effective length of process buffer.
 * @param 'buf' sos buffer.
 * @param 'b_nbyte' effective length of sos buffer.
 * @returns bytes processed
 */
size_t process_to_buf(int pid, char *vaddr, size_t v_nbyte, char *buf, size_t b_nbyte);

// Same as process_to_buf, but instead transferring from the sos buffer
// to the process buffer.
size_t buf_to_process(int pid, char *vaddr, size_t v_nbyte, char *buf, size_t b_nbyte);

/**
 * Given a buffer vaddr and size, check that the buffer exists in
 * a valid memory range.
 * 
 * @param 'pid' process id of process with vaddr buffer.
 * @param 'buf' process buffer vaddr.
 * @param 'nbyte' effective length of process buffer.
 * @param 'write_to' if we are checking the buffer for reading or 
 * writing. Note: this currently is not used, as address space 
 * permissions are not currently managed.
 * @returns true for valid buffer, false otherwise
 */
bool buffer_range_check(int pid, char *buf, size_t nbyte, bool write_to);


// ==== Syscall Handlers ====
typedef struct sleep_args {
    uint64_t delay;
    seL4_CPtr ntfn;
    ut_t *ntfn_ut;
} sleep_args_t;

struct local_fd;
typedef struct local_fd local_fd_t;

seL4_MessageInfo_t handle_dummy();

void *new_nfs_open_args(void *data);
void handle_open_thread(void *data);

void *new_nfs_close_args(void *data);
void handle_close_thread(void *data);

void *new_nfs_read_args(void *data);
void handle_read_thread(void *data);

void *new_nfs_write_args(void *data);
void handle_write_thread(void *data);

void *new_nfs_dirent_args(void *data);
void handle_dirent_thread(void *data);

void *new_nfs_stat_args(void *data);
void handle_stat_thread(void *data);

void *new_proc_create_args(void *data);
void *new_proc_wait_args(void *data);
void *new_proc_del_args(void *data);
void *new_proc_stat_args(void *data);
void handle_proc_thread(void *data);

void *new_sleep_args(void *data);
void free_sleep_args(void *data);
void handle_sleep_thread(void *data);

seL4_MessageInfo_t handle_timestamp();

void handle_os_thread_exit();