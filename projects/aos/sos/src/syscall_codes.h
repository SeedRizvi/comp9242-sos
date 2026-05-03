#pragma once

#define SOS_SYSCALL_DUMMY   0
/* Syscall 1 Unused */
#define SOS_SYSCALL_RD   2
#define SOS_SYSCALL_WR   3
#define SOS_SYSCALL_GETDIRENT   4
#define SOS_SYSCALL_STAT        5
#define SOS_SYSCALL_PROC_CREATE 6
#define SOS_SYSCALL_PROC_DEL    7
#define SOS_SYSCALL_PROC_ID     8
#define SOS_SYSCALL_PROC_STATUS 9
#define SOS_SYSCALL_PROC_WAIT   10
#define SOS_SYSCALL_SLEEP       11
#define SOS_SYSCALL_TIMESTAMP   12
#define SOS_SYSCALL_BRK         13
#define SOS_SYSCALL_NEWBRK      14
#define SOS_SYSCALL_OPEN        15
#define SOS_SYSCALL_CLOSE       16
#define SOS_SYSCALL_EXIT_OS_THREAD 17

/* Unsupported but included for completeness */
#define SOS_SYSCALL_MMAP2       222