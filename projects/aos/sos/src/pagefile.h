#pragma once
#include "frame_table.h"

/* For sending back to Page Table */
#define ON_PF_MASK 0x80000000
#define PF_IDX_MASK 0x7FFFFFFF

/* Pagefile dependencies */
#define SIZEOF_PID 2
#define SIZEOF_VALID 1
#define METADATA_REGION_SIZE (sizeof(uintptr_t) + SIZEOF_PID + SIZEOF_VALID)
#define ENTRY_SIZE (PAGE_SIZE_4K + METADATA_REGION_SIZE)

/* Defragmenting of Pagefile */
#define N_DEFRAG_RATE 15

/**
 * @brief Takes a to-be-cleared frame ref, writes its contents to disk (with additional metadata).
 * Uses "old_vaddr" to update PT entry to indicate data on disk rather than memory.
 * @returns 0 on success.
 */
int rm_frame_slot(int pid, pt_meta_t *pt, frame_ref_t old_frame);

/** @brief Given a "src" frame_ref, uses internal metadata to fetch data from pagefile
 *  and writes it to "dst" frame in frame_table.
 * @returns 0 on success.
 */
int fetch_frame_slot(frame_ref_t dst, frame_ref_t src);

/** Creates the metadata header needed in page file
 *  Meta Data: 11-Byte string
      - 8 bytes: vaddr of page
      - 2 bytes: PID
      - 1 byte: validity of Pagefile entry
 */
void new_metadata(char *mem, uintptr_t vaddr, bool valid, uint16_t pid);

// Move the last element of the pagefile into the slot index and
// truncate the file. There will be no fragmentation in the pagefile
// if this is called after every removal of a pagefile entry at the
// appropriate index.
void defragment(uint32_t index);
