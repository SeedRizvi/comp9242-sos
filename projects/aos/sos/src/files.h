#pragma once

#include "utils.h"
#include <stdbool.h>
#include <stdlib.h>
#include "ut.h"
#include <nfsc/libnfs.h>
#include "serverside.h"

// ==== LOCAL FD ====
typedef struct local_fd {
    int fd;
    struct nfsfh *handle;
    int flags;
    int devtype; // Device type, enumeration defined in filesystem.h.
    struct local_fd *next;
} local_fd_t;

// ==== FD LIST ====
typedef struct fd_list {
    local_fd_t *nodes;
} fd_list_t;

/**
 * Initialise new empty fd_list.
 *
 * @return empty fd_list.
 */
fd_list_t *fd_list_init(void);

/**
 * Empty and destroy fd_list. Closes all currently open file handles.
 *
 * @param 'fd_list' fd_list to destroy.
 */
void fd_list_destroy(fd_list_t *fd_list);

/**
 * Get the local_fd_t for and fd from a process' fd_list.
 *
 * @param 'pid' pid of process to fetch local_fd_t from.
 * @param 'fd' fd to fetch local_fd_t for.
 * @return local_fd_t * of fd if successfully found, NULL otherwise.
 */
local_fd_t *get_local_fd(int pid, int fd);

/**
 * Add a new local_fd_t to fd_list. Selects lowest available fd for
 * the new entry.
 *
 * @param 'pid' pid of process to add new fd to.
 * @param 'handle' nfs handle to add to new local_fd_t. Should be left as NULL for console devices.
 * @param 'devtype' device type of new fd.
 * @param 'flags' flags for fd.
 * @return 0 on success, -1 otherwise.
 */
int add_local_fd(int pid, struct nfsfh *handle, int devtype, int flags);

/**
 * Remove local_fd_t from process PIDs fd_list.
 *
 * @param 'pid' pid of process to remove local_fd_t from.
 * @param 'fd' fd to remove local_fd_t for.
 * @return 0 on success, 1 on failure or if entry not found.
 */
int remove_local_fd(int pid, int fd);