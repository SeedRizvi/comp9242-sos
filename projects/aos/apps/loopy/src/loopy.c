#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <utils/time.h>
#include <syscalls.h>
/* Your OS header file */
#include <sos.h>

int main(void) {
    printf("===========================\n");
    printf("Welcome to loopy!\n");
    for (int i = 0; i < 10; i++) {
        sleep(2);
        printf("LOOP %d\n", i);
    }
    printf("Loopy over!\n");
    printf("===========================\n");
}