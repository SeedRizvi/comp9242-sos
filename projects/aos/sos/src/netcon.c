#include "netcon.h"
#include <string.h>
#include <cspace/cspace.h>
#include "ut.h"
#include <stdbool.h>
#include "serverside.h"
#include "utils.h"

#define INIT_READ_BUF 512

#define WRITE_MAX 1024

/* Network Console */
static struct network_console *netcon;

static seL4_CPtr nread_ntfn = 0;
static ut_t *nread_ntfn_ut = NULL;

static char *curr_frame = 0;
static size_t curr_bytes = 0;
static size_t read_bytes = 0;
static bool read_ready = false;
static seL4_CPtr rh_ntfn = 0;
static ut_t *rh_ntfn_ut = NULL;

static seL4_CPtr rf_ntfn = 0;
static ut_t *rf_ntfn_ut = NULL;

static bool frame_wait = false;

int init_netcon() {
    netcon = network_console_init();
    if(new_notification(&nread_ntfn, (void *)&nread_ntfn_ut, true) != 0) {
        return 1;
    }
    if(new_notification(&rh_ntfn, (void *)&rh_ntfn_ut, false) != 0) {
        return 1;
    }
    if(new_notification(&rf_ntfn, (void *)&rf_ntfn_ut, false) != 0) {
        return 1;
    }
    network_console_register_handler(netcon, &netcon_read_handler);
    return 0;
}

int safe_write_min(int len) {
    if (len <= WRITE_MAX) {
        return len;
    }
    return WRITE_MAX;
}

// Note: after about approximately page worth of data sent via separate network_console_send operations, output can be cut off.
// This appears to be an issue with network_console_send as the data we are providing it is appropriate and of small size (1024 bytes at a time max).
int netcon_write(char *str, int len) {
    ZF_LOGV("\n======== STARTING NETCON WRITE WITH LEN = %d ========\n", len);
    ZF_LOGV("ATTEMPTING TO WRITE %.*s\n", len, str);

    int num_output = network_console_send(netcon, str, safe_write_min(len));

    ZF_LOGV("NETCON WRITE TOTAL WROTE %d\n", num_output);

    while (num_output != len) {
        num_output += network_console_send(netcon, str + num_output, safe_write_min(len - num_output));
        ZF_LOGV("NETCON WRITE TOTAL WROTE %d\n", num_output);
    }

    ZF_LOGV("======== ENDING NETCON WRITE HAVING WRITTEN = %d ========\n\n", num_output);
    return num_output;
}

void term_read_lock() {
    seL4_Wait(nread_ntfn, NULL);
}

void term_read_unlock() {
    frame_wait = false;
    read_ready = false; // Read handler can start dropping bytes again.
    seL4_Signal(rh_ntfn);
    seL4_Signal(nread_ntfn);
}

size_t read_into_frame(char *frame, size_t nbyte) {
    curr_frame = frame;
    curr_bytes = nbyte;
    read_bytes = 0;

    frame_wait = false;
    read_ready = true;
    seL4_Signal(rh_ntfn);

    seL4_Wait(rf_ntfn, NULL); // Read handler done reading into frame.

    return read_bytes;
}

void netcon_read_handler(struct network_console *net, char c) {
    (void)net;
    if (frame_wait) seL4_Wait(rh_ntfn, NULL);
    if (read_ready && read_bytes < curr_bytes) {
        curr_frame[read_bytes++] = c;
        if (c == '\n') {
            frame_wait = false;
            read_ready = false;
            seL4_Signal(rf_ntfn);

        } else if (read_bytes == curr_bytes) {
            frame_wait = true;
            seL4_Signal(rf_ntfn);
            seL4_NBWait(rh_ntfn, NULL);
        }

    } else {
        frame_wait = false;
    }
}