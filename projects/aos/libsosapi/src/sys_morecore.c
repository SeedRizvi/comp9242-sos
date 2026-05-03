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
#include <autoconf.h>
#include <utils/util.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <errno.h>
#include <assert.h>
#include <sos.h>
#include "../../sos/src/syscall_codes.h"


/*
 * Statically allocated morecore area.
 *
 * This is rather terrible, but is the simplest option without a
 * huge amount of infrastructure.
 */
#define MORECORE_AREA_BYTE_SIZE 0x100000
char morecore_area[MORECORE_AREA_BYTE_SIZE];

/* Pointer to free space in the morecore area. */
static uintptr_t morecore_base = (uintptr_t) &morecore_area;
static uintptr_t morecore_top = (uintptr_t) &morecore_area[MORECORE_AREA_BYTE_SIZE];

/* Actual morecore implementation
   returns 0 if failure, returns newbrk if success.
*/

long sys_brk(va_list ap)
{
    uintptr_t newbrk = va_arg(ap, uintptr_t);
    seL4_MessageInfo_t msg; 
    if (!newbrk) {
        msg = set_syscall_mr(SOS_SYSCALL_BRK, 1);
    }
    else {
        msg = set_syscall_mr(SOS_SYSCALL_BRK, 2);
        seL4_SetMR(1, newbrk);
    }
    seL4_MessageInfo_t result = seL4_Call(SOS_IPC_EP_CAP, msg);
    uintptr_t ret = seL4_GetMR(0);
    // dprintf(2, "sys_brk exiting with retval: %lu\n", ret);
    return ret;
}

/* Large mallocs will result in muslc calling mmap, so we do a minimal implementation
   here to support that. We make a bunch of assumptions in the process */
long sys_mmap(va_list ap)
{
    ZF_LOGE("Large memory allocation using mmap() is not supported\n");
    return -1;
}
