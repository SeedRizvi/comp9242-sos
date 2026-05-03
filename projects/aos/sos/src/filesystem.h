#pragma once

#include <stdbool.h>
#include <stdlib.h>
#include "ut.h"
// #include <nfsc/libnfs.h>
#include "serverside.h"
// #include "files.h"

#define CB_DATA_BUF_SIZE 5
#define CB_DATA_BYTE_SIZE 52
#define IGNORE_DOTDOT 1 /* To handle the stat(..) fail case => TODO: Fix this later */
#define DIRENT_OPEN_STR "."
#define PARENT_DIR_STR ".."
#define PAGEFILE_PATH "pagefile"

#define FILE_DEVTYPE 0
#define CONSOLE_DEVTYPE 1
#define PAGEFILE_DEVTYPE 2
#define CLOSE_NOREMOVE -1

enum NFS_OPS {
    FS_READ = 1,
    FS_WRITE,
    FS_DIRENT,
    FS_STAT,
    FS_OPEN,
    FS_CLOSE,
    FS_SEEK
};

struct nfs_context;
struct local_fd_t;

// NFS argument struct to pass to fs_ops_caller to perform appropriate
// nfs operations with. 
// NOTE: Intended use for arguments may be ignored
// and some specific fs operations may use arguments differently, 
// although in most cases the following is accurate.
// NOTE: Not all arguments are used by all operations.
typedef struct nfs_args {
    uint8_t operation;      // Operation to perform. NFS_OPS enum.
    char *name;             // File pathname.
    size_t pathlen;         // File pathname length.

    int pid;                // Pid operation is being performed on.

    int fd;                 // Fd of open file to perform operation on.
    struct nfsfh *handle;   // NFS handle of file to perform operation on. Generally populated with update_nfs_args.
    int flags;              // Flags of file fs operations is using.
    int devtype;            // Device type of file fs operations is using.
    local_fd_t *lfd;        // Local file descriptor struct associated with file fs operations is using on process {pid}s fd_list.
    
    uintptr_t usr_buf;      // Generally pointer to buffer for reading and writing from / to.
    void *extra_buf;        // Additional sos address space buffer.
    size_t nbyte;           // Number of bytes, e.g. for file reading and writing.
    int result;             // Result value of file operation after completion or failure.

    int pos;                // Misc positional value.

    seL4_CPtr ntfn;         // Notification cptr to wait on for file opeation completion.
    ut_t *ntfn_ut;          // Notification ut_t *, tracked for cleanup.
} nfs_args_t;


// Create and return a blank nfs args with initialised values. Returns NULL on failure.
nfs_args_t *new_nfs_blank_arg();

/**
 * Updates an nfs_args_t struct with the handle, flags, devtype and lfd
 * of a fd entry in a processes fd list based on the pid and fd fields
 * currently in the provided nfs_args_t.
 *
 * @param 'args' nfs_args_t to update.
 * @return 0 on success, 1 on failure (unable to retreive fd from process fd_list).
 */
int update_nfs_args(nfs_args_t *args);

// Frees provided nfs_args_t *(data). Conditionally frees allocated memory stored in args.
void free_nfs_args(void *data);

// Check if provided fd is a valid fd value.
bool is_valid_fd(int fd);

// Check if flags field of provided nfs_args_t indicates a read permission.
bool has_read_perm(nfs_args_t *args);

// Check if flags field of provided nfs_args_t indicates a write permission.
bool has_write_perm(nfs_args_t *args);

// Check if provided string is the same as the pagefile name.
bool is_pagefile(char *str);

// THREAD FUNCTION! Initialise the pagefile. ZF_LOGF runs on failure.
void fs_init_pagefile(void *data);

// Truncate the pagefile to length len. Blocks until NFS operation completion.
int pf_truncate(uint64_t len);

/**
 * Write to the pagefile. Blocks until NFS operation completion.
 *
 * @param 'offset' offset at which to start writing.
 * @param 'src' sos address space buffer with source data.
 * @param 'nbyte' bytes to write to file.
 * @return 0 on success, -1 on failure.
 */
int fs_write_pagefile(int64_t offset, char *src, size_t nbyte);

/**
 * Read from the pagefile. Blocks until NFS operation completion.
 *
 * @param 'offset' offset at which to start reading.
 * @param 'dst' sos address space buffer to read into.
 * @param 'nbyte' bytes to read from file.
 * @return 0 on success, -1 on failure.
 */
int fs_read_pagefile(int64_t offset, char *dst, size_t nbyte);

// Initial filesystem setup function.
void fs_global_init();

// Sets nfs_context to use for all fs operations to provided nfs_context.
void fs_init(struct nfs_context *from_cb);
void fs_destroy();

/**
 * Perform an NFS operation using provided args. Blocks until NFS operation completion.
 *
 * @param 'args' nfs_args_t to use fs operation. args->operation determines which operation is called. See NFS_OPS enum.
 */
void fs_ops_caller(nfs_args_t *args);

/**
 * Callback to run after nfs operation launched through fs_ops_caller completes. Signals the nfs_args_t notification on completion to unblock fs_ops_caller.
 *
 * @param 'err' error code of nfs operation calling callback.
 * @param 'nfs' nfs context nfs operation used.
 * @param 'data' data passed by completed nfs operation, as specified in libnfs.
 * @param 'priv_data' data provided by us when launching NFS operation. This will be the nfs_args_t used for the operation.
 */
void fs_ops_cb(int err, UNUSED struct nfs_context *nfs, void *data, void *priv_data);

// Check if nfs operation was queued successfully. If not, update args->result to -1.
void is_queued(int status, nfs_args_t *args);

// ==== FS OP HANDLERS ====
// Launchers are called by fs_ops_caller to launch apporpriate nfs_operation with args.
// Callbacks are called by fs_ops_cb, where p_data is the nfs_args_t that was initially provided and data is dependent on the nfs operation.
// FS operations such as read and write do not handle the console device case. That is handled in serverside.c.

// Uses: devtype (device type to open), flags (flags to open with), name (file path to open (FILE_DEVTYPE case only)), pid (process to open file on)
void nfs_open_launcher(nfs_args_t *args);
void fs_open_callback(nfs_args_t *p_data, void *data);

// Uses: devtype (device type of FD to close), handle (handle to close, NULL in CONSOLE_DEVTYPE case), pid (process to close on), fd (fd to close)
void nfs_close_launcher(nfs_args_t *args);

// Uses: handle (file handle), nbyte (bytes to read), devtype (device type of handle (file or pagefile)), usr_buf (in pagefile case, buffer to read into)
void nfs_read_launcher(nfs_args_t *args);

// In case of FILE_DEVTYPE, data will be copied into nfs_args_t extra_buf, which will be malloced in the callback.
void fs_read_callback(int err, nfs_args_t *p_data, void *data);

// Uses: handle (file handle), nbyte (bytes to write), usr_buf (buffer to read from)
void nfs_write_launcher(nfs_args_t *args);

void fs_dirent_callback(nfs_args_t *p_data, void *data);

// Uses: name (file name), extra_buf (sos_stat_t object to copy to)
void nfs_stat_launcher(nfs_args_t *args);
void fs_stat_callback(nfs_args_t *p_data, void *data);

// Uses: handle (file handle), nbyte (seek offset).
void nfs_seek_launcher(nfs_args_t *args);

// ==== MISC PUBLIC UTILITY FUNCTIONS ====
/**
 * Read from file represented by fd in process pids fd_list. Used extensively in elf loading from NFS.
 *
 * @param 'pid' process of fd_list to use.
 * @param 'fd' fd on fd_list to read from.
 * @param 'offset' file offset to start read from.
 * @param 'nbyte' bytes to read.
 * @return pointer to buffer with read data on success. NULL on failure.
 */
void *read_fd_to_buf(int pid, int fd, size_t offset, size_t nbyte);

/**
 * Close a file handle of fd fd from process pids fd_list.
 *
 * @param 'pid' process of fd_list to use.
 * @param 'fd' fd on fd_list to close.
 * @param 'ext' whether or not this is being called externally or from within an operation that will handle cleaning up the fd_list entry. If set to true, the associated entry of the fd_list will also be removed by this function.
 */
void close_file_by_fd(int pid, int fd, bool ext);

/**
 * Close a file handle directly by fd_list entry.
 *
 * @param 'lfd' local_fd_t associated with handle to close.
 */
void close_file_by_handle(local_fd_t *lfd);