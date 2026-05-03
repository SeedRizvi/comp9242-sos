#pragma once

#define ELF_FILE_FD 3
#define ELF_HEADER_OFFSET 0

void *elf_read(int pid, size_t offset, size_t nbyte);