#pragma once

#include <networkconsole/networkconsole.h>
#include <stdlib.h>

int init_netcon();
int netcon_write(char *str, int len);
void netcon_read_handler(struct network_console *net, char c);
void term_read_lock();
void term_read_unlock();
size_t read_into_frame(char *frame, size_t nbyte);