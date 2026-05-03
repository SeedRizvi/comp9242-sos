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
#include "frame_table.h"
#include "mapping.h"
#include "vmem_layout.h"
#include "pagefile.h"
#include "vmem.h"
#include "utils.h"

#include <assert.h>
#include <string.h>
#include <stdbool.h>
#include <utils/util.h>
#include <sos/gen_config.h>

static int pin_count = 0;

/* Debugging macro to get the human-readable name of a particular list. */
#define LIST_NAME(list) LIST_ID_NAME(list->list_id)

/* Names of each of the lists. */
#define LIST_NAME_ENTRY(list) [list] = #list
char *frame_table_list_names[] = {
    LIST_NAME_ENTRY(NO_LIST),
    LIST_NAME_ENTRY(FREE_LIST),
    LIST_NAME_ENTRY(ALLOCATED_LIST),
};

/*
 * An entire page of data.
 */
typedef unsigned char frame_data_t[BIT(seL4_PageBits)];
compile_time_assert("Frame data size correct", sizeof(frame_data_t) == BIT(seL4_PageBits));

/* Memory-efficient doubly linked list of frames
 *
 * As all frame objects will live in effectively an array, we only need
 * to be able to index into that array.
 */
typedef struct {
    list_id_t list_id;
    /* Index of first element in list */
    frame_ref_t first;
    /* Index in last element of list */
    frame_ref_t last;
    /* Size of list (useful for debugging) */
    size_t length;
} frame_list_t;

/* This global variable tracks the frame table */
static struct {
    /* The array of all frames in memory. */
    frame_t *frames;
    /* The data region of the frame table. */
    frame_data_t *frame_data;
    /* The current capacity of the frame table. */
    size_t capacity;
    /* The current number of frames resident in the table. */
    size_t used;
    /* The current size of the frame table in bytes. */
    size_t byte_length;
    /* The free frames. */
    frame_list_t free;
    /* The allocated frames. */
    frame_list_t allocated;
    /* cspace used to make allocations of capabilities. */
    cspace_t *cspace;
    /* vspace used to map pages into SOS. */
    seL4_ARM_PageGlobalDirectory vspace;
} frame_table = {
    .frames = (void *)SOS_FRAME_TABLE,
    .frame_data = (void *)SOS_FRAME_DATA,
    .free = { .list_id = FREE_LIST },
    .allocated = { .list_id = ALLOCATED_LIST },
};

/* Management of frame nodes */
static frame_ref_t ref_from_frame(frame_t *frame);

/* Management of frame list */
static void push_front(frame_list_t *list, frame_t *frame);
static void push_back(frame_list_t *list, frame_t *frame);
static frame_t *pop_front(frame_list_t *list);
static void remove_frame(frame_list_t *list, frame_t *frame);

static sync_recursive_mutex_t ft_rmutex;

/*
 * Allocate a frame at a particular address in SOS.
 *
 * @param(in)  vaddr  Address in SOS at which to map the frame.
 * @return            Page used to map frame into SOS.
 */
static seL4_ARM_Page alloc_frame_at(uintptr_t vaddr);

/* Allocate a new frame. */
static frame_t *alloc_fresh_frame(pt_meta_t *pt);

/* Increase the capacity of the frame table.
 *
 * @return  0 on succuss, -ve on failure. */
static int bump_capacity(void);

void frame_table_init(cspace_t *cspace, seL4_CPtr vspace)
{
    frame_table.cspace = cspace;
    frame_table.vspace = vspace;

    if (init_rmutex(&ft_rmutex)) {
        ZF_LOGF("!!! could not allocate mutex for synchronistaion of frame table resources !!!");
    }
}

void ft_lock() {
    ZF_LOGD("FT Wait Start\n");
    rmutex_lock(&ft_rmutex);
    ZF_LOGD("FT Wait End\n");
}

void ft_unlock() {
    ZF_LOGD("FT Signal\n");
    rmutex_unlock(&ft_rmutex);
}

cspace_t *frame_table_cspace(void)
{
    return frame_table.cspace;
}

// Checks if frame reference bit is set or not. If it is, unset the reference bit
// and unmap the frame so that the reference bit can be reset next access.
static bool frame_ref_check(pt_meta_t *pt, frame_t *frame) {
    if (!frame->pin) {
        if (!frame->ref) {
            return true;
        } else {
            frame->ref = false;
            seL4_ARM_Page_Unmap(get_pt_cap(get_pt_entry(pt, frame->vaddr))); // Unmapping to properly trigger vm_fault to refcount next time.
        }
    }
    return false;
}

// Alloc frame, but also tracks a paget table pointer within the frame.
frame_ref_t alloc_frame_pt(pt_meta_t *pt, uintptr_t vaddr)
{
    assert(pt != NULL);

    frame_t *frame = pop_front(&frame_table.free);

    if (frame == NULL) {
        frame = alloc_fresh_frame(pt);
        if (frame == NULL) {
            frame = pop_front(&frame_table.free);
        }
    }

    ZF_LOGV("New frame allocated with vaddr: %lx\n", vaddr);
    if (frame != NULL) {
        frame->vaddr = vaddr;
        frame->ref = true;
        frame->pin = false;
        frame->pt = pt;
        push_back(&frame_table.allocated, frame);
    }
    return ref_from_frame(frame);
}

void free_frame(frame_ref_t frame_ref)
{
    if (frame_ref != NULL_FRAME) {
        frame_t *frame = frame_from_ref(frame_ref);

        remove_frame(&frame_table.allocated, frame);
        push_front(&frame_table.free, frame);
    }
}

seL4_ARM_Page frame_page(frame_ref_t frame_ref)
{
    frame_t *frame = frame_from_ref(frame_ref);
    return frame->sos_page;
}

unsigned char *frame_data(frame_ref_t frame_ref)
{
    assert(frame_ref != NULL_FRAME);
    assert(frame_ref < frame_table.capacity);
    return frame_table.frame_data[frame_ref];
}

frame_t *frame_from_ref(frame_ref_t frame_ref)
{
    assert(frame_ref != NULL_FRAME);
    assert(frame_ref < frame_table.capacity);
    return &frame_table.frames[frame_ref];
}

static frame_ref_t ref_from_frame(frame_t *frame)
{
    assert(frame >= frame_table.frames);
    assert(frame < frame_table.frames + frame_table.used);
    return frame - frame_table.frames;
}

static void push_front(frame_list_t *list, frame_t *frame)
{
    assert(frame != NULL);
    assert(frame->list_id == NO_LIST);
    assert(frame->next == NULL_FRAME);
    assert(frame->prev == NULL_FRAME);

    frame_ref_t frame_ref = ref_from_frame(frame);

    if (list->last == NULL_FRAME) {
        list->last = frame_ref;
    }

    frame->next = list->first;
    if (frame->next != NULL_FRAME) {
        frame_t *next = frame_from_ref(frame->next);
        next->prev = frame_ref;
    }

    list->first = frame_ref;
    list->length += 1;
    frame->list_id = list->list_id;

    ZF_LOGV("%s.length = %lu", LIST_NAME(list), list->length);
}

static void push_back(frame_list_t *list, frame_t *frame)
{
    assert(frame != NULL);
    assert(frame->list_id == NO_LIST);
    assert(frame->next == NULL_FRAME);
    assert(frame->prev == NULL_FRAME);

    frame_ref_t frame_ref = ref_from_frame(frame);

    if (list->last != NULL_FRAME) {
        frame_t *last = frame_from_ref(list->last);
        last->next = frame_ref;

        frame->prev = list->last;
        list->last = frame_ref;

        frame->list_id = list->list_id;
        list->length += 1;
        ZF_LOGV("%s.length = %lu", LIST_NAME(list), list->length);
    } else {
        /* Empty list */
        push_front(list, frame);
    }
}

static frame_t *pop_front(frame_list_t *list)
{
    if (list->first != NULL_FRAME) {
        frame_t *head = frame_from_ref(list->first);
        if (list->last == list->first) {
            /* Was last in list */
            list->last = NULL_FRAME;
            assert(head->next == NULL_FRAME);
        } else {
            frame_t *next = frame_from_ref(head->next);
            next->prev = NULL_FRAME;
        }

        list->first = head->next;

        assert(head->prev == NULL_FRAME);
        head->next = NULL_FRAME;
        head->list_id = NO_LIST;
        head->prev = NULL_FRAME;
        head->next = NULL_FRAME;
        list->length -= 1;
        ZF_LOGV("%s.length = %lu", LIST_NAME(list), list->length);
        return head;
    } else {
        return NULL;
    }
}

static void remove_frame(frame_list_t *list, frame_t *frame)
{
    assert(frame != NULL);
    assert(frame->list_id == list->list_id);

    if (frame->prev != NULL_FRAME) {
        frame_t *prev = frame_from_ref(frame->prev);
        prev->next = frame->next;
    } else {
        list->first = frame->next;
    }

    if (frame->next != NULL_FRAME) {
        frame_t *next = frame_from_ref(frame->next);
        next->prev = frame->prev;
    } else {
        list->last = frame->prev;
    }

    list->length -= 1;
    frame->list_id = NO_LIST;
    frame->prev = NULL_FRAME;
    frame->next = NULL_FRAME;
    ZF_LOGV("%s.length = %lu", LIST_NAME(list), list->length);
}

// Clock based frame replacement algorithm. Iterates through mapped frames
// unsetting the reference bit of each frame until a frame with a 0 reference
// bit is found to be replaced, the frame reference of which is returned.
static frame_ref_t frame_replacement() {
    frame_ref_t to_remove = 0;
    bool found = false;
    while (!found) {
        for (frame_ref_t curr_ref = frame_table.allocated.first; curr_ref != NULL_FRAME;) {
            frame_t *curr_frame = &frame_table.frames[curr_ref];
            ZF_LOGV("Frame: 0x%lx | Vaddr: 0x%lx\n", ref_from_frame(curr_frame), curr_frame->vaddr);
            pt_lock(curr_frame->pt);
            if (valid_mem_range_from_pt(curr_frame->pt, curr_frame->vaddr)) {
                if (frame_ref_check(curr_frame->pt, curr_frame)) {
                    found = true;
                    to_remove = curr_ref;
                    ZF_LOGD("frame_replacement(): 0x%lx | Vaddr: 0x%lx\n", to_remove, curr_frame->vaddr);
                    pt_unlock(curr_frame->pt);
                    break;
                }
            }
            pt_unlock(curr_frame->pt);
            curr_ref = curr_frame->next;
        }
    }
    
    return to_remove;
}

// Removes a frame from the frame table and stores it on the pagefile.
void remove_frame_from_table(frame_ref_t to_replace, bool to_page) {
    frame_t *rep_frame = frame_from_ref(to_replace);
    pt_lock(rep_frame->pt);
    seL4_CPtr cap_to_remove = get_pt_cap(get_pt_entry(rep_frame->pt, rep_frame->vaddr));
    if (rep_frame->pin) {
        pin_count--;
    }
    if (!rep_frame->ref) {
        if (remap_frame_ref_vaddr(rep_frame->pt, rep_frame->vaddr)) {
            ZF_LOGF("Error: could not remap frame for replacement algorithm\n");
        }
    }
    if (to_page) {
        rm_frame_slot(rep_frame->pt->pid, rep_frame->pt, to_replace);
    }
    pt_unlock(rep_frame->pt);
    seL4_ARM_Page_Unmap(cap_to_remove);
    cspace_delete(&cspace, cap_to_remove);
    cspace_free_slot(&cspace, cap_to_remove);
    free_frame(to_replace); // Candidate frame pages are already unmapped from the page table, so simply remove from frame table.
}

static frame_t *alloc_fresh_frame(pt_meta_t *pt)
{
    assert(frame_table.used <= frame_table.capacity);
#ifdef CONFIG_SOS_FRAME_LIMIT
    if (CONFIG_SOS_FRAME_LIMIT != 0ul) {
        assert(frame_table.capacity <= CONFIG_SOS_FRAME_LIMIT);
    }
#endif

    // If frame table at max capacity, page out frame before continuing.
    if (frame_table.used == frame_table.capacity) {
        if (bump_capacity() != 0) {
            /* Could not increase capacity. */
            ZF_LOGW("Warning: Physical memory is full\n");
            remove_frame_from_table(frame_replacement(), true);
            return NULL;
        }
    }

    assert(frame_table.used < frame_table.capacity);

    if (frame_table.used == 0) {
        /* The first frame is a sentinel NULL frame. */
        frame_table.used += 1;
    }

    frame_t *frame = &frame_table.frames[frame_table.used];
    frame_table.used += 1;

    uintptr_t vaddr = (uintptr_t)frame_data(ref_from_frame(frame));
    seL4_ARM_Page sos_page = alloc_frame_at(vaddr);
    if (sos_page == seL4_CapNull) {
        frame_table.used -= 1;
        return NULL;
    }

    *frame = (frame_t) {
        .sos_page = sos_page,
        .list_id = NO_LIST,
    };

    ZF_LOGV("Frame table contains %lu/%lu frames", frame_table.used, frame_table.capacity);
    return frame;
}

static int bump_capacity(void)
{
#ifdef CONFIG_SOS_FRAME_LIMIT
    if (CONFIG_SOS_FRAME_LIMIT != 0ul && frame_table.capacity == CONFIG_SOS_FRAME_LIMIT) {
        /* Reached maximum capacity. */
        return -1;
    }
#endif

    uintptr_t vaddr = (uintptr_t)frame_table.frames + frame_table.byte_length;

    seL4_ARM_Page cptr = alloc_frame_at(vaddr);
    if (cptr == seL4_CapNull) {
        return -1;
    }

    frame_table.byte_length += BIT(seL4_PageBits);
    frame_table.capacity = frame_table.byte_length / sizeof(frame_t);

#ifdef CONFIG_SOS_FRAME_LIMIT
    if (CONFIG_SOS_FRAME_LIMIT != 0ul) {
        frame_table.capacity = MIN(CONFIG_SOS_FRAME_LIMIT, frame_table.capacity);
    }
#endif

    ZF_LOGV("Frame table contains %lu/%lu frames", frame_table.used, frame_table.capacity);
    return 0;
}

static seL4_ARM_Page alloc_frame_at(uintptr_t vaddr)
{
    /* Allocate an untyped for the frame. */
    ut_t *ut = ut_alloc_4k_untyped(NULL);
    if (ut == NULL) {
        return seL4_CapNull;
    }

    /* Allocate a slot for the page capability. */
    seL4_ARM_Page cptr = cspace_alloc_slot(frame_table.cspace);
    if (cptr == seL4_CapNull) {
        ut_free(ut);
        return seL4_CapNull;
    }

    /* Retype the untyped into a page. */
    int err = cspace_untyped_retype(frame_table.cspace, ut->cap, cptr, seL4_ARM_SmallPageObject, seL4_PageBits);
    if (err != 0) {
        cspace_free_slot(frame_table.cspace, cptr);
        ut_free(ut);
        return seL4_CapNull;
    }

    /* Map the frame into SOS. */
    seL4_ARM_VMAttributes attrs = seL4_ARM_Default_VMAttributes | seL4_ARM_ExecuteNever;
    err = map_frame(frame_table.cspace, cptr, frame_table.vspace, vaddr, seL4_ReadWrite, attrs);
    if (err != 0) {
        cspace_delete(frame_table.cspace, cptr);
        cspace_free_slot(frame_table.cspace, cptr);
        ut_free(ut);
        return seL4_CapNull;
    }

    return cptr;
}

int remap_frame_ref_vaddr(pt_meta_t *pt, uintptr_t vaddr) {
    pt_entry_t *pt_entry = get_pt_entry(pt, vaddr);
    frame_t *frame = frame_from_ref(get_pt_frame(pt_entry));

    if (frame->ref) {
        return 0;
    }

    frame->ref = true;
    return seL4_ARM_Page_Map(get_pt_cap(pt_entry), *(pt->pgd), ~OFFSET_MASK & vaddr, seL4_AllRights, seL4_ARM_Default_VMAttributes);
}

int frame_pin(frame_ref_t frame_ref) {
    frame_t *frame = frame_from_ref(frame_ref);
    if (!frame->pin) {
        pin_count++;
    }
    frame->pin = true;
    ZF_LOGD("Pin Count = %d\n", pin_count);
    if (!(pin_count < CONFIG_SOS_FRAME_LIMIT || CONFIG_SOS_FRAME_LIMIT == 0)) {
        // We would potentially deadlock the OS if this pin is allowed to proceed.
        return 1;
    }
    return 0;
}

void frame_unpin(frame_ref_t frame_ref) {
    frame_t *frame = frame_from_ref(frame_ref);
    if (frame->pin) {
        pin_count--;
    }
    frame->pin = false;
}