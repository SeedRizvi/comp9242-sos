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
#include <stdlib.h>
#include <stdint.h>
#include <clock/clock.h>
#include <clock/timeouts.h>

/* The functions in src/device.h should help you interact with the timer
 * to set registers and configure timeouts. */
#include "device.h"
#include "timeouts_test.c"

static struct {
    volatile meson_timer_reg_t *regs;
    timeouts timeouts;
    bool driver_init;
} clock;

void start_time() {
    consume_timeouts(&(clock.timeouts), get_time());
    
    // Get expiry.
    uint64_t expiry = peek_expiry(&(clock.timeouts));

    uint64_t curr_time = get_time();
    uint64_t delay = expiry - curr_time + 10;

    if (curr_time >= expiry || delay <= MIN_TIMER) {
        configure_timeout(clock.regs, MESON_TIMER_A, true, false, TIMEOUT_TIMEBASE_1_MS, MIN_TIMER);
        
    } else if (delay <= (MAX16 * 1000)) {
        configure_timeout(clock.regs, MESON_TIMER_A, true, false, TIMEOUT_TIMEBASE_1_MS, delay / 1000);

    } else {
        configure_timeout(clock.regs, MESON_TIMER_A, true, false, TIMEOUT_TIMEBASE_1_MS, MAX16);
    }
}

void pause_time() {
    configure_timeout(clock.regs, MESON_TIMER_A, false, false, TIMEOUT_TIMEBASE_1_MS, 0);
}

int start_timer(unsigned char *timer_vaddr)
{
    int err = stop_timer();
    if (err != 0) {
        return err;
    }

    clock.regs = (meson_timer_reg_t *)(timer_vaddr + TIMER_REG_START);

    // Initialise timeouts.
    if (clock.timeouts.reset == 1) {
        init_timeouts(&(clock.timeouts));
        clock.timeouts.reset = 1;
    } else {
        init_timeouts(&(clock.timeouts));
    }

    //! Set input clock rate of TIMER E as 10uS by altering bits 8-10 (inclusive)
    configure_timestamp(clock.regs, TIMESTAMP_TIMEBASE_1_US);

    clock.driver_init = true;
    return CLOCK_R_OK;
}

timestamp_t get_time(void) {
    return read_timestamp(clock.regs);
}

uint32_t register_timer(uint64_t delay, timer_callback_t callback, void *data)
{
    if (!clock.driver_init) {
        return CLOCK_R_UINT;
    }

    pause_time();
    
    timestamp_t curr_time = get_time();
    timestamp_t expiry = curr_time + delay;
    uint32_t ret = insert_timeout(&(clock.timeouts), expiry, callback, data);

    start_time();

    return ret;
}

int remove_timer(uint32_t id)
{
    if (!clock.driver_init) {
        return CLOCK_R_UINT;
    }

    pause_time();

    if (remove_timeout(&(clock.timeouts), id, true) == 0) {
        start_time();
        return CLOCK_R_OK;
    }

    start_time();
    return CLOCK_R_FAIL; 
}

int timer_irq(
    void *data,
    seL4_Word irq,
    seL4_IRQHandler irq_handler
)
{
    if (!clock.driver_init) {
        return CLOCK_R_UINT;
    }

    pause_time();

    /* Handle the IRQ */
    consume_timeouts(&(clock.timeouts), get_time());
    
    /* Acknowledge that the IRQ has been handled */
    seL4_IRQHandler_Ack(irq_handler);

    start_time();
    return CLOCK_R_OK;
}

int stop_timer(void)
{
    if (!clock.driver_init) {
        return CLOCK_R_OK;
    }

    /* Stop the timer from producing further interrupts and remove all existing timeouts */
    pause_time();
    clock.driver_init = false;
    destroy_timeouts(&(clock.timeouts));
    return CLOCK_R_OK;
}