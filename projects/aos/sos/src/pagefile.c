#include <string.h>
#include "utils.h"
#include "pagefile.h"
#include "frame_table.h"
#include "filesystem.h"
#include "vmem.h"
#include "process.h"

/* Macros for reading Pagefile Meta data */
#define IS_VALID(meta_data) (meta_data[METADATA_REGION_SIZE-SIZEOF_VALID])
#define GET_PID(meta_data) ( ((uint16_t*)(meta_data))[4] )
#define GET_VADDR(meta_data) ( ((uintptr_t*)(meta_data))[0] )

/* Notes:
    num_pages will only support up to UINT32_MAX - 1 because
    highest bit is reserved for in file/memory bit.
    ---------------------------------------------------------------------
    Once defragmenting is implemented, we really shouldnt have
    to worry about it, but the "threat" is still there.
*/

uint32_t num_operations = 0;
uint32_t num_pages = 0;

int rm_frame_slot(int pid, pt_meta_t *pt, frame_ref_t old_frame) {
    ZF_LOGD("Paging out frame\n");
    uintptr_t old_vaddr = frame_from_ref(old_frame)->vaddr;
    ZF_LOGV("Victim frame: Frame_ref: 0x%lx | vaddr: 0x%lx\n", old_frame, old_vaddr);
    if (old_frame & ON_PF_MASK) {
        ZF_LOGE("Frame marked as 'on-disk' cannot be written to pagefile\n");
    }

    char meta_data[METADATA_REGION_SIZE];
    (void) meta_data[4];
    new_metadata(meta_data, old_vaddr, true, pid);

    ZF_LOGD("Pagefile: writing metadata...\n");
    int err = fs_write_pagefile(ENTRY_SIZE * num_pages, meta_data, METADATA_REGION_SIZE);
    if (err != METADATA_REGION_SIZE) return 1;

    // Write frame contents
    char *phys_addr = (char *)frame_data(old_frame);
    ZF_LOGD("Pagefile: writing contents...\n");
    err = fs_write_pagefile(ENTRY_SIZE * num_pages + METADATA_REGION_SIZE, phys_addr, PAGE_SIZE_4K);
    if (err != PAGE_SIZE_4K) return 1;

    old_frame = 0;
    old_frame |= ON_PF_MASK;
    old_frame |= (num_pages++ & PF_IDX_MASK);
    ZF_LOGD("Updated frame in PT to: %lx\n", old_frame);

    // Update Page Table
    mod_pt_entry(pt, old_vaddr, old_frame);
    ZF_LOGD("Finished paging out frame\n");
    return 0;
}

int fetch_frame_slot(frame_ref_t dst, frame_ref_t src) {
    ZF_LOGD("Paging in frame\n");
    ZF_LOGD("\nFetch frame_ref from disk: 0x%lx \n", src);
    
    if (!(src & ON_PF_MASK)) {
        ZF_LOGE("Frame marked as 'on-memory' cannot be read from pagefile\n");
    }
    uint32_t index = (src & PF_IDX_MASK);
    
    if (dst != NULL_FRAME) {
        char *phys_addr = (char *)frame_data(dst);
        ZF_LOGD("Reading page contents from pagefile...\n");
        int err = fs_read_pagefile(ENTRY_SIZE * index + METADATA_REGION_SIZE, phys_addr, PAGE_SIZE_4K);
        if (err != PAGE_SIZE_4K) return 1;
    } 
    
    defragment(index);
    ZF_LOGD("Finished paging out frame\n");
    return 0;
}

void new_metadata(char *mem, uintptr_t vaddr, bool valid, uint16_t pid) {
    *(uintptr_t *)mem = vaddr;
    ((uint16_t *)(mem))[4] = pid;
    mem[METADATA_REGION_SIZE-SIZEOF_VALID] = (uint8_t)(valid) ? 0xF : 0;
}

void defragment(uint32_t index) {
    ZF_LOGV("Pagefile defragment at index %u\n", index);
    
    if (index == num_pages - 1) {
        ZF_LOGD("Defragment edge case\n");
        pf_truncate(ENTRY_SIZE * --num_pages);
        return; 
    }

    char page_contents[ENTRY_SIZE];
    fs_read_pagefile(ENTRY_SIZE * (--num_pages), page_contents, ENTRY_SIZE);
    
    uintptr_t vaddr = GET_VADDR(page_contents);
    int pid = GET_PID(page_contents);
    fs_write_pagefile(ENTRY_SIZE * index, page_contents, ENTRY_SIZE);

    pt_meta_t *pt = get_pt_from_pid(pid);
    frame_ref_t frame = get_pt_frame(get_pt_entry(pt, vaddr));
    frame = 0 | (PF_IDX_MASK & index) | ON_PF_MASK;
    
    // Update PT entry which was referring to tail to now contain new index
    mod_pt_entry(pt, vaddr, frame);
    pt_unlock(pt);

    pf_truncate(ENTRY_SIZE * num_pages);
}