#include <stdio.h>
#include <stdlib.h>
#include <sos.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

#define STATIC_STR "3) RWLocks are so cool and I am totally unbiased"
#define BUF_SIZE 1299

int main(void) {
    printf("===========================\n");
    printf("Welcome to rust_fan.c!\n");
    printf("Why do we love rust?\n");
    printf("1) Concurrency made easy\n");
    printf("2) Concurrency made easy\n");
    printf("and finally... (I am mallocing!)\n");
    char *my_string = malloc(BUF_SIZE);
    memcpy(my_string, STATIC_STR, strlen(STATIC_STR));
    printf("quick, check the pagefile! (3 seconds)\n");
    sleep(3);
    printf("%s\n", my_string);
    // printf("%s\n", STATIC_STR);
    printf("Goodbye, World!\n");
    printf("===========================\n");
}