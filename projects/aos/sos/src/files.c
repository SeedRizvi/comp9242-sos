#include "files.h"

#include "utils.h"
#include <stdbool.h>
#include <stdlib.h>
#include "ut.h"
#include <nfsc/libnfs.h>
#include "serverside.h"
#include "network.h"
#include "process.h"

// ==================
// ==== LOCAL FD ====
// ==================
// Initialise and return new local_fd_t with provided parameters. Returns NULL on failure.
local_fd_t *local_fd_init(struct nfsfh *handle, int devtype, int flags) {
    local_fd_t *new_entry = malloc(sizeof(local_fd_t));
    if (new_entry == NULL) {
        return NULL;
    }
    new_entry->fd = 0;
    new_entry->handle = handle;
    new_entry->devtype = devtype;
    new_entry->next = NULL;
    new_entry->flags = flags;
    return new_entry;
}

/*
 * Free memory for a local_fd_t, also potentially closing the
 * associated file handle on the nfs.
 *
 * @param 'entry' local_fd_t to destroy.
 * @param 'ext' if run externally or from within fd_list_destroy.
 *              , in which case file handles will be closed.
 */
void local_fd_destroy(local_fd_t *entry, bool ext) {
    if (entry->devtype == FILE_DEVTYPE && !ext) {
        close_file_by_handle(entry);
    }
    free(entry);
}


// =================
// ==== FD LIST ====
// =================

fd_list_t *fd_list_init(void) {
    ZF_LOGI("Creating new fd list\n");
    wait_on_fs_mount();

    fd_list_t *new_list = malloc(sizeof(fd_list_t));
    if (new_list == NULL) {
        return NULL;
    } 
    new_list->nodes = NULL;
    return new_list;
}

void fd_list_destroy(fd_list_t *fd_list) {
    ZF_LOGI("Removing fd list\n");
    for (local_fd_t *curr = fd_list->nodes; curr != NULL;) {
        local_fd_t *tmp = curr->next;
        local_fd_destroy(curr, false);
        curr = tmp;
    }
    free(fd_list);
}

local_fd_t *get_local_fd(int pid, int fd) {
    ZF_LOGD("Fetching local FD\n");
    fd_list_t *fd_list = get_fd_list_from_pid(pid);

    for (local_fd_t *curr = fd_list->nodes; curr != NULL; curr = curr->next) {
        if (curr->fd == fd) {
            ZF_LOGD("Found matching FD\n");
            return curr;
        }
    }
    return NULL;
}

int add_local_fd(int pid, struct nfsfh *handle, int devtype, int flags) {
    ZF_LOGD("Adding local FD for pid: %d\n", pid);
    fd_list_t *fd_list = get_fd_list_from_pid(pid);

    local_fd_t *new_fd = local_fd_init(handle, devtype, flags);
    if (new_fd == NULL) {
        return -1;
    }
    
    if (fd_list->nodes == NULL) {
        fd_list->nodes = new_fd;
        return new_fd->fd;
    }
    
    if (fd_list->nodes->fd > new_fd->fd) {
        new_fd->next = fd_list->nodes;
        fd_list->nodes = new_fd;
        return new_fd->fd;
    }
    
    for (local_fd_t *curr = fd_list->nodes; curr != NULL; curr = curr->next) {
        if (curr->fd == new_fd->fd) {
            new_fd->fd++;
        }
        
        if (curr->next == NULL) {
            curr->next = new_fd;
            return new_fd->fd;
        }
        
        if (curr->next->fd > new_fd->fd) {
            new_fd->next = curr->next;
            curr->next = new_fd;
            return new_fd->fd;
        }
    }
    
    return -1;
}

int remove_local_fd(int pid, int fd) {
    ZF_LOGD("Removing local FD\n");
    fd_list_t *fd_list = get_fd_list_from_pid(pid);

    if (fd_list->nodes == NULL || fd <= 2) {
        return 1;
    }

    local_fd_t *prev = fd_list->nodes;
    for (local_fd_t *curr = prev->next; curr != NULL; curr = curr->next) {
        if (curr->fd == fd) {
            prev->next = curr->next;
            local_fd_destroy(curr, true);
            curr = prev;
            return 0;
        } else {
            prev = curr;
        }
    }
    return 1;
}