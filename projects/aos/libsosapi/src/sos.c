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
#include <stdarg.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sos.h>
#include <fcntl.h> // for Linux RDONLY/WRONLY values

#include <sel4/sel4.h>
#include <sys/stat.h>
#include "../../sos/src/syscall_codes.h"

char test[100] = {'\0'};

int safe_path_len(const char *path) {
    if (path == NULL) return -1;
    int len = 0;
    while (1) {
        if (len > MAX_PATH) {
            return -1;
        }
	if (path[len++] == '\0') {
            break;
        }
    }
    return len;
}

static size_t sos_debug_print(const void *vData, size_t count)
{
#ifdef CONFIG_DEBUG_BUILD
    size_t i;
    const char *realdata = vData;
    for (i = 0; i < count; i++) {
        seL4_DebugPutChar(realdata[i]);
    }
#endif
    return count;
}

static void sos_dprintf(const char *str) {
    sprintf(test, str);
    sos_debug_print(test, strlen(test));
}

int sos_open(const char *path, fmode_t mode)
{
    //sos_dprintf("OPEN CALLED\n");

    // Pathlen and null termination check.
    int pathlen = safe_path_len(path);
    if(pathlen < 0) {
        return -1;
    }

    // Initial syscall setup.
    seL4_SetMR(1, (seL4_Word)path);
    seL4_SetMR(2, pathlen);
    seL4_SetMR(3, mode);
    if (strcmp(path, CONSOLE_PATH) != 0) {
        seL4_SetMR(4, 0);
    } else {
        seL4_SetMR(4, 1);
    }

    seL4_Call(SOS_IPC_EP_CAP, set_syscall_mr(SOS_SYSCALL_OPEN, 5));
    if (!seL4_GetMR(0)) {
        return seL4_GetMR(1);
    }
    return -1;
}

int sos_close(int file)
{
    //sos_dprintf("CLOSE CALLED\n");

    if (file < 0) return -1;

    // Send syscall.
    seL4_SetMR(1, file);
    seL4_Call(SOS_IPC_EP_CAP, set_syscall_mr(SOS_SYSCALL_CLOSE, 2));
    if (!seL4_GetMR(0)) {
        return seL4_GetMR(1);
    }
    return -1;
}

int sos_read(int file, char *buf, size_t nbyte)
{
    //sos_dprintf("READ CALLED\n");

    if (file < 0 || buf == NULL || nbyte <= 0) return -1;

    // Initial system call for IO.
    seL4_SetMR(0, SOS_SYSCALL_RD);
    seL4_SetMR(1, file);
    seL4_SetMR(2, (uintptr_t)buf);
    seL4_SetMR(3, nbyte);

    // Get ep.
    seL4_Call(SOS_IPC_EP_CAP, seL4_MessageInfo_new(0, 0, 0, 4));
    if (!seL4_GetMR(0)) {
        return seL4_GetMR(1);
    }
    return -1;
}

int sos_write(int file, const char *buf, size_t nbyte)
{
    //sos_dprintf("WRITE CALLED\n");

    if (file < 0 || buf == NULL || nbyte <= 0) return -1;

    // Initial system call for IO.
    seL4_SetMR(0, SOS_SYSCALL_WR);
    seL4_SetMR(1, file);
    seL4_SetMR(2, (uintptr_t)buf);
    seL4_SetMR(3, nbyte);

    seL4_Call(SOS_IPC_EP_CAP, seL4_MessageInfo_new(0, 0, 0, 4));
    if (!seL4_GetMR(0)) {
        return seL4_GetMR(1);
    }
    return -1;
}

int sos_getdirent(int pos, char *name, size_t nbyte)
{
    //sos_dprintf("DIRENT CALLED\n");

    if (nbyte <= 0 || pos < 0 || !safe_path_len(name)) return -1;
    
    // Initial syscall for IO.
    seL4_MessageInfo_t msg = set_syscall_mr(SOS_SYSCALL_GETDIRENT, 4);
    seL4_SetMR(1, pos);
    seL4_SetMR(2, nbyte);
    seL4_SetMR(3, (uintptr_t)name);
    seL4_Call(SOS_IPC_EP_CAP, msg);
    if (!seL4_GetMR(0)) {
        return seL4_GetMR(1);
    }
    return -1;
}

int sos_stat(const char *path, sos_stat_t *buf)
{
    //sos_dprintf("STAT CALLED\n");

    if (buf == NULL) return -1;

    // Pathlen and null termination check.
    int pathlen = safe_path_len(path);
    if(pathlen < 0) {
        return -1;
    }

    // Initial syscall for IO.
    seL4_MessageInfo_t msg = set_syscall_mr(SOS_SYSCALL_STAT, 4);
    seL4_SetMR(1, (uintptr_t)path);
    seL4_SetMR(2, pathlen);
    seL4_SetMR(3, (uintptr_t)buf);

    int result = -1;
    seL4_Call(SOS_IPC_EP_CAP, msg);
    if (!seL4_GetMR(0)) {
        result = seL4_GetMR(1);
    }
    
    if (!result) {
        fmode_t cpy = buf->st_fmode;
        buf->st_fmode = 0;
        buf->st_fmode |= (cpy & S_IRUSR) ? FM_READ : 0;
        buf->st_fmode |= (cpy & S_IWUSR) ? FM_WRITE : 0; 
        buf->st_fmode |= (cpy & S_IXUSR) ? FM_EXEC : 0; 
    }

    return result;
}

pid_t sos_process_create(const char *path)
{
    //sos_dprintf("PROC CREATE CALLED\n");
    
    seL4_MessageInfo_t msg = set_syscall_mr(SOS_SYSCALL_PROC_CREATE, 3);
    int pathlen = safe_path_len(path);
    seL4_SetMR(1, (uintptr_t)path);
    seL4_SetMR(2, pathlen);
    
    seL4_Call(SOS_IPC_EP_CAP, msg);
    
    if (!seL4_GetMR(0)) {
        return seL4_GetMR(1);
    }
    return -1;
}

int sos_process_delete(pid_t pid)
{
    //sos_dprintf("PROC DELETE CALLED\n");

    seL4_MessageInfo_t msg = set_syscall_mr(SOS_SYSCALL_PROC_DEL, 2);
    seL4_SetMR(1, pid);
    seL4_Call(SOS_IPC_EP_CAP, msg);
    
    if (!seL4_GetMR(0)) {
        return seL4_GetMR(1);
    }
    return -1;
}

pid_t sos_my_id(void)
{
    //sos_dprintf("PROC ID CALLED\n");

    seL4_MessageInfo_t msg = set_syscall_mr(SOS_SYSCALL_PROC_ID, 1);
    seL4_Call(SOS_IPC_EP_CAP, msg);
    // my_id syscall doesn't go through serverside
    return seL4_GetMR(0);
}

int sos_process_status(sos_process_t *processes, unsigned max)
{
    //sos_dprintf("PROC STATUS CALLED\n");

    seL4_MessageInfo_t msg = set_syscall_mr(SOS_SYSCALL_PROC_STATUS, 3);
    seL4_SetMR(1, (uintptr_t)processes);
    seL4_SetMR(2, max);
    seL4_Call(SOS_IPC_EP_CAP, msg);
    
    if (!seL4_GetMR(0)) {
        return seL4_GetMR(1);
    }
    return -1;
}

pid_t sos_process_wait(pid_t pid)
{
    //sos_dprintf("PROC WAIT CALLED\n");

    seL4_MessageInfo_t msg = set_syscall_mr(SOS_SYSCALL_PROC_WAIT, 2);
    seL4_SetMR(1, pid);
    seL4_Call(SOS_IPC_EP_CAP, msg);

    if (!seL4_GetMR(0)) {
        return seL4_GetMR(1);
    }
    return -1;
}

void sos_usleep(int msec)
{
    //sos_dprintf("SLEEP CALLED\n");

    if (msec <= 0) return;

    // Initial system call for IO.
    seL4_SetMR(0, SOS_SYSCALL_SLEEP);
    seL4_SetMR(1, msec);
    seL4_Call(SOS_IPC_EP_CAP, seL4_MessageInfo_new(0, 0, 0, 2));
}

int64_t sos_time_stamp(void)
{
    //sos_dprintf("TIMESTAMP CALLED\n");

    // Get timestamp and return.
    seL4_MessageInfo_t msg = set_syscall_mr(SOS_SYSCALL_TIMESTAMP, 1);
    seL4_MessageInfo_t result = seL4_Call(SOS_IPC_EP_CAP, msg);
    return (int64_t)(seL4_GetMR(0));
}

seL4_MessageInfo_t set_syscall_mr(int code, int size) {
    seL4_MessageInfo_t ret = seL4_MessageInfo_new(0, 0, 0, size);
    seL4_SetMR(0, code);
    return ret;
}