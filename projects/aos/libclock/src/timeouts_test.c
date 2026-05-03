#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <clock/clock.h>
#include <clock/timeouts.h>
#include <assert.h>

#define DUMMY_EXPIRY_PRE 4000000
#define DUMMY_EXPIRY 5000000
#define DUMMY_EXPIRY_POST 6000000

static int dummy_data1 = 1;
static int dummy_data2 = 2;

static int last_seen = 0;
void dummy_callback(uint32_t id, void* data) {
    int *num = (data);
    last_seen = *num;
}

// Multiple inserts and removals.
void timeouts_test1() {
    timeouts t;
    init_timeouts(&t);

    insert_timeout(&(t), DUMMY_EXPIRY, &dummy_callback, &dummy_data1);
    assert(*fetch_timeout(&t, 0) == 1);
    assert(fetch_timeout(&t, 1) == NULL);

    insert_timeout(&(t), DUMMY_EXPIRY_PRE, &dummy_callback, &dummy_data1);
    assert(*fetch_timeout(&t, 0) == 2);
    assert(*fetch_timeout(&t, 1) == 1);
    assert(fetch_timeout(&t, 2) == NULL);

    insert_timeout(&(t), DUMMY_EXPIRY_POST, &dummy_callback, &dummy_data1);
    assert(*fetch_timeout(&t, 0) == 2);
    assert(*fetch_timeout(&t, 1) == 1);
    assert(*fetch_timeout(&t, 2) == 3);
    assert(fetch_timeout(&t, 3) == NULL);
    assert(peek_expiry(&(t)) == DUMMY_EXPIRY_PRE);

    assert(remove_timeout(&t, 1, true) == 0);
    assert(*fetch_timeout(&t, 0) == 2);
    assert(*fetch_timeout(&t, 1) == 3);
    assert(fetch_timeout(&t, 2) == NULL);
    assert(peek_expiry(&(t)) == DUMMY_EXPIRY_PRE);

    assert(remove_timeout(&t, 3, true) == 0);
    assert(*fetch_timeout(&t, 0) == 2);
    assert(fetch_timeout(&t, 1) == NULL);
    assert(peek_expiry(&(t)) == DUMMY_EXPIRY_PRE);

    assert(remove_timeout(&t, 2, true) == 0);
    assert(fetch_timeout(&t, 0) == NULL);

    destroy_timeouts(&t);
    assert(fetch_timeout(&t, 0) == NULL);
    dprintf(2, "timeouts_test1 passed\n");
}

// Timeout consumption tests.
void timeouts_test2() {
    timeouts t;
    init_timeouts(&t);

    insert_timeout(&(t), DUMMY_EXPIRY_PRE, &dummy_callback, &dummy_data1);
    insert_timeout(&(t), DUMMY_EXPIRY, &dummy_callback, &dummy_data2);
    consume_timeouts(&(t), DUMMY_EXPIRY_POST);
    assert(last_seen == 2);

    destroy_timeouts(&t);
    assert(fetch_timeout(&t, 0) == NULL);
    dprintf(2, "timeouts_test2 passed\n");
}

// Destroy and reinitialise.
void timeouts_test3() {
    timeouts t;

    init_timeouts(&t);
    insert_timeout(&(t), DUMMY_EXPIRY_POST, &dummy_callback, &dummy_data1);
    destroy_timeouts(&t);
    init_timeouts(&t);
    insert_timeout(&(t), DUMMY_EXPIRY_POST, &dummy_callback, &dummy_data1);
    assert(*fetch_timeout(&t, 0) == 1);

    dprintf(2, "timeouts_test3 passed\n");
}

void test_timeouts() {
    dprintf(2, "==TESTING_TIMEOUTS==\n");
    timeouts_test1();
    timeouts_test2();
    timeouts_test3();
    dprintf(2, "==TESTED_TIMEOUTS==\n");
}