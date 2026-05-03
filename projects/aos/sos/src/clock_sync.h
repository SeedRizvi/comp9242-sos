#pragma once

// Implemented in utils.c and used for clock driver synchronisation.
int init_clock_sync();
void clock_signal();
void clock_wait();

int init_clock_test_sync();
void destroy_clock_test_sync();
void clock_test_signal();
void clock_test_wait();