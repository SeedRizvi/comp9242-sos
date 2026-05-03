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
/****************************************************************************
 *
 *      $Id:  $
 *
 *      Description: Simple milestone 0 test.
 *
 *      Author:         Godfrey van der Linden
 *      Original Author:    Ben Leslie
 *
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <sel4/sel4.h>
#include <sos.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <string.h>
#include <utils/page.h>
#include <stdlib.h>

#include <utils/page.h>

#define NBLOCKS 9
#define NPAGES_PER_BLOCK 28
#define TEST_ADDRESS 0x8000000000
char test_str[] = "Basic test string\n";

// Block a thread forever
// we do this by making an unimplemented system call.
static void thread_block(void)
{
    /* construct some info about the IPC message console_test will send
     * to sos -- it's 1 word long */
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 1);
    /* Set the first word in the message to 1 */
    seL4_SetMR(0, 1);
    /* Now send the ipc -- call will send the ipc, then block until a reply
     * message is received */
    seL4_Call(SOS_IPC_EP_CAP, tag);
    /* Currently SOS does not reply -- so we never come back here */
}

/* called from pt_test */
static void do_pt_test(char **buf)
{
    int i;

    /* set */
    for (int b = 0; b < NBLOCKS; b++) {
        for (int p = 0; p < NPAGES_PER_BLOCK; p++) {
          buf[b][p * PAGE_SIZE_4K] = p;
        }
    }

    /* check */
    for (int b = 0; b < NBLOCKS; b++) {
        for (int p = 0; p < NPAGES_PER_BLOCK; p++) {
          assert(buf[b][p * PAGE_SIZE_4K] == p);
        }
    }
}

static void pt_test( void )
{
    /* need a decent sized stack */
    char buf1[NBLOCKS][NPAGES_PER_BLOCK * PAGE_SIZE_4K];
    char *buf1_ptrs[NBLOCKS];
    char *buf2[NBLOCKS];

    /* check the stack is above phys mem */
    for (int b = 0; b < NBLOCKS; b++) {
        buf1_ptrs[b] = buf1[b];
    }
    assert((void *) buf1 > (void *) TEST_ADDRESS);

    /* stack test */
    do_pt_test(buf1_ptrs);

    /* heap test */
    for (int b = 0; b < NBLOCKS; b++) {
        buf2[b] = malloc(NPAGES_PER_BLOCK * PAGE_SIZE_4K);
        assert(buf2[b]);
    }
    do_pt_test(buf2);
    for (int b = 0; b < NBLOCKS; b++) {
        free(buf2[b]);
    }
}

char *ptr[24] = {NULL};

int main(void)
{
    
    printf("===========================\n");
    printf("Hello and welcome to main()\n");

    printf("I must've been spawned, thanks sosh!\n");
    printf("Exiting!\n");
    printf("===========================\n");
    // printf("====STARTING [0]====\n");
    // char temp_buf[5000];
    // assert(close(50) == -1);
    // assert(sos_read(50, temp_buf, 10) == -1);
    // assert(sos_write(50, temp_buf, 10) == -1);
    // printf("====ENDING [0]====\n\n");
    
    // // SIMPLE CONSOLE OUTPUT: Existing and new fd.
    // printf("====STARTING [1]====\n");
    // sos_write(1, test_str, strlen(test_str));
    // sos_write(2, test_str, strlen(test_str));
    // int console_out = open("console", O_WRONLY);
    // sos_write(console_out, test_str, strlen(test_str));
    // close(console_out);
    // sos_write(console_out, test_str, strlen(test_str));
    // printf("====ENDING [1]====\n\n");
    
    
    // printf("====STARTING [2]====\n");
    // char read_buf[5000];
    // char *malloc_buf = malloc(100);
    // if (malloc_buf == NULL) {
    //     printf("NO MALLOC BUF FOUND\n");
    // }
    // int console_in = open("console", O_RDONLY);
    
    // printf("INPUT (10 CAP): ");
    // int result = sos_read(console_in, read_buf, 10);
    // printf("RESULT: %u\n", result);
    // sos_write(1, read_buf, result);
    
    // printf("INPUT (10 CAP): ");
    // result = sos_read(0, read_buf, 10);
    // printf("RESULT: %u\n", result);
    // sos_write(1, read_buf, result);

    // printf("INPUT (10 CAP): ");
    // result = sos_read(console_in, malloc_buf, 10);
    // printf("RESULT: %u\n", result);
    // sos_write(1, malloc_buf, result);

    // printf("INPUT (10 CAP): ");
    // result = sos_read(0, malloc_buf, 10);
    // printf("RESULT: %u\n", result);
    // sos_write(1, malloc_buf, result);

    // printf("INPUT (5000 CAP): ");
    // result = sos_read(console_in, read_buf, 5000);
    // printf("RESULT: %u\n", result);
    // sos_write(1, read_buf, result);

    // printf("INPUT (5000 CAP): ");
    // result = sos_read(0, read_buf, 5000);
    // printf("RESULT: %u\n", result);
    // sos_write(1, read_buf, result);

    // close(console_out);
    // printf("====ENDING [2]====\n\n");

    // // FILE WRITING / READING TEST
    // printf("====STARTING [3]====\n");
    // int file_fd = open("test_file.txt", O_WRONLY);
    // printf("FD: %d\n", file_fd);
    // sos_write(file_fd, test_str, strlen(test_str));
    // sos_write(file_fd, test_str, strlen(test_str)); // File size should be 36 I believe after this.
    
    // printf("CLEARED WRITE\n");
    
    // char input_buf[100];
    // int file_fd_read = open("test_file.txt", O_RDONLY);
    // int ret = sos_read(file_fd_read, input_buf, 99);
    // printf("BYTES READ %d\n", ret);
    // input_buf[ret] = '\0';
    // printf("READ STRING: %s\n", input_buf);
    // close(file_fd);
    // close(file_fd_read);
    // printf("====ENDING [3]====\n\n");
    
    // // PT TEST
    // printf("====STARTING [4]====\n");
    // char buf1[NBLOCKS][NPAGES_PER_BLOCK * PAGE_SIZE_4K];
    // char *buf1_ptrs[NBLOCKS];
    // char *buf2[NBLOCKS];
    // for (int b = 0; b < NBLOCKS; b++) {
    //     buf1_ptrs[b] = buf1[b];
    // }
    // assert((void *) buf1 > (void *) TEST_ADDRESS);
    // do_pt_test(buf1_ptrs);
    // for (int b = 0; b < NBLOCKS; b++) {
    //     buf2[b] = malloc(NPAGES_PER_BLOCK * PAGE_SIZE_4K);
    //     assert(buf2[b]);
    // }
    // do_pt_test(buf2);
    // for (int b = 0; b < NBLOCKS; b++) {
    //     free(buf2[b]);
    // }
    // printf("====ENDING [4]====\n\n");

    // // BASIC THRASHING TEST
    // printf("====STARTING [5]====\n");
    // for (int i = 0; i < 25; i++) {
    //     // printf("I will make frame thrashing\n");
    //     char *test = malloc(4090);
    //     printf("%p\n", test);
    //     test[0] = 'N';
    //     test[1] = 'O';
    //     test[2] = '\0';
    //     ptr[i] = test;
    //     printf("%s\n", ptr[i]);
    // }
    // for (int i = 0; i < 25; i++) {
    //     printf("%s\n", ptr[i]);
    // }
    // printf("====ENDING [5]====\n");
    
    // sleep(2);
    
    // printf("Exiting main()\n");
    // printf("===========================\n");

    // thread_block();
    return 0;
}
