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
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>

#include <cspace/cspace.h>
#include <aos/sel4_zf_logif.h>
#include <aos/debug.h>

#include <clock/clock.h>
#include <cpio/cpio.h>
#include <elf/elf.h>
#include <fcntl.h>

#include <sel4runtime.h>
#include <sel4runtime/auxv.h>

#include "syscall_codes.h"
#include "bootstrap.h"
#include "irq.h"
#include "network.h"
#include "frame_table.h"
#include "drivers/uart.h"
#include "ut.h"
#include "vmem_layout.h"
#include "mapping.h"
#include "elfload.h"
#include "syscalls.h"
#include "tests.h"
#include "utils.h"
#include "threads.h"
#include <sos/gen_config.h>
#include "serverside.h"
#include <aos/vsyscall.h>
#include "netcon.h"
#include "filesystem.h"
#include "vmem.h"
#include <sel4/faults.h>
#include "clock_sync.h"
#include "main_includes.h"
#include "files.h"
#include "process.h"
#include "backtrace.h"

#ifdef CONFIG_SOS_GDB_ENABLED
#include "debugger.h"
#endif /* CONFIG_SOS_GDB_ENABLED */

/*
 * To differentiate between signals from notification objects and and IPC messages,
 * we assign a badge to the notification object. The badge that we receive will
 * be the bitwise 'OR' of the notification object badge and the badges
 * of all pending IPC messages.
 *
 * All badged IRQs set high bit, then we use unique bits to
 * distinguish interrupt sources.
 */
#define IRQ_EP_BADGE         BIT(seL4_BadgeBits - 1ul)
#define IRQ_IDENT_BADGE_BITS MASK(seL4_BadgeBits - 1ul)

#define RUN_ADDITIONAL_TESTS false

#define SERVER_PRIORITY      (0)

/* Timer A IRQ, copied from device.h */
#define TIMER_A_IRQ  42

/* provided by gcc */
extern void (__register_frame)(void *);

/* root tasks cspace */
cspace_t cspace;

static seL4_CPtr sched_ctrl_start;
static seL4_CPtr sched_ctrl_end;

/* Timer A IRQ handler */
static seL4_IRQHandler tA_handler;

seL4_MessageInfo_t handle_fault(int badge, seL4_MessageInfo_t tag, bool *have_reply, seL4_CPtr *reply, ut_t **reply_ut) {
    seL4_Fault_t fault = seL4_getFault(tag);
    *have_reply = false;

    switch (seL4_Fault_get_seL4_FaultType(fault)) {
        case seL4_Fault_VMFault:
            (void)0;
            uintptr_t vaddr = seL4_Fault_VMFault_get_Addr(fault);
            ZF_LOGI("VM fault at vaddr = %p\n", vaddr);
            if (badge == SOS_THREAD_BADGE) { // We should never get a vm_fault from within SOS.
                print_backtrace();
                ZF_LOGF("Error: VM fault from SOS thread\n");
            }
            syscall_thread_spawner(badge, new_vm_fault_args, free_vm_fault_args, handle_vm_fault, "vm_fault", reply, reply_ut, &vaddr, have_reply);
            break;
    }

    return seL4_MessageInfo_new(0, 0, 0, 0);
}

/**
 * Deals with a syscall and sets the message registers before returning the
 * message info to be passed through to seL4_ReplyRecv()
 */
seL4_MessageInfo_t handle_syscall(seL4_Word badge, int num_args, bool *have_reply, seL4_CPtr *reply, ut_t **reply_ut)
{
    // Syscall number is first word of message.
    seL4_Word syscall_number = seL4_GetMR(0);

    // Statically allocated as a malloc would change ipc buffer values. Currently supports 10 arguments.
    // syscall_thread_spawner uses these values before spawning a thread with malloced data. As such, this does not
    // need to be freed and is safe to pass to syscall_thread_spawner.
    seL4_Word syscall_args[10];

    // Arguments are captured early to ensure that no operations change IPC buffer values (due to synchronisation, std_sync.h)
    // before they are used.
    syscall_args[0] = badge;
    for (int i = 1; i < num_args + 1 && i < 10; i++) {
        syscall_args[i] = seL4_GetMR(i);
    }

    // Initialise default reply params.
    *have_reply = true;
    seL4_MessageInfo_t reply_msg = seL4_MessageInfo_new(0, 0, 0, 0);

    /* Process system call */
    switch (syscall_number) {
    case SOS_SYSCALL_DUMMY:
        ZF_LOGI("Handling Syscall Dummy\n");
        reply_msg = handle_dummy();
        break;

    case SOS_SYSCALL_OPEN:
        ZF_LOGI("Handling Syscall Open\n");
        reply_msg = syscall_thread_spawner(badge, new_nfs_open_args, free_nfs_args, handle_open_thread, "open", reply, reply_ut, syscall_args, have_reply);
        break;

    case SOS_SYSCALL_CLOSE:
        ZF_LOGI("Handling Syscall Close\n");
        reply_msg = syscall_thread_spawner(badge, new_nfs_close_args, free_nfs_args, handle_close_thread, "write", reply, reply_ut, syscall_args, have_reply);
        break;

    case SOS_SYSCALL_RD:
        ZF_LOGI("Handling Syscall Read\n");
        reply_msg = syscall_thread_spawner(badge, new_nfs_read_args, free_nfs_args, handle_read_thread, "read", reply, reply_ut, syscall_args, have_reply);
        break;

    case SOS_SYSCALL_WR:
        ZF_LOGI("Handling Syscall Write\n");
        reply_msg = syscall_thread_spawner(badge, new_nfs_write_args, free_nfs_args, handle_write_thread, "write", reply, reply_ut, syscall_args, have_reply);
        break;

    case SOS_SYSCALL_GETDIRENT:
        ZF_LOGI("Handling Syscall Dirent\n");
        reply_msg = syscall_thread_spawner(badge, new_nfs_dirent_args, free_nfs_args, handle_dirent_thread, "dirent", reply, reply_ut, syscall_args, have_reply);
        break;

    case SOS_SYSCALL_STAT:
        ZF_LOGI("Handling Syscall Stat\n");
        reply_msg = syscall_thread_spawner(badge, new_nfs_stat_args, free_nfs_args, handle_stat_thread, "stat", reply, reply_ut, syscall_args, have_reply);
        break;

    case SOS_SYSCALL_PROC_CREATE:
        ZF_LOGI("Handling Syscall Proc Create\n");
        reply_msg = syscall_thread_spawner(badge, new_proc_create_args, free_nfs_args, handle_proc_thread, "proc_create", reply, reply_ut, syscall_args, have_reply);
        break;

    case SOS_SYSCALL_PROC_DEL:
        ZF_LOGI("Handling Syscall Proc Delete\n");
        reply_msg = syscall_thread_spawner(badge, new_proc_del_args, free_nfs_args, handle_proc_thread, "proc_delete", reply, reply_ut, syscall_args, have_reply);
        break;

    case SOS_SYSCALL_PROC_ID:
        ZF_LOGI("Handling Syscall Proc ID\n");
        reply_msg = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, (int)badge);
        *have_reply = true;
        break;

    case SOS_SYSCALL_PROC_STATUS:
        ZF_LOGI("Handling Syscall Proc Status\n");
        reply_msg = syscall_thread_spawner(badge, new_proc_stat_args, free_nfs_args, handle_proc_thread, "proc_stat", reply, reply_ut, syscall_args, have_reply);
        break;

    case SOS_SYSCALL_PROC_WAIT:
        ZF_LOGI("Handling Syscall Proc Wait\n");
        reply_msg = syscall_thread_spawner(badge, new_proc_wait_args, free_nfs_args, handle_proc_thread, "proc_wait", reply, reply_ut, syscall_args, have_reply);
        break;

    case SOS_SYSCALL_SLEEP:
        ZF_LOGI("Handling Syscall Sleep\n");
        reply_msg = syscall_thread_spawner(badge, new_sleep_args, free_sleep_args, handle_sleep_thread, "sleep", reply, reply_ut, syscall_args, have_reply);
        break;

    case SOS_SYSCALL_TIMESTAMP:
        ZF_LOGI("Handling Syscall Timestamp\n");
        reply_msg = handle_timestamp();
        break;

    case SOS_SYSCALL_BRK:
        ZF_LOGI("Handling Syscall Brk\n");
        reply_msg = (num_args) ? handle_brk(badge, seL4_GetMR(1)) : handle_brk(badge, 0);
        break;

    case SOS_SYSCALL_MMAP2:
        ZF_LOGI("Handling Syscall MMAP2\n");
        reply_msg = seL4_MessageInfo_new(0, 0, 0, 0);
        break;

    case SOS_SYSCALL_EXIT_OS_THREAD:
        if (badge == SOS_THREAD_BADGE) {
            handle_os_thread_exit();
        } else {
            ZF_LOGE("Error: unknown syscall %lu\n", syscall_number);
        }
        reply_msg = seL4_MessageInfo_new(0, 0, 0, 0);
        break;

    default:
        ZF_LOGE("Error: unknown syscall %lu\n", syscall_number);
        reply_msg = seL4_MessageInfo_new(0, 0, 0, 0);
    }

    return reply_msg;
}


NORETURN void syscall_loop(seL4_CPtr ep)
{
    seL4_CPtr reply;

    /* Create reply object */
    ut_t *reply_ut = alloc_retype(&reply, seL4_ReplyObject, seL4_ReplyBits);
    if (reply_ut == NULL) {
        ZF_LOGF("Failed to alloc reply object ut");
    }

    bool have_reply = false;
    seL4_MessageInfo_t reply_msg = seL4_MessageInfo_new(0, 0, 0, 0);

    // Print to Odroid Serial
    ZF_LOGI("[%lu] Entering \"syscall_loop\"\n", get_time());
    while (1) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t message;

        /* Reply (if there is a reply) and block on ep, waiting for an IPC
         * sent over ep, or a notification from our bound notification object */
        if (have_reply) {
            message = seL4_ReplyRecv(ep, reply_msg, &badge, reply);
        } else {
            message = seL4_Recv(ep, &badge, reply);
        }

        /* Awake! We got a message - check the label and badge to
         * see what the message is about */
        seL4_Word label = seL4_MessageInfo_get_label(message);

        if (badge & IRQ_EP_BADGE) {
            sos_handle_irq_notification(&badge, &have_reply);

        } else if (label == seL4_Fault_NullFault) {
            reply_msg = handle_syscall(badge, seL4_MessageInfo_get_length(message) - 1, &have_reply, &reply, &reply_ut);

        } else {
            reply_msg = handle_fault(badge, message, &have_reply, &reply, &reply_ut);
        }
    }
}

/* Allocate an endpoint and a notification object for sos.
 * Note that these objects will never be freed, so we do not
 * track the allocated ut objects anywhere
 */
static void sos_ipc_init(seL4_CPtr *ipc_ep, seL4_CPtr *ntfn)
{
    /* Create an notification object for interrupts */
    ut_t *ut = alloc_retype(ntfn, seL4_NotificationObject, seL4_NotificationBits);
    ZF_LOGF_IF(!ut, "No memory for notification object\n");

    /* Bind the notification object to our TCB */
    seL4_Error err = seL4_TCB_BindNotification(seL4_CapInitThreadTCB, *ntfn);
    ZF_LOGF_IFERR(err, "Failed to bind notification object to TCB\n");

    /* Create an endpoint for user application IPC */
    ut = alloc_retype(ipc_ep, seL4_EndpointObject, seL4_EndpointBits);
    ZF_LOGF_IF(!ut, "No memory for endpoint\n");
}

/* called by crt */
seL4_CPtr get_seL4_CapInitThreadTCB(void)
{
    return seL4_CapInitThreadTCB;
}

/* tell muslc about our "syscalls", which will be called by muslc on invocations to the c library */
void init_muslc(void)
{
    setbuf(stdout, NULL);

    muslcsys_install_syscall(__NR_set_tid_address, sys_set_tid_address);
    muslcsys_install_syscall(__NR_writev, sys_writev);
    muslcsys_install_syscall(__NR_exit, sys_exit);
    muslcsys_install_syscall(__NR_rt_sigprocmask, sys_rt_sigprocmask);
    muslcsys_install_syscall(__NR_gettid, sys_gettid);
    muslcsys_install_syscall(__NR_getpid, sys_getpid);
    muslcsys_install_syscall(__NR_tgkill, sys_tgkill);
    muslcsys_install_syscall(__NR_tkill, sys_tkill);
    muslcsys_install_syscall(__NR_exit_group, sys_exit_group);
    muslcsys_install_syscall(__NR_ioctl, sys_ioctl);
    muslcsys_install_syscall(__NR_mmap, sys_mmap);
    muslcsys_install_syscall(__NR_brk,  sys_brk);
    muslcsys_install_syscall(__NR_clock_gettime, sys_clock_gettime);
    muslcsys_install_syscall(__NR_nanosleep, sys_nanosleep);
    muslcsys_install_syscall(__NR_getuid, sys_getuid);
    muslcsys_install_syscall(__NR_getgid, sys_getgid);
    muslcsys_install_syscall(__NR_openat, sys_openat);
    muslcsys_install_syscall(__NR_close, sys_close);
    muslcsys_install_syscall(__NR_socket, sys_socket);
    muslcsys_install_syscall(__NR_bind, sys_bind);
    muslcsys_install_syscall(__NR_listen, sys_listen);
    muslcsys_install_syscall(__NR_connect, sys_connect);
    muslcsys_install_syscall(__NR_accept, sys_accept);
    muslcsys_install_syscall(__NR_sendto, sys_sendto);
    muslcsys_install_syscall(__NR_recvfrom, sys_recvfrom);
    muslcsys_install_syscall(__NR_readv, sys_readv);
    muslcsys_install_syscall(__NR_getsockname, sys_getsockname);
    muslcsys_install_syscall(__NR_getpeername, sys_getpeername);
    muslcsys_install_syscall(__NR_fcntl, sys_fcntl);
    muslcsys_install_syscall(__NR_setsockopt, sys_setsockopt);
    muslcsys_install_syscall(__NR_getsockopt, sys_getsockopt);
    muslcsys_install_syscall(__NR_ppoll, sys_ppoll);
    muslcsys_install_syscall(__NR_madvise, sys_madvise);
}

// Thread function for testing clock functionality.
void test_clock_handler(void *data) {
    (void)data;
    test_clock();
    os_exit();
}

void test_sos_framework() {
    ZF_LOGI("\n====STARTING TESTS====\n");
    spawn(&test_clock_handler, NULL, SOS_THREAD_BADGE, false);
    ZF_LOGI("====TESTS COMPLETE====\n\n");
}

NORETURN void *main_continued(UNUSED void *arg)
{
    /* Initialise other system compenents here */
    seL4_CPtr ipc_ep, ntfn;
    sos_ipc_init(&ipc_ep, &ntfn);

    sos_init_irq_dispatch(
        &cspace,
        seL4_CapIRQControl,
        ntfn,
        IRQ_EP_BADGE,
        IRQ_IDENT_BADGE_BITS
    );

    /* Initialize threads library */
#ifdef CONFIG_SOS_GDB_ENABLED
    /* Create an endpoint that the GDB threads listens to */
    seL4_CPtr gdb_recv_ep;
    ut_t *ep_ut = alloc_retype(&gdb_recv_ep, seL4_EndpointObject, seL4_EndpointBits);
    ZF_LOGF_IF(ep_ut == NULL, "Failed to create GDB endpoint");

    init_threads(ipc_ep, gdb_recv_ep, sched_ctrl_start, sched_ctrl_end);
#else
    init_threads(ipc_ep, ipc_ep, sched_ctrl_start, sched_ctrl_end);
#endif /* CONFIG_SOS_GDB_ENABLED */

    frame_table_init(&cspace, seL4_CapInitThreadVSpace);

    /* run sos initialisation tests */
    run_tests(&cspace);

    /* Map the timer device (NOTE: this is the same mapping you will use for your timer driver -
     * sos uses the watchdog timers on this page to implement reset infrastructure & network ticks,
     * so touching the watchdog timers here is not recommended!) */
    void *timer_vaddr = sos_map_device(&cspace, PAGE_ALIGN_4K(TIMER_MAP_BASE), PAGE_SIZE_4K);

    /* Initialise Process Table */
    ZF_LOGI("Process table init\n");
    init_proc_table(ipc_ep, sched_ctrl_start, sched_ctrl_end);
    /* Initialise the network hardware. */
    ZF_LOGI("Network init\n");
    network_init(&cspace, timer_vaddr, ntfn);
    if (init_netcon() != 0) {
        ZF_LOGE("Failed to initialise netcon");
    }


#ifdef CONFIG_SOS_GDB_ENABLED
    /* Initialize the debugger */
    seL4_Error err = debugger_init(&cspace, seL4_CapIRQControl, gdb_recv_ep);
    ZF_LOGF_IF(err, "Failed to initialize debugger %d", err);
#endif /* CONFIG_SOS_GDB_ENABLED */

    /* Initialises the timer */
    ZF_LOGI("Timer init\n");
    init_clock_sync();
    start_timer(timer_vaddr);
    sos_register_irq_handler(TIMER_A_IRQ, true, &timer_irq, NULL, &tA_handler);

    /* test sos after everything has initialised except processes */
    if (RUN_ADDITIONAL_TESTS) {
        test_sos_framework();
    }

    // Kernel synchronisation setup.
    ZF_LOGI("Setting up synchronisation primitives\n");
    enable_std_locking();

    /* Start the user application */
    ZF_LOGI("Start first process\n");
    spawn(&start_first_process, NULL, SOS_THREAD_BADGE, false);

    ZF_LOGI("\nSOS entering syscall loop\n");
    syscall_loop(ipc_ep);
}

/*
 * Main entry point - called by crt.
 */
int main(void)
{
    init_muslc();

    /* register the location of the unwind_tables -- this is required for
     * backtrace() to work */
    __register_frame(&__eh_frame_start);

    seL4_BootInfo *boot_info = sel4runtime_bootinfo();

    debug_print_bootinfo(boot_info);

    ZF_LOGI("\nSOS Starting...\n");

    NAME_THREAD(seL4_CapInitThreadTCB, "SOS:root");

    sched_ctrl_start = boot_info->schedcontrol.start;
    sched_ctrl_end = boot_info->schedcontrol.end;

    /* Initialise the cspace manager, ut manager and dma */
    sos_bootstrap(&cspace, boot_info);

    /* switch to the real uart to output (rather than seL4_DebugPutChar, which only works if the
     * kernel is built with support for printing, and is much slower, as each character print
     * goes via the kernel)
     *
     * NOTE we share this uart with the kernel when the kernel is in debug mode. */
    uart_init(&cspace);
    update_vputchar(uart_putchar);

    /* test print */
    ZF_LOGI("SOS Started!\n");

    /* allocate a bigger stack and switch to it -- we'll also have a guard page, which makes it much
     * easier to detect stack overruns */
    seL4_Word vaddr = SOS_STACK;
    for (int i = 0; i < SOS_STACK_PAGES; i++) {
        seL4_CPtr frame_cap;
        ut_t *frame = alloc_retype(&frame_cap, seL4_ARM_SmallPageObject, seL4_PageBits);
        ZF_LOGF_IF(frame == NULL, "Failed to allocate stack page");
        seL4_Error err = map_frame(&cspace, frame_cap, seL4_CapInitThreadVSpace,
                                   vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes);
        ZF_LOGF_IFERR(err, "Failed to map stack");
        vaddr += PAGE_SIZE_4K;
    }

    utils_run_on_stack((void *) vaddr, main_continued, NULL);

    UNREACHABLE();
}


