#include "vmem.h"
#include "vmem_layout.h"
#include "filesystem.h"
#include "pagefile.h"
#include "serverside.h"
#include "process.h"

#include <assert.h>

#define INVALID_FRAME 0


// =======================
// ==== PT Management ====
// =======================
void pt_lock(pt_meta_t *pt) {
    ZF_LOGD("PT Wait Start");
    rmutex_lock(&pt->pt_rmutex);
    ZF_LOGD("PT Wait End");
}

void pt_unlock(pt_meta_t *pt) {
    ZF_LOGD("PT Signal");
    rmutex_unlock(&pt->pt_rmutex);
}

int pt_track_intermediate(pt_meta_t *pt, uintptr_t vaddr, seL4_CPtr ips, ut_t *ips_ut, int level) {
    uint16_t l1_idx = PGD_IDX(vaddr);
    uint16_t l2_idx = PUD_IDX(vaddr);
    uint16_t l3_idx = PD_IDX(vaddr);

    if (pt->root_level.next == NULL) {
        pt->root_level.next = malloc(sizeof(pt_level_t) * NUM_NINE_BIT);
        if (pt->root_level.next == NULL) {
            ZF_LOGE("Error: could not allocate intermediate tracking\n");
            return -1;
        }
        for (int i = 0; i < NUM_NINE_BIT; i++) {
            pt->root_level.next[i].ips = seL4_CapNull;
            pt->root_level.next[i].ips_ut = NULL;
            pt->root_level.next[i].next = NULL;
        }
    }
    if (level == 1) {
        pt->root_level.next[l1_idx].ips = ips;
        pt->root_level.next[l1_idx].ips_ut = ips_ut;
        return 0;
    }

    if (pt->root_level.next[l1_idx].next == NULL) {
        pt->root_level.next[l1_idx].next = malloc(sizeof(pt_level_t) * NUM_NINE_BIT);
        if (pt->root_level.next[l1_idx].next == NULL) {
            ZF_LOGE("Error: could not allocate intermediate tracking\n");
            return -1;
        }
        for (int i = 0; i < NUM_NINE_BIT; i++) {
            pt->root_level.next[l1_idx].next[i].ips = seL4_CapNull;
            pt->root_level.next[l1_idx].next[i].ips_ut = NULL;
            pt->root_level.next[l1_idx].next[i].next = NULL;
        } 
    }
    if (level == 2) {
        pt->root_level.next[l1_idx].next[l2_idx].ips = ips;
        pt->root_level.next[l1_idx].next[l2_idx].ips_ut = ips_ut;
        return 0;
    }

    if (pt->root_level.next[l1_idx].next[l2_idx].next == NULL) {
        pt->root_level.next[l1_idx].next[l2_idx].next = malloc(sizeof(pt_level_t) * NUM_NINE_BIT);
        if (pt->root_level.next[l1_idx].next[l2_idx].next == NULL) {
            ZF_LOGE("Error: could not allocate intermediate tracking\n");
            return -1;
        }
        for (int i = 0; i < NUM_NINE_BIT; i++) {
            pt->root_level.next[l1_idx].next[l2_idx].next[i].ips = seL4_CapNull;
            pt->root_level.next[l1_idx].next[l2_idx].next[i].ips_ut = NULL; 
            pt->root_level.next[l1_idx].next[l2_idx].next[i].next = NULL;
        }
    }
    if (level == 3) {
        pt->root_level.next[l1_idx].next[l2_idx].next[l3_idx].ips = ips;
        pt->root_level.next[l1_idx].next[l2_idx].next[l3_idx].ips_ut = ips_ut;
        return 0;
    }

    return -1;
}

pt_meta_t *pt_init(int pid, seL4_CPtr *pgd, ut_t *pgd_ut, elf_t *elf_file) {
    ZF_LOGI("Creating new page table\n");
    pt_meta_t *pt = malloc(sizeof(pt_meta_t));
    if (pt == NULL) {
        ZF_LOGE("Error: could not allocate memory for new page table metadata struct\n");
        return NULL;
    }

    pt->root_level.ips = seL4_CapNull;
    pt->root_level.ips_ut = NULL;
    pt->root_level.next = NULL;

    pt->pgd = pgd;
    pt->pid = pid;
    pt->pgd_ut = pgd_ut;
    if (init_rmutex(&pt->pt_rmutex)) {
        ZF_LOGE("!!! could not allocate mutex for synchronistaion of nfs resources !!!");
        free(pt);
        return NULL;
    }
    pt->page_table = malloc(NUM_NINE_BIT * sizeof(pt_entry_t ***));
    pt->heap_start = get_heap_start(elf_file) + PAGE_SIZE_4K;
    pt->code_start = get_code_start(elf_file);
    for (int i = 0; i < NUM_NINE_BIT; i++) {
        pt->page_table[i] = NULL;
    }
    pt->num_pages = 1;
    pt->total_pages = 0;
    
    return pt;
}

// Clears a pt_level_t node, freeing intermediate paging structure
// and allocated memory for next level.
void clear_intermediate_level(pt_level_t *level) {
    if (level->ips != seL4_CapNull) {
        cspace_delete(&cspace, level->ips);
        cspace_free_slot(&cspace, level->ips);
    }
    if (level->ips_ut != NULL) {
        ut_free(level->ips_ut);
    }
    if (level->next != NULL) {
        free(level->next);
    }
}

// Destroys the intermediate paging structure tracking structure in this
// page table.
void pt_intermediate_destroy(pt_meta_t *pt) {
    if (pt->root_level.next == NULL) {
        return;
    }
    for (int i = 0; i < NUM_NINE_BIT; i++) {
        if (pt->root_level.next[i].next != NULL) {
            for (int j = 0; j < NUM_NINE_BIT; j++) {
                if (pt->root_level.next[i].next[j].next != NULL) {
                    for (int k = 0; k < NUM_NINE_BIT; k++) {
                        if (pt->root_level.next[i].next[j].next[k].ips != seL4_CapNull) seL4_ARM_PageTable_Unmap(pt->root_level.next[i].next[j].next[k].ips);
                        clear_intermediate_level(&pt->root_level.next[i].next[j].next[k]);
                    }
                    if (pt->root_level.next[i].next[j].ips != seL4_CapNull) seL4_ARM_PageDirectory_Unmap(pt->root_level.next[i].next[j].ips);
                    clear_intermediate_level(&pt->root_level.next[i].next[j]);
                }
            }
            if (pt->root_level.next[i].ips != seL4_CapNull) seL4_ARM_PageUpperDirectory_Unmap(pt->root_level.next[i].ips);
            clear_intermediate_level(&pt->root_level.next[i]);
        }
    }
    clear_intermediate_level(&pt->root_level);
}

void pt_destroy(pt_meta_t *pt) {
    ZF_LOGI("Destroying page table\n");
    for (int i = 0; i < NUM_NINE_BIT; i++) {
        if (pt->page_table[i] != NULL) {
            for (int j = 0; j < NUM_NINE_BIT; j++) {
                if (pt->page_table[i][j] != NULL) {
                    for (int k = 0; k < NUM_NINE_BIT; k++) {
                        if (pt->page_table[i][j][k] != NULL) {
                            pt_rm_pf_entries(pt->page_table[i][j][k]);
                            free(pt->page_table[i][j][k]);
                        }
                    }
                    free(pt->page_table[i][j]);
                }
            }
            free(pt->page_table[i]);
        }
    }
    free(pt->page_table);
    remove_rmutex(&pt->pt_rmutex);
    pt_intermediate_destroy(pt);
    free(pt);
}

void pt_rm_pf_entries(pt_entry_t *pt_del) {
    for (int i = 0; i < NUM_NINE_BIT; i++) {
        ft_lock();
        if (pt_del[i].frame & ON_PF_MASK) {
            fetch_frame_slot(NULL_FRAME, pt_del[i].frame);
        } else if (pt_del[i].frame != INVALID_FRAME) {
            frame_unpin(pt_del[i].frame);
            remove_frame_from_table(pt_del[i].frame, false);
        }
        ft_unlock();
    }
}

frame_ref_t get_pt_frame(pt_entry_t *pt_entry) {
    if (pt_entry == NULL) {
        return INVALID_FRAME;
    }
    return pt_entry->frame;
}

seL4_CPtr get_pt_cap(pt_entry_t *pt_entry) {
    if (pt_entry == NULL) {
        return seL4_CapNull;
    }
    return pt_entry->frame_cptr;
}

pt_entry_t *get_pt_entry(pt_meta_t *pt, uintptr_t vaddr) {
    uint16_t l1_idx = PGD_IDX(vaddr);
    if (pt->page_table[l1_idx] != NULL) {

        uint16_t l2_idx = PUD_IDX(vaddr);
        if (pt->page_table[l1_idx][l2_idx] != NULL) {

            uint16_t l3_idx = PD_IDX(vaddr);
            if (pt->page_table[l1_idx][l2_idx][l3_idx] != NULL) {

                uint16_t l4_idx = PT_IDX(vaddr);
                if (pt->page_table[l1_idx][l2_idx][l3_idx][l4_idx].frame != INVALID_FRAME) {
                    return &(pt->page_table[l1_idx][l2_idx][l3_idx][l4_idx]);
                }
            }
        }
    }
    return INVALID_FRAME;
}

/**
 * Will free up to 'lvl'th layer of page table for specific vaddr, where lvl = [1,3]. That is, the
 * highest level PT (aka PGD) will never be free'd in this function.
 */
void free_nth_level_pt(pt_meta_t *pt, uintptr_t vaddr, uint8_t lvl) {
    if (lvl >= 1) {
        if (lvl >= 2) {
            if (lvl >= 3) {
                free(pt->page_table[PGD_IDX(vaddr)][PUD_IDX(vaddr)][PD_IDX(vaddr)]);
            }
            free(pt->page_table[PGD_IDX(vaddr)][PUD_IDX(vaddr)]);
        }
        free(pt->page_table[PGD_IDX(vaddr)]);
    }
}

/**
 * Initialises all memory at pointer to have value "INVALID_FRAME"
 */
void zero_init_l4_pt(pt_entry_t *l4_pt) {
    for (int i = 0; i < NUM_NINE_BIT; i++) {
        l4_pt[i].frame = INVALID_FRAME;
        l4_pt[i].frame_cptr = 0;
    }
}

int add_pt_entry(pt_meta_t *pt, uintptr_t vaddr, frame_ref_t frame, seL4_CPtr frame_cptr) {
    uint16_t l1_idx = PGD_IDX(vaddr);
    uint16_t l2_idx = PUD_IDX(vaddr);
    uint16_t l3_idx = PD_IDX(vaddr);
    uint16_t l4_idx = PT_IDX(vaddr);

    if (pt->page_table[l1_idx] == NULL) {
        pt->page_table[l1_idx] = malloc(NUM_NINE_BIT * sizeof(pt_entry_t **));
        if (pt->page_table[l1_idx] == NULL) {
            ZF_LOGE("Error: could not allocate l1 memory for page table entry\n");
            free_nth_level_pt(pt, vaddr, 1);
            return 1;
        }
        memset((char *)(pt->page_table[l1_idx]), 0, NUM_NINE_BIT * sizeof(pt_entry_t **));
    }

    if (pt->page_table[l1_idx][l2_idx] == NULL) {
        pt->page_table[l1_idx][l2_idx] = malloc(NUM_NINE_BIT * sizeof(pt_entry_t *));
        if (pt->page_table[l1_idx][l2_idx] == NULL) {
            ZF_LOGE("Error: could not allocate l2 memory for page table entry\n");
            free_nth_level_pt(pt, vaddr, 2);
            return 1;
        }
        memset((char *)(pt->page_table[l1_idx][l2_idx]), 0, NUM_NINE_BIT * sizeof(pt_entry_t *));
    }

    if (pt->page_table[l1_idx][l2_idx][l3_idx] == NULL) {
        pt->page_table[l1_idx][l2_idx][l3_idx] = malloc(NUM_NINE_BIT * sizeof(pt_entry_t));
        if (pt->page_table[l1_idx][l2_idx][l3_idx] == NULL) {
            ZF_LOGE("Error: could not allocate l3 memory for page table entry\n");
            free_nth_level_pt(pt, vaddr, 3);
            return 1;
        }
        memset((char *)(pt->page_table[l1_idx][l2_idx][l3_idx]), 0, NUM_NINE_BIT * sizeof(pt_entry_t));
        zero_init_l4_pt(pt->page_table[l1_idx][l2_idx][l3_idx]);
    }

    if (pt->page_table[l1_idx][l2_idx][l3_idx][l4_idx].frame == INVALID_FRAME) {
        pt->total_pages++;
    }

    pt->page_table[l1_idx][l2_idx][l3_idx][l4_idx].frame = frame;
    pt->page_table[l1_idx][l2_idx][l3_idx][l4_idx].frame_cptr = frame_cptr;
    return 0;
}

void mod_pt_entry(pt_meta_t *pt, uintptr_t vaddr, frame_ref_t frame) {
    // If we are here, then page is being kicked from memory
    // So it *should* already be in PT
    uint16_t l1_idx = PGD_IDX(vaddr);
    if (pt->page_table[l1_idx] != NULL) {

        uint16_t l2_idx = PUD_IDX(vaddr);
        if (pt->page_table[l1_idx][l2_idx] != NULL) {

            uint16_t l3_idx = PD_IDX(vaddr);
            if (pt->page_table[l1_idx][l2_idx][l3_idx] != NULL) {

                uint16_t l4_idx = PT_IDX(vaddr);
                if (pt->page_table[l1_idx][l2_idx][l3_idx][l4_idx].frame != INVALID_FRAME) {
                    pt->page_table[l1_idx][l2_idx][l3_idx][l4_idx].frame = frame;
                }
            }
        }
    }
}


// =================================
// ==== Memory Range Management ====
// =================================
seL4_MessageInfo_t handle_brk(int pid, uintptr_t newbrk) {
    ZF_LOGD("Brk handler invoked with newbrk %lu\n", newbrk);
    pt_meta_t *pt = get_pt_from_pid(pid);

    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
    if (!newbrk) {
        newbrk = get_heap_end(pt);
    }
    else if (newbrk >= MAX_PROCESS_STACK_BOTTOM) {
        ZF_LOGV("Requested 'newbrk' exceeds HEAP Region\n");
        newbrk = get_heap_end(pt);
    }
    else if (newbrk > get_heap_end(pt)) {
        while (newbrk > get_heap_end(pt) && newbrk < MAX_PROCESS_STACK_BOTTOM) {
            pt->num_pages++;
        }
    }
    /* If neither of the conditions were true, then newbrk is already
    in heap region and just returned unchanged 
    */
    pt_unlock(pt);
    seL4_SetMR(0, newbrk);
    return reply;
}

uintptr_t get_code_start(elf_t *elf_file) {
    int loadable = 0;
    size_t ind = 0;
    for (; ind < elf_getNumProgramHeaders(elf_file); ind++) {
        if (elf_getProgramHeaderType(elf_file, ind) == PT_LOAD) {
            loadable = 1;
            break;
        }
    }
    if (loadable == 0) {
        ZF_LOGE("No loadable elf header provided\n");
        return 0;
    }
    return elf_getProgramHeaderVaddr(elf_file, ind);
}

uintptr_t get_heap_start(elf_t *elf_file) {
    int loadable = 0;
    size_t last_index = elf_getNumProgramHeaders(elf_file) - 1;
    for (; last_index < elf_getNumProgramHeaders(elf_file); last_index--) {
        if (elf_getProgramHeaderType(elf_file, last_index) == PT_LOAD) {
            loadable = 1;
            break;
        }
    }
    if (loadable == 0) {
        ZF_LOGE("No loadable elf header provided\n");
        return 0;
    }
    uintptr_t start_vaddr = elf_getProgramHeaderVaddr(elf_file, last_index);
    size_t segment_size = elf_getProgramHeaderMemorySize(elf_file, last_index);

    return ROUND_UP(segment_size + start_vaddr, PAGE_SIZE_4K);
}

uintptr_t get_heap_end(pt_meta_t *pt) {
    uintptr_t ret = (pt->heap_start + (pt->num_pages * PAGE_SIZE_4K));
    return ret;
}

bool valid_mem_range_to(int pid, uintptr_t vaddr) {
    pt_meta_t *pt = get_pt_from_pid(pid);

    bool in_heap = vaddr >= pt->heap_start && vaddr < get_heap_end(pt);
    bool in_stack = vaddr > MAX_PROCESS_STACK_BOTTOM && vaddr <= PROCESS_STACK_TOP;

    pt_unlock(pt);
    return in_heap || in_stack;
}

bool valid_mem_range_from(int pid, uintptr_t vaddr) {
    pt_meta_t *pt = get_pt_from_pid(pid);

    bool in_code = vaddr < pt->heap_start && vaddr >= pt->code_start;

    pt_unlock(pt);
    return valid_mem_range_to_pt(pt, vaddr) || in_code;
}

bool valid_mem_range_to_pt(pt_meta_t *pt, uintptr_t vaddr) {
    bool in_heap = vaddr >= pt->heap_start && vaddr < get_heap_end(pt);
    bool in_stack = vaddr > MAX_PROCESS_STACK_BOTTOM && vaddr <= PROCESS_STACK_TOP;
    return in_heap || in_stack;
}

bool valid_mem_range_from_pt(pt_meta_t *pt, uintptr_t vaddr) {
    bool in_code = vaddr < pt->heap_start && vaddr >= pt->code_start;
    return valid_mem_range_to_pt(pt, vaddr) || in_code;
}


// ========================
// ==== VM Fault Logic ====
// ========================
void *new_vm_fault_args(void *data) {
    uintptr_t vaddr = *((uintptr_t *)data);
    vm_fault_args_t *ret = malloc(sizeof(vm_fault_args_t));
    if (ret == NULL) {
        ZF_LOGE("Error: could not allocate memory for vm_fault_args_t\n");
        return NULL;
    }
    ret->vaddr = vaddr;
    return ret;
}

void free_vm_fault_args(void *data) {
    vm_fault_args_t *ret = (data);
    free(ret);
}

// Must not be called on a frame that has not been pinned prior.
void vm_unpin(int pid, uintptr_t vaddr) {
    pt_meta_t *pt = get_pt_from_pid(pid);
    frame_ref_t frame_ref = get_pt_frame(get_pt_entry(pt, vaddr));
    assert(frame_ref != INVALID_FRAME && !(frame_ref & ON_PF_MASK));
    frame_unpin(frame_ref);
    pt_unlock(pt);
}

frame_ref_t vm_fault_logic(int pid, uintptr_t vaddr, bool pin) {
    ZF_LOGD("VM Fault handler invoked! %p\n", vaddr);

    ft_lock();
    pt_meta_t *pt = get_pt_from_pid(pid);

    // Get frame.
    frame_ref_t frame_ref = get_pt_frame(get_pt_entry(pt, vaddr));

    // Frame is valid but unmapped (clock replacement strategy).
    if (frame_ref != INVALID_FRAME && !(frame_ref & ON_PF_MASK)) {
        int remap_err = remap_frame_ref_vaddr(pt, vaddr);
        if (remap_err != 0) {
            ZF_LOGE("Error: failed to remap frame %d\n", remap_err);
            ft_unlock();
            pt_unlock(pt);
            return INVALID_FRAME;
        }
        if (pin) {
            if (frame_pin(frame_ref)) {
                ZF_LOGE("Error: failed to pin frame\n");
                return INVALID_FRAME;
            }
        }
        ft_unlock();
        pt_unlock(pt);
        return frame_ref;
    }

    // Frame needs to be allocated.
    if (valid_mem_range_from_pt(pt, vaddr)) {
        seL4_CPtr temp_frame_cptr;

        if (alloc_map_frame(pt, *(pt->pgd), &temp_frame_cptr, vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes) != 0) {
            ZF_LOGE("Failed to allocate and map frame on vm fault\n");
            ft_unlock();
            pt_unlock(pt);
            return INVALID_FRAME;
        }

        // If page on pagefile, read in from pagefile into frmae.
        frame_ref_t new_frame_ref = get_pt_frame(get_pt_entry(pt, vaddr));
        if (frame_ref & ON_PF_MASK) {
            fetch_frame_slot(new_frame_ref, frame_ref);
        }
        if (pin) {
            if (frame_pin(new_frame_ref)) {
                ZF_LOGE("Error: failed to pin frame\n");
                return INVALID_FRAME;
            }
        }
        ft_unlock();
        pt_unlock(pt);
        return new_frame_ref;
    }

    ZF_LOGE("\tInvalid vaddr %p\n", vaddr);
    ft_unlock();
    pt_unlock(pt);
    return INVALID_FRAME;
}

void handle_vm_fault(void *data) {
    pass_data_t *pd = (data);
    vm_fault_args_t *vm_fault_args = (pd->data);

    if (vm_fault_logic(pd->pid, vm_fault_args->vaddr, false) == INVALID_FRAME) {
        proc_syscall_done(pd->pid);
        proc_delete_local(pd->pid);
        free_pass_data(pd);
        os_exit();
    } else {
        send_result(pd, 0, 0);
        ZF_LOGI("VM Fault Handled\n");
        os_exit();
    }
}