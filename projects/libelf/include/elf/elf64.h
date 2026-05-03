/*
 * Copyright (c) 1999-2004 University of New South Wales
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdlib.h>
#include <stdint.h>
#include <elf/elf.h>

#include "../../../aos/sos/src/elf_file.h"

typedef struct nfs_args nfs_args_t;

/* ELF header functions */
int elf64_checkFile(elf_t *elf);

int elf64_checkProgramHeaderTable(const elf_t *elf);

int elf64_checkSectionTable(const elf_t *elf);

static inline bool elf_isElf64(const elf_t *elf)
{
    return elf->elfClass == ELFCLASS64;
}

static inline Elf64_Ehdr elf64_getHeader(const elf_t *elf)
{
    return *(Elf64_Ehdr *) elf->elfFile;
}

static inline uintptr_t elf64_getEntryPoint(const elf_t *file)
{
    return elf64_getHeader(file).e_entry;
}


// =========================
// ==== ADDED FUNCTIONS ====
// =========================
static inline Elf64_Phdr *elf64_getProgramHeaderTableEntry(const elf_t *file, size_t ind)
{
    return (Elf64_Phdr *)elf_read(file->pid, elf64_getHeader(file).e_phoff + (sizeof(Elf64_Phdr) * ind), sizeof(Elf64_Phdr));
}

static inline Elf64_Shdr *elf64_getSectionTableEntry(const elf_t *file, size_t ind)
{
    return (Elf64_Shdr *)elf_read(file->pid, elf64_getHeader(file).e_shoff + (sizeof(Elf64_Shdr) * ind), sizeof(Elf64_Shdr));
}
// =========================
// =========================


static inline const Elf64_Phdr *elf64_getProgramHeaderTable(const elf_t *file)
{
    return file->elfFile + elf64_getHeader(file).e_phoff;
}

static inline const Elf64_Shdr *elf64_getSectionTable(const elf_t *file)
{
    return file->elfFile + elf64_getHeader(file).e_shoff;
}

static inline size_t elf64_getNumProgramHeaders(const elf_t *file)
{
    return elf64_getHeader(file).e_phnum;
}

static inline size_t elf64_getNumSections(const elf_t *elf)
{
    return elf64_getHeader(elf).e_shnum;
}

static inline size_t elf64_getSectionStringTableIndex(const elf_t *elf)
{
    return elf64_getHeader(elf).e_shstrndx;
}


/* Section header functions */
static inline size_t elf64_getSectionNameOffset(const elf_t *elf, size_t s)
{
    if (elf->read) {
        Elf64_Shdr *ste = elf64_getSectionTableEntry(elf, s);
        if (ste == NULL) {
            return NULL;
        }
        size_t ret = ste->sh_name;
        free(ste);
        return ret;
    }
    return elf64_getSectionTable(elf)[s].sh_name;
}

static inline uint32_t elf64_getSectionType(const elf_t *file, size_t s)
{
    if (file->read) {
        Elf64_Shdr *ste = elf64_getSectionTableEntry(file, s);
        if (ste == NULL) {
            return NULL;
        }
        uint32_t ret = ste->sh_type;
        free(ste);
        return ret;
    }
    return elf64_getSectionTable(file)[s].sh_type;
}

static inline size_t elf64_getSectionFlags(const elf_t *file, size_t s)
{
    if (file->read) {
        Elf64_Shdr *ste = elf64_getSectionTableEntry(file, s);
        if (ste == NULL) {
            return NULL;
        }
        size_t ret = ste->sh_flags;
        free(ste);
        return ret;
    }
    return elf64_getSectionTable(file)[s].sh_flags;
}

static inline uintptr_t elf64_getSectionAddr(const elf_t *elf, size_t i)
{
    if (elf->read) {
        Elf64_Shdr *ste = elf64_getSectionTableEntry(elf, i);
        if (ste == NULL) {
            return NULL;
        }
        uintptr_t ret = ste->sh_addr;
        free(ste);
        return ret;
    }
    return elf64_getSectionTable(elf)[i].sh_addr;
}

static inline size_t elf64_getSectionOffset(const elf_t *elf, size_t i)
{
    if (elf->read) {
        Elf64_Shdr *ste = elf64_getSectionTableEntry(elf, i);
        if (ste == NULL) {
            return NULL;
        }
        size_t ret = ste->sh_offset;
        free(ste);
        return ret;
    }
    return elf64_getSectionTable(elf)[i].sh_offset;
}

static inline size_t elf64_getSectionSize(const elf_t *elf, size_t i)
{
    if (elf->read) {
        Elf64_Shdr *ste = elf64_getSectionTableEntry(elf, i);
        if (ste == NULL) {
            return NULL;
        }
        size_t ret = ste->sh_size;
        free(ste);
        return ret;
    }
    return elf64_getSectionTable(elf)[i].sh_size;
}

static inline uint32_t elf64_getSectionLink(const elf_t *elf, size_t i)
{
    if (elf->read) {
        Elf64_Shdr *ste = elf64_getSectionTableEntry(elf, i);
        if (ste == NULL) {
            return NULL;
        }
        uint32_t ret = ste->sh_link;
        free(ste);
        return ret;
    }
    return elf64_getSectionTable(elf)[i].sh_link;
}

static inline uint32_t elf64_getSectionInfo(const elf_t *elf, size_t i)
{
    if (elf->read) {
        Elf64_Shdr *ste = elf64_getSectionTableEntry(elf, i);
        if (ste == NULL) {
            return NULL;
        }
        uint32_t ret = ste->sh_info;
        free(ste);
        return ret;
    }
    return elf64_getSectionTable(elf)[i].sh_info;
}

static inline size_t elf64_getSectionAddrAlign(const elf_t *elf, size_t i)
{
    if (elf->read) {
        Elf64_Shdr *ste = elf64_getSectionTableEntry(elf, i);
        if (ste == NULL) {
            return NULL;
        }
        size_t ret = ste->sh_addralign;
        free(ste);
        return ret;
    }
    return elf64_getSectionTable(elf)[i].sh_addralign;
}

static inline size_t elf64_getSectionEntrySize(const elf_t *elf, size_t i)
{
    if (elf->read) {
        Elf64_Shdr *ste = elf64_getSectionTableEntry(elf, i);
        if (ste == NULL) {
            return NULL;
        }
        size_t ret = ste->sh_entsize;
        free(ste);
        return ret;
    }
    return elf64_getSectionTable(elf)[i].sh_entsize;
}


/* Program header functions */
static inline uint32_t elf64_getProgramHeaderType(const elf_t *file, size_t ph)
{
    if (file->read) {
        Elf64_Phdr *pte = elf64_getProgramHeaderTableEntry(file, ph);
        if (pte == NULL) {
            return NULL;
        }
        uint32_t ret = pte->p_type;
        free(pte);
        return ret;
    }
    return elf64_getProgramHeaderTable(file)[ph].p_type;
}

static inline size_t elf64_getProgramHeaderOffset(const elf_t *file, size_t ph)
{
    if (file->read) {
        Elf64_Phdr *pte = elf64_getProgramHeaderTableEntry(file, ph);
        if (pte == NULL) {
            return NULL;
        }
        size_t ret = pte->p_offset;
        free(pte);
        return ret;
    }
    return elf64_getProgramHeaderTable(file)[ph].p_offset;
}

static inline uintptr_t elf64_getProgramHeaderVaddr(const elf_t *file, size_t ph)
{
    if (file->read) {
        Elf64_Phdr *pte = elf64_getProgramHeaderTableEntry(file, ph);
        if (pte == NULL) {
            return NULL;
        }
        uintptr_t ret = pte->p_vaddr;
        free(pte);
        return ret;
    }
    return elf64_getProgramHeaderTable(file)[ph].p_vaddr;
}

static inline uintptr_t elf64_getProgramHeaderPaddr(const elf_t *file, size_t ph)
{
    if (file->read) {
        Elf64_Phdr *pte = elf64_getProgramHeaderTableEntry(file, ph);
        if (pte == NULL) {
            return NULL;
        }
        uintptr_t ret = pte->p_paddr;
        free(pte);
        return ret;
    }
    return elf64_getProgramHeaderTable(file)[ph].p_paddr;
}

static inline size_t elf64_getProgramHeaderFileSize(const elf_t *file, size_t ph)
{
    if (file->read) {
        Elf64_Phdr *pte = elf64_getProgramHeaderTableEntry(file, ph);
        if (pte == NULL) {
            return NULL;
        }
        size_t ret = pte->p_filesz;
        free(pte);
        return ret;
    }
    return elf64_getProgramHeaderTable(file)[ph].p_filesz;
}

static inline size_t elf64_getProgramHeaderMemorySize(const elf_t *file, size_t ph)
{
    if (file->read) {
        Elf64_Phdr *pte = elf64_getProgramHeaderTableEntry(file, ph);
        if (pte == NULL) {
            return NULL;
        }
        size_t ret = pte->p_memsz;
        free(pte);
        return ret;
    }
    return elf64_getProgramHeaderTable(file)[ph].p_memsz;
}

static inline uint32_t elf64_getProgramHeaderFlags(const elf_t *file, size_t ph)
{
    if (file->read) {
        Elf64_Phdr *pte = elf64_getProgramHeaderTableEntry(file, ph);
        if (pte == NULL) {
            return NULL;
        }
        uint32_t ret = pte->p_flags;
        free(pte);
        return ret;
    }
    return elf64_getProgramHeaderTable(file)[ph].p_flags;
}

static inline size_t elf64_getProgramHeaderAlign(const elf_t *file, size_t ph)
{
    if (file->read) {
        Elf64_Phdr *pte = elf64_getProgramHeaderTableEntry(file, ph);
        if (pte == NULL) {
            return NULL;
        }
        size_t ret = pte->p_align;
        free(pte);
        return ret;
    }
    return elf64_getProgramHeaderTable(file)[ph].p_align;
}