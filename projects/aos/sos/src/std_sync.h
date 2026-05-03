#pragma once

// STRING FOR INCLUDE FROM LIB #include "../../sos/src/std_sync.h"
void cspace_ut_lock();
void cspace_ut_unlock();

void nfslib_lock();
void nfslib_unlock();

void brk_lock();
void brk_unlock();

void eth_lock();
void eth_unlock();

void pico_lock();
void pico_unlock();