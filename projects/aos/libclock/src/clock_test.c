#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <clock/clock.h>
#include <clock/timeouts.h>
#include <assert.h>
#include "../../sos/src/clock_sync.h"

#define RECURRENT_CLOCK_WAIT_TEST 1000000

int global_test_value = 1;

void clock_test_callback(uint32_t id, void* data) {
    int *value = (data);
    dprintf(2, "[%lu]\tclock_test_callback occured on ID: %lu with data %u\n", get_time(), id, *value);
    clock_test_signal();
    *value = 0;
}

void clock_test_callback_alt(uint32_t id, void* data) {
    int value = *((int *)data);
    dprintf(2, "[%lu]\tclock_test_callback occured on ID: %lu with data %u\n", get_time(), id, value);
}

void clock_test_callback_rec(uint32_t id, void* data) {
    int value = *((int *)data);
    dprintf(2, "[%lu]\tclock_test_callback occured on ID: %lu with data %u\n", get_time(), id, value);
    register_timer(RECURRENT_CLOCK_WAIT_TEST, &clock_test_callback_rec, &global_test_value);
}

void clock_test1() {
    int test_value = -1;
    register_timer(1000000, &clock_test_callback, &test_value);
    assert(test_value == -1);
    clock_test_wait();
    assert(test_value == 0);

    int one = 1;
    int two = 2;
    int three = 3;
    register_timer(2000000, &clock_test_callback, &two);
    register_timer(1000000, &clock_test_callback, &one);
    register_timer(3000000, &clock_test_callback, &three);
    clock_test_wait();
    assert(one == 0);
    assert(two == 2);
    assert(three == 3);
    clock_test_wait();
    assert(one == 0);
    assert(two == 0);
    assert(three == 3);
    clock_test_wait();
    assert(one == 0);
    assert(two == 0);
    assert(three == 0);
    dprintf(2, "clock_test1 passed\n");
}

void clock_test_max() {
    dprintf(2, "clock_test_max start\n");
    for (int i = 0; i < CONC_MAX_TIMEOUTS - 2; i++) {
        dprintf(2, "clock_test_max register\n");
        dprintf(2, "TIMER REGISTERED = %d\n", register_timer(1000000, &clock_test_callback_alt, &global_test_value));
    }
    dprintf(2, "TIMER REGISTERED = %d\n", register_timer(2000000, &clock_test_callback, &global_test_value));
    clock_test_wait();
    dprintf(2, "clock_test_max passed\n");
}

void spawn_recurrent() {
    register_timer(RECURRENT_CLOCK_WAIT_TEST, &clock_test_callback_rec, &global_test_value);
}

void test_clock_long() {
    int test_value = 1;
    register_timer(80000000, &clock_test_callback, &test_value);
    assert(test_value == 1);
    clock_test_wait();
    assert(test_value == 0);
    dprintf(2, "clock_test_long passed\n");
}

// Assumed to be run after clock has been initialised.
void test_clock() {
    // Test timeouts data structure.
    test_timeouts();

    dprintf(2, "==TESTING CLOCK==\n");

    // Test clock functionality.
    init_clock_test_sync();
    clock_test_max();

    clock_test1();

    spawn_recurrent();

    test_clock_long();

    destroy_clock_test_sync();

    dprintf(2, "==TESTED CLOCK==\n\n");
}
