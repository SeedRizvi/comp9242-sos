#pragma once

#define NUM_NINE_BIT 512

#define OFFSET_MASK (unsigned long)(0xFFF)
#define PT_MASK (0x1FF000)        // L4
#define PD_MASK (0x3FE00000)      // L3
#define PUD_MASK (0x7FC0000000)   // L2
#define PGD_MASK (0xFF8000000000) // L1

#define PT_SHIFT 12
#define PD_SHIFT  (PT_SHIFT+9)
#define PUD_SHIFT (PD_SHIFT+9)
#define PGD_SHIFT (PUD_SHIFT+9)

#define PGD_IDX(vaddr) ((vaddr & PGD_MASK) >> PGD_SHIFT)
#define PUD_IDX(vaddr) ((vaddr & PUD_MASK) >> PUD_SHIFT)
#define PD_IDX(vaddr) ((vaddr & PD_MASK) >> PD_SHIFT)
#define PT_IDX(vaddr) ((vaddr & PT_MASK) >> PT_SHIFT)

#define HEADER_END_1PROC 0x00917000
#define HEAP_START HEADER_END_1PROC + PAGE_SIZE_4K
#define HEAP_END(n) (HEAP_START + n*PAGE_SIZE_4K) 

#define INVALID_FRAME 0

#include "frame_table.h"
#include <stdbool.h>
#include <elf/elf.h>
#include "utils.h"

typedef struct pt_entry_t {
    frame_ref_t frame;
    seL4_CPtr frame_cptr;
} pt_entry_t;

typedef struct pt_level {
    seL4_CPtr ips;
    ut_t * ips_ut;
    struct pt_level *next;
} pt_level_t;

// NOTE: While concurrency constructs are initialised, PT by itself is not thread safe.
// Currently it must be synchronised appropriately wherever it is modified, using pt_lock
// and pt_unlock.
typedef struct pt_meta {
    int pid;                    // Pid of process this page table belongs to.

    pt_entry_t ****page_table;  // Page table structure.
    uint64_t num_pages;         // Number of pages in heap.

    size_t total_pages;         // Total allocated unique pages over lifetime.

    pt_level_t root_level;      // Root of intermediate structure tracking tree.

    seL4_CPtr *pgd;             // PGD cap (vspace).
    ut_t *pgd_ut;               // PGD ut.

    sync_recursive_mutex_t pt_rmutex;   // Synchronisation mutex for page table.

    uintptr_t code_start;       // Code start address.
    uintptr_t heap_start;       // Heap start address, same as code end.
} pt_meta_t;

// Lock / unlock the pagetable.
void pt_lock(pt_meta_t *pt);
void pt_unlock(pt_meta_t *pt);

// Tracks intermediate paging structures in sos_map_frame.
int pt_track_intermediate(pt_meta_t *pt, uintptr_t vaddr, seL4_CPtr ips, ut_t *ips_ut, int level);

/**
 * Handler for brk syscall. Optionally takes newbrk argument to determine if in heap region
 * @param Non-zero positive value for newbrk is valid, otherwise finds end of heap region. 
 * @returns new brk point on success, -1 otherwise
 */
seL4_MessageInfo_t handle_brk(int pid, uintptr_t newbrk);


// Initialise a new page table with provided attributes (elf file provided
// for determining heap and code memory ranges).
pt_meta_t *pt_init(int pid, seL4_CPtr *pgd, ut_t *pgd_ut, elf_t *elf_file);

/**
 * Free all memory associated with this page table
 */
void pt_destroy(pt_meta_t *pt);

// Remove a page table entry by either unmapping the associated frame
// if currently mapped or removing the associated entry from the pagefile.
void pt_rm_pf_entries(pt_entry_t *pt_del);

/**
 * Attempts to get Page Table entry for the given virtual address. Will
 * leave the entire page table under mutual exclusion until freed with pt_unlock.
 * @returns "frame reference" on success, INVALID_FRAME otherwise
 */
pt_entry_t *get_pt_entry(pt_meta_t *pt, uintptr_t vaddr);

// Gets the frame and frame cap respectively from a given pt_entry_t.
frame_ref_t get_pt_frame(pt_entry_t *pt_entry);
seL4_CPtr get_pt_cap(pt_entry_t *pt_entry);

/**
 * Adds relevant entry to page table for given vaddr. Mallocs where necessary.
 * Returns 0 on failure.
 */
int add_pt_entry(pt_meta_t *pt, uintptr_t vaddr, frame_ref_t frame, seL4_CPtr frame_cptr);

/**
 * Needed for demand paging. Updates frame_ref at vaddr to be "frame" instead.
 * *frame* is encoded with pagefile status and index. 
 */
void mod_pt_entry(pt_meta_t *pt, uintptr_t vaddr, frame_ref_t frame);

/**
 * Given an elf file, returns the vaddr directly after the code segment for heap allocation.
 */
uintptr_t get_code_start(elf_t *elf_file);
uintptr_t get_heap_start(elf_t *elf_file);
uintptr_t get_heap_end(pt_meta_t *pt);

// Checks if vaddr is in valid memory range for this process' page table / page table.
// to indicates valid to write to, from indicates valid to read from.
// NOTE: Currently this differentiation is not used.
bool valid_mem_range_to(int pid, uintptr_t vaddr);
bool valid_mem_range_from(int pid, uintptr_t vaddr);
bool valid_mem_range_to_pt(pt_meta_t *pt, uintptr_t vaddr);
bool valid_mem_range_from_pt(pt_meta_t *pt, uintptr_t vaddr);

typedef struct vm_fault_args {
    uintptr_t vaddr;
} vm_fault_args_t;
void *new_vm_fault_args(void *data);
void free_vm_fault_args(void *data);

// Unpin a frame at provided vaddr from memory for process with pid pid.
void vm_unpin(int pid, uintptr_t vaddr);

/**
 * Logic for handling vm_faults and pinning frames to memory. Cases:
 * 1. If a frame is still valid in our page table, then it must have been
 *    unmapped by the frame replacement clock algorithm, and will be remapped.
 * 2. If a frame isn't mapped in the hardware paging structures, a new frame 
 *    will be created and mapped. 
 * 3. If a frame isn't mapped in the hardware paging structures but is stored 
 *    on the page file, indicated by the ON_PF_MASK, then the remaining bytes of 
 *    frame_ref will instead be an index into the pagefile and a new frame will 
 *    be mapped into which data from the appropriate pagefile entry will be read.
 * 4. If the vaddr provided is invalid, then do not map the frame and delete the
 *    calling process.
 * 
 * @param 'pid' pid of process page table to map to.
 * @param 'vaddr' vaddr to map.
 * @param 'pin' whether or not to pin the frame to memory on mapping.
 * @returns the frame ref of the mapped frame, or INVALID_FRAME on failure.
 */
frame_ref_t vm_fault_logic(int pid, uintptr_t vaddr, bool pin);
/**
 * Handler for VM_faults. Threaded function that takes pass_data_t as its argument
 * , containing vm_fault_args_t. Called like other normal system call threads.
 */
void handle_vm_fault(void *data);