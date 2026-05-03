#include <fcntl.h>
#include <nfsc/libnfs.h>
#include "utils.h"
#include "files.h"
#include "filesystem.h"
#include "network.h"
#include "threads.h"
#include "serverside.h"
#include <string.h>
#include "../../libsosapi/include/sos.h"
#include <stdlib.h>
#include "pagefile.h"
#include "utils.h"

#include "elf_file.h"

#include "std_sync.h"

static struct nfs_context *nfs = NULL;
static struct nfsfh *pf_handle = NULL;
static seL4_CPtr pf_ntfn = 0;
static ut_t *pf_ntfn_ut = NULL;
static seL4_CPtr pf_seek_ntfn = 0;
static ut_t *pf_seek_ntfn_ut = NULL;
static seL4_CPtr pf_truncate_ntfn = 0;
static ut_t *pf_truncate_ntfn_ut = NULL;


// =================================
// ======== PRIVATE HELPERS ========
// =================================
// https://www.cl.cam.ac.uk/~mgk25/ucs/utf8_check.c
// Markus Kuhn <http://www.cl.cam.ac.uk/~mgk25/> -- 2005-03-30
// * License: http://www.cl.cam.ac.uk/~mgk25/short-license.html
unsigned char *utf8_check(unsigned char *s)
{
  while (*s) {
    if (*s < 0x80)
      /* 0xxxxxxx */
      s++;
    else if ((s[0] & 0xe0) == 0xc0) {
      /* 110XXXXx 10xxxxxx */
      if ((s[1] & 0xc0) != 0x80 ||
	  (s[0] & 0xfe) == 0xc0)                        /* overlong? */
	return s;
      else
	s += 2;
    } else if ((s[0] & 0xf0) == 0xe0) {
      /* 1110XXXX 10Xxxxxx 10xxxxxx */
      if ((s[1] & 0xc0) != 0x80 ||
	  (s[2] & 0xc0) != 0x80 ||
	  (s[0] == 0xe0 && (s[1] & 0xe0) == 0x80) ||    /* overlong? */
	  (s[0] == 0xed && (s[1] & 0xe0) == 0xa0) ||    /* surrogate? */
	  (s[0] == 0xef && s[1] == 0xbf &&
	   (s[2] & 0xfe) == 0xbe))                      /* U+FFFE or U+FFFF? */
	return s;
      else
	s += 3;
    } else if ((s[0] & 0xf8) == 0xf0) {
      /* 11110XXX 10XXxxxx 10xxxxxx 10xxxxxx */
      if ((s[1] & 0xc0) != 0x80 ||
	  (s[2] & 0xc0) != 0x80 ||
	  (s[3] & 0xc0) != 0x80 ||
	  (s[0] == 0xf0 && (s[1] & 0xf0) == 0x80) ||    /* overlong? */
	  (s[0] == 0xf4 && s[1] > 0x8f) || s[0] > 0xf4) /* > U+10FFFF? */
	return s;
      else
	s += 4;
    } else
      return s;
  }

  return NULL;
}

// Checks if args->name is a valid utf8 file name to avoid bugging
// odroid filesystem.
bool nfs_path_check(nfs_args_t *args) {
    if (utf8_check((unsigned char *)(args->name)) != NULL) {
        ZF_LOGE("Error: non-utf8 file path provided");
        args->result = -1;
        seL4_Signal(args->ntfn);
        return false;
    }
    return true;
}


// ==========================
// ======== NFS ARGS ========
// ==========================
nfs_args_t *new_nfs_blank_arg() {
    nfs_args_t *ret = malloc(sizeof(nfs_args_t));
    if (new_notification(&ret->ntfn, (void *)&ret->ntfn_ut, false) != 0) {
        free(ret);
        ZF_LOGE("Error: could not allocate nfs_blank_arg");
        return NULL;
    }

    ret->operation = 0;
    ret->name = NULL; // Will be freed if not null.
    ret->pathlen = 0;

    ret->pid = 0;
    
    ret->fd = -1;
    ret->handle = NULL;
    ret->flags = 0;
    ret->devtype = -1;
    ret->lfd = NULL;

    ret->usr_buf = (uintptr_t)NULL;
    ret->extra_buf = NULL; // Will be freed if not null.
    ret->nbyte = 0;
    ret->result = -1;

    ret->pos = -1;

    return ret;
}

int update_nfs_args(nfs_args_t * args) {
    ZF_LOGD("Update NFS args with fd = %d\n", args->fd);

    wait_on_fs_mount();

    // Get handle from file descriptor.
    local_fd_t *lfd = get_local_fd(args->pid, args->fd);
    if (lfd == NULL) {
        ZF_LOGE("Error: could not retrieve entry from fd\n");
        return 1;
    }

    args->handle = lfd->handle;
    args->flags = lfd->flags;
    args->devtype = lfd->devtype;
    args->lfd = lfd;
    return 0;
}

void free_nfs_args(void *data) {
    nfs_args_t *nfs_args = (nfs_args_t *)data;
    remove_notification(nfs_args->ntfn, nfs_args->ntfn_ut);
    if (nfs_args->name != NULL) free(nfs_args->name);
    if (nfs_args->extra_buf != NULL) free(nfs_args->extra_buf);
    free(nfs_args);
}


// ==========================
// ==== FS ERROR CHECKS =====
// ==========================
bool is_valid_fd(int fd) {
    if (fd < 0) {
        ZF_LOGE("Error: invalid fd\n");
        return false;
    }
    return true;
}

bool has_read_perm(nfs_args_t *args) {
    if ((args->flags & O_ACCMODE) != O_RDONLY && (args->flags & O_ACCMODE) != O_RDWR) {
        ZF_LOGE("Error: no read permission on file\n");
        return false;
    }
    return true;
}

bool has_write_perm(nfs_args_t *args) {
    if ((args->flags & O_ACCMODE) != O_WRONLY && (args->flags & O_ACCMODE) != O_RDWR) {
        ZF_LOGE("Error: no write permission on file\n");
        return false;
    }
    return true;
}

bool is_pagefile(char *str) {
    if (strcmp(str, PAGEFILE_PATH) == 0) {
        ZF_LOGE("Error: path shared by pagefile\n");
        return true;
    }
    return false;
}


// ================================================
// ======== PAGEFILE FILESYSTEM OPERATIONS ========
// ================================================
void fs_init_pagefile(void *data) {
    (void)data;
    nfs_args_t *pf_args = new_nfs_blank_arg();
    if (pf_args == NULL) {
        ZF_LOGF("Error: could not create pf_args\n");
        os_exit();
        return;
    }
    pf_args->name = malloc(strlen(PAGEFILE_PATH) + 1);
    if (pf_args->name == NULL) {
        ZF_LOGF("Error: could not allocate space for pathname of pagefile\n");
        os_exit();
        return;
    }

    sprintf(pf_args->name, PAGEFILE_PATH);
    pf_args->operation = FS_OPEN;
    pf_args->flags = O_RDWR | O_TRUNC;
    pf_args->devtype = PAGEFILE_DEVTYPE;

    fs_ops_caller(pf_args);
    if (pf_args->result) {
        ZF_LOGF("Error: could not open pagefile\n");
    } else {
        ZF_LOGI("Pagefile Initialised\n");
    }
    os_exit();
    return;
}

void pf_truncate_cb(int err, UNUSED struct nfs_context *nfs, void *data, void *priv_data) {
    int *status = (priv_data);
    if (err < 0) {
        *status = -1;
        ZF_LOGE("Error occurred in FS Operation pagefile truncate: %s\n", (char *)data);
    }
    ZF_LOGD("PF Truncate Signal\n");
    seL4_Signal(pf_truncate_ntfn);
}

int pf_truncate(uint64_t len) {
    ZF_LOGD("PF Wait Start\n");
    seL4_Wait(pf_ntfn, NULL);
    ZF_LOGD("PF Wait End\n");
    int status = 0;
    nfslib_lock();
    nfs_ftruncate_async(nfs, pf_handle, len, &pf_truncate_cb, &status);
    nfslib_unlock();
    ZF_LOGD("PF Truncate Wait Start\n");
    seL4_Wait(pf_truncate_ntfn, NULL);
    ZF_LOGD("PF Truncate Wait End\n");
    if (status != 0) {
        ZF_LOGE("Error: unsuccessful pf_truncate\n");   
    }
    ZF_LOGD("PF Signal\n");
    seL4_Signal(pf_ntfn);
    return status;
}

// Seek to offset within pagefile.
int pf_seek(int64_t offset) {
    nfs_args_t *args = new_nfs_blank_arg();
    if (args == NULL) {
        ZF_LOGE("Could not create args for pf_seek\n");
        return 1;
    }
    args->operation = FS_SEEK;
    args->nbyte = offset;
    args->handle = pf_handle;
    fs_ops_caller(args);
    int ret = args->result;
    free_nfs_args(args);
    return ret;
}

int fs_write_pagefile(int64_t offset, char *src, size_t nbyte) {
    ZF_LOGD("Pagefile Write\n");

    ZF_LOGD("PF Wait Start\n");
    seL4_Wait(pf_ntfn, NULL);
    ZF_LOGD("PF Wait End\n");

    if (pf_seek(offset) != offset) {
        ZF_LOGE("Error: pf seek failed\n");
        ZF_LOGD("PF Signal\n");
        seL4_Signal(pf_ntfn);
        return -1;
    }

    nfs_args_t *pfwr_args = new_nfs_blank_arg();
    if (pfwr_args == NULL) {
        ZF_LOGE("Error: could not create pf_args\n");
        ZF_LOGD("PF Signal\n");
        seL4_Signal(pf_ntfn);
        return -1;
    }

    pfwr_args->operation = FS_WRITE;
    pfwr_args->devtype = PAGEFILE_DEVTYPE;
    pfwr_args->handle = pf_handle;
    pfwr_args->nbyte = nbyte;
    pfwr_args->usr_buf = (uintptr_t)src;

    fs_ops_caller(pfwr_args);

    if (pfwr_args->result < 0) {
        ZF_LOGE("Error: could not write pagefile\n");
    }

    free_nfs_args(pfwr_args);
    ZF_LOGD("PF Signal\n");
    seL4_Signal(pf_ntfn);
    return pfwr_args->result;
}

int fs_read_pagefile(int64_t offset, char *dst, size_t nbyte) {
    ZF_LOGD("Pagefile Read\n");

    ZF_LOGD("PF Wait Start\n");
    seL4_Wait(pf_ntfn, NULL);
    ZF_LOGD("PF Wait End\n");
    if (pf_seek(offset) != offset) {
        ZF_LOGE("Error: pf_seek failed for pf read\n");
        ZF_LOGD("PF Signal\n");
        seL4_Signal(pf_ntfn);
        return -1;
    }

    nfs_args_t *pfrd_args = new_nfs_blank_arg();
    if (pfrd_args == NULL) {
        ZF_LOGE("Error: could not create pf_args\n");
        ZF_LOGD("PF Signal\n");
        seL4_Signal(pf_ntfn);
        return -1;
    }
    pfrd_args->operation = FS_READ;
    pfrd_args->devtype = PAGEFILE_DEVTYPE;
    pfrd_args->handle = pf_handle;
    pfrd_args->nbyte = nbyte;
    pfrd_args->usr_buf = (uintptr_t)dst;

    fs_ops_caller(pfrd_args);
    if (pfrd_args->result  < 0) {
        ZF_LOGE("Error: could not read pagefile\n");
    }

    free_nfs_args(pfrd_args);
    ZF_LOGD("PF Signal\n");
    seL4_Signal(pf_ntfn);
    return pfrd_args->result;
}


// =========================================
// ======== GENERIC FILE OPERATIONS ========
// =========================================
void fs_global_init() {
    if (new_notification(&pf_ntfn, (void *)&pf_ntfn_ut, false) != 0) {
        ZF_LOGF("Error: could not create notification for pagefile\n");
        os_exit();
        return;
    }
    if (new_notification(&pf_seek_ntfn, (void *)&pf_seek_ntfn_ut, false) != 0) {
        ZF_LOGF("Error: could not create notification for pagefile seek\n");
        os_exit();
        return;
    }
    if (new_notification(&pf_truncate_ntfn, (void *)&pf_truncate_ntfn_ut, false) != 0) {
        ZF_LOGF("Error: could not create notification for pagefile truncate\n");
        os_exit();
        return;
    }
}

void fs_init(struct nfs_context *from_cb) {
    nfs = from_cb;
    if (nfs != NULL) {
        ZF_LOGD("Received NFS from network.c\n");
    } else {
        ZF_LOGE("Error: received NULL from network.c\n");
    }
}

void fs_destroy() {
    nfs = NULL;
    ZF_LOGI("FS Service Stopped\n");
}

void fs_signal(nfs_args_t *args) {
    ZF_LOGD("FS Signal\n");
    seL4_Signal(args->ntfn);
}

void fs_wait(nfs_args_t *args) {
    ZF_LOGD("FS Wait Start\n");
    seL4_Wait(args->ntfn, NULL);
    ZF_LOGD("FS Wait End\n");
}

void fs_ops_caller(nfs_args_t *args) {
    ZF_LOGD("FS Operation Started\n");
    wait_on_fs_mount();
    nfslib_lock();
    switch (args->operation) {
        case FS_OPEN:
            ZF_LOGD("FS Open Launching\n");
            nfs_open_launcher(args);
            break;
        case FS_CLOSE:
            ZF_LOGD("FS Close Launching\n");
            nfs_close_launcher(args);
            break;
        case FS_READ:
            ZF_LOGD("FS Read Launching\n");
            nfs_read_launcher(args);
            break;
        case FS_WRITE:
            ZF_LOGD("FS Write Launching\n");
            nfs_write_launcher(args);
            break;
        case FS_DIRENT:
            ZF_LOGD("FS Dirent Launching\n");
            is_queued(nfs_opendir_async(nfs, DIRENT_OPEN_STR, &fs_ops_cb, args), args);
            break;
        case FS_STAT:
            ZF_LOGD("FS Stat Launching\n");
            nfs_stat_launcher(args);
            break;
        case FS_SEEK:
            ZF_LOGD("FS Seek Launching\n");
            nfs_seek_launcher(args);
            break;
    }
    nfslib_unlock();
    fs_wait(args);
}

void fs_ops_cb(int err, UNUSED struct nfs_context *nfs, void *data, void *priv_data) {
    ZF_LOGD("FS Callback Started\n");
    nfs_args_t *p_data = (priv_data);
    if (err < 0) {
        p_data->result = -1;
        ZF_LOGE("Error occurred in FS Operation %d: %s\n", p_data->operation, (char *)data);
        fs_signal(p_data);
        return;
    }

    switch (p_data->operation) {
        case FS_OPEN:
            ZF_LOGD("FS Open Callback\n");
            fs_open_callback(p_data, data);
            break;
        case FS_CLOSE:
            ZF_LOGD("FS Close Callback\n");
            if (p_data->flags != CLOSE_NOREMOVE) {
                p_data->result = remove_local_fd(p_data->pid, p_data->fd);
            } else {
                p_data->result = 0;
            }
            p_data->lfd = NULL;
            break;
        case FS_READ:
            ZF_LOGD("FS Read Callback\n");
            fs_read_callback(err, p_data, data);
            break;
        case FS_WRITE:
            ZF_LOGD("FS Write Callback\n");
            p_data->result = err;
            break;
        case FS_DIRENT:
            ZF_LOGD("FS Dirent Callback\n");
            fs_dirent_callback(p_data, data);
            nfs_closedir(nfs, (struct nfsdir *)data);
            break;
        case FS_STAT:
            ZF_LOGD("FS Stat Callback\n");
            fs_stat_callback(p_data, data);
            break;
        case FS_SEEK:
            ZF_LOGD("FS Seek Callback\n");
            p_data->result = *((uint64_t *)data);
            break;
    }
    fs_signal(p_data);
}

void is_queued(int status, nfs_args_t *args) {
    if (status < 0) {
        ZF_LOGE("Error: NFS_OP failed to queue\n");
        fs_signal(args);
        args->result = -1;
        return;
    }
    ZF_LOGD("NFS_OP %d queued successfully\n", args->operation);
}


// =======================
// ==== OPEN HANDLERS ====
// =======================
void nfs_open_launcher(nfs_args_t *args) {
    // File case.
    if (args->devtype == FILE_DEVTYPE || args->devtype == PAGEFILE_DEVTYPE) {
        if (!nfs_path_check(args)) return;
        is_queued(nfs_open_async(nfs, args->name, args->flags | O_CREAT, &fs_ops_cb, args), args);

    // Console device case.
    } else if (args->devtype == CONSOLE_DEVTYPE) {
        ZF_LOGI("Nfs open on console device\n");
        args->result = add_local_fd(args->pid, NULL, args->devtype, args->flags);
        fs_signal(args);

    // Invalid device type.
    } else {
        ZF_LOGE("Error: invalid device type for open\n");
        args->result = -1;
        fs_signal(args);
    }
}

void fs_open_callback(nfs_args_t *p_data, void *data) {
    struct nfsfh *fh = (data);

    // Special case for pagefile.
    if (p_data->devtype == PAGEFILE_DEVTYPE) {
        p_data->result = 0;
        pf_handle = fh;
        seL4_Signal(pf_ntfn);
        return;
    }

    if (fh == NULL) {
        p_data->result = -1;
    } else {
        p_data->result = add_local_fd(p_data->pid, fh, p_data->devtype, p_data->flags); 
    }
}


// ========================
// ==== CLOSE HANDLING ====
// ========================
void nfs_close_launcher(nfs_args_t *args) {
    // File device type.
    if (args->devtype == FILE_DEVTYPE) {
        if (args->handle != NULL) {
            is_queued(nfs_close_async(nfs, args->handle, &fs_ops_cb, args), args);
        } else {
            ZF_LOGE("Error: handle for file closure is NULL\n");
            args->result = -1;
            fs_signal(args);
        }
        
    // Console device type.
    } else if (args->devtype == CONSOLE_DEVTYPE) {
        args->result = remove_local_fd(args->pid, args->fd);
        args->lfd = NULL;
        fs_signal(args);
    
    // Invalid device type.
    } else {
        args->lfd = NULL;
        ZF_LOGE("Error: invalid device type for close\n");
        args->result = -1;
        fs_signal(args);
    }
}


// =======================
// ==== READ HANDLING ====
// =======================
void nfs_read_launcher(nfs_args_t *args) {
    if (args->handle != NULL) {
        is_queued(nfs_read_async(nfs, args->handle, args->nbyte, &fs_ops_cb, args), args);
    } else {
        ZF_LOGE("Error: NULL handle for read\n");
        args->result = -1;
        fs_signal(args);
    }
}

void fs_read_callback(int err, nfs_args_t *p_data, void *data) {
    p_data->result = err;
    if (p_data->devtype == FILE_DEVTYPE) {
        p_data->extra_buf = malloc(p_data->result);
        if (p_data->extra_buf == NULL) {
            p_data->result = -1;
            return;
        }
        memcpy(p_data->extra_buf, data, p_data->result);
        
    } else if (p_data->devtype == PAGEFILE_DEVTYPE) {
        memcpy((char *)(p_data->usr_buf), data, p_data->result);
        p_data->result = p_data->result;
        p_data->extra_buf = NULL;
    }
}


// ========================
// ==== WRITE HANDLING ====
// ========================
void nfs_write_launcher(nfs_args_t *args) {
    if (args->handle != NULL) {
        is_queued(nfs_write_async(nfs, args->handle, args->nbyte, (char *)(args->usr_buf), &fs_ops_cb, args), args);
    } else {
        ZF_LOGE("Error: NULL handle for write\n");
        args->result = -1;
        fs_signal(args);
    }
}


// =========================
// ==== DIRENT HANDLING ====
// =========================
void fs_dirent_callback(nfs_args_t *p_data, void *data) {
    struct nfsdirent *dir = nfs_readdir(nfs, data);

    int pos = p_data->pos;
    for (int cur_pos = 0; cur_pos <= pos && dir != NULL; cur_pos++, dir=dir->next) {

        // Single level filesystem, so we ignore parent dir
        if (strcmp(dir->name, PARENT_DIR_STR) == 0) {
            cur_pos--;
            continue;
        }

        if (cur_pos == pos - 1 && dir->next == NULL) {
            p_data->pos = 0;
            p_data->result = p_data->pos;
            return;
        }

        if (cur_pos == pos) {
            if (p_data->nbyte <= strlen(dir->name) + 1) {
                p_data->pos = p_data->nbyte;
                strncpy(p_data->extra_buf, dir->name, p_data->nbyte);
            } else {
                p_data->pos = strlen(dir->name) + 1;
                strncpy(p_data->extra_buf, dir->name, strlen(dir->name) + 1);
            }
            p_data->result = p_data->pos;
            return;
        }
    }
    
    p_data->result = -1;
}


// =======================
// ==== STAT HANDLING ====
// =======================
void nfs_stat_launcher(nfs_args_t *args) {
    if (!nfs_path_check(args)) return;
    is_queued(nfs_stat64_async(nfs, (char *)args->name, &fs_ops_cb, args), args);
}

void fs_stat_callback(nfs_args_t *p_data, void *data) {
    sos_stat_t *ret_stat = p_data->extra_buf;
    struct nfs_stat_64 *f_stat = (data);

    ret_stat->st_type = f_stat->nfs_mode;
    ret_stat->st_fmode = f_stat->nfs_mode;
    ret_stat->st_size = f_stat->nfs_size;
    ret_stat->st_ctime = f_stat->nfs_ctime;
    ret_stat->st_atime = f_stat->nfs_atime;

    p_data->result = 0;
}

// =======================
// ==== SEEK HANDLING ====
// =======================
void nfs_seek_launcher(nfs_args_t *args) {
    is_queued(nfs_lseek_async(nfs, args->handle, args->nbyte, 0, &fs_ops_cb, args), args);
}


// =======================================
// ==== MISC PUBLIC UTILITY FUNCTIONS ====
// =======================================
void close_file_by_fd(int pid, int fd, bool ext) {
    nfs_args_t *args = new_nfs_blank_arg();
    if (args == NULL) return;

    args->operation = FS_CLOSE;
    args->devtype = FILE_DEVTYPE;
    args->fd = fd;
    args->pid = pid;
    if (ext) {
        args->flags = 0;
    } else {
        args->flags = CLOSE_NOREMOVE;
    }
    
    if (update_nfs_args(args)) {
        ZF_LOGE("CFBF: Could not get handle from fd and pid\n");
        free_nfs_args(args);
        return;
    }

    fs_ops_caller(args);

    if (args->result != 0) {
        ZF_LOGE("Error: close file by handle failed\n");
    }
    return;
}

void close_file_by_handle(local_fd_t *lfd) {
    nfs_args_t *args = new_nfs_blank_arg();
    if (args == NULL) return;

    args->operation = FS_CLOSE;
    args->devtype = FILE_DEVTYPE;
    args->flags = CLOSE_NOREMOVE;
    args->handle = lfd->handle;

    fs_ops_caller(args);

    if (args->result != 0) {
        ZF_LOGE("Error: close file by handle failed\n");
    }
    return;
}

void *read_fd_to_buf(int pid, int fd, size_t offset, size_t nbyte) {
    nfs_args_t *args = new_nfs_blank_arg();
    if (args == NULL) return NULL;

    args->operation = FS_SEEK;
    args->nbyte = offset;
    args->devtype = FILE_DEVTYPE;
    args->fd = fd;
    args->pid = pid;

    if (update_nfs_args(args)) {
        ZF_LOGE("RFTB: Could not get handle from fd and pid\n");
        free_nfs_args(args);
        return NULL;
    }

    fs_ops_caller(args);
    if ((size_t)args->result != offset) {
        ZF_LOGE("RFTB: Could not seek to proper file position\n");
        free_nfs_args(args);
        return NULL;
    }

    args->operation = FS_READ;
    args->nbyte = nbyte;

    fs_ops_caller(args);
    if ((size_t)args->result != nbyte) {
        ZF_LOGE("RFTB: Could not read entire file contents\n");
        free_nfs_args(args);
        return NULL;
    }
    void *retval = args->extra_buf;
    args->extra_buf = NULL;
    free_nfs_args(args);
    return retval;
}

void *elf_read(int pid, size_t offset, size_t nbyte) {
    return read_fd_to_buf(pid, ELF_FILE_FD, offset, nbyte);
}