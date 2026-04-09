
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "paging.h"
#include "kmem.h"

/* KERNEL DEMAND PAGING HEADER
   ---
   This file provides inlinable functions and definitions for kernel-mode
   demand paging. Users are expected to bring their own bitmap, bitmap size,
   size of one element in the pages, and a status code enum that overlays
   dpage_alloc_status_t.
   
   Useful for highly dynamic structures we need lots of, like mm_t or vma_t.
*/

typedef uint8_t* kbitmap_t;

extern uint32_t paging_get_kernel_pd_phys(void);

// Other demand paging contexts may overlay this enum. DO NOT change the
// order of this enum without checking all downstream dependencies
// (hint: look for all usages of dpage.h and dpage_alloc in particular)
typedef enum dpage_alloc_status {
    DPAGE_SUCCESS = 0,
    DPAGE_PMM_INIT_FAILED = 1,
    DPAGE_PG_INIT_FAILED = 2,
    DPAGE_NO_MORE_REGION_SLOTS = 3
} dpage_alloc_status_t;

// Sets a single bit in a bitmap pointer which tracks allocated/free pages or frames.
static inline void dpage_bm_set(uint32_t i, kbitmap_t bitmap, uint32_t bitmap_size_bytes) { 
    if (i >= bitmap_size_bytes * 8) return; // out of bounds, caller should ensure this doesn't happen
    bitmap[i >> 3] |=  (1u << (i & 7));
}

// Clears a single bit in a bitmap pointer which tracks allocated/free pages or frames.
static inline void dpage_bm_clear(uint32_t i, kbitmap_t bitmap, uint32_t bitmap_size_bytes) {
    if (i >= bitmap_size_bytes * 8) return; // out of bounds, caller should ensure this doesn't happen
    bitmap[i >> 3] &= ~(1u << (i & 7));
}

// Given a slot ID and a base address, returns virtual address corresponding to the slot.
static inline uint32_t dpage_bm_slot_to_vaddr(uint32_t slot, uint32_t base, uint32_t size) {
    return base + (slot * size);
}

// Initialize demand paging for a given bitmap base address, pool base address, and bitmap pointer.
bool dpage_init(uint32_t bitmap_base, uint32_t pool_base, uint8_t* bm);

// Analyzes the given bitmap to find a free slot, marks it as taken, and returns the vaddress pointer
// corresponding to that slot. Caller is responsible for ensuring sanctity of passed values.
void* dpage_alloc(kbitmap_t vaddr_bitmap, uint32_t vaddr_base, uint32_t vaddr_size_bytes, size_t element_size, uint32_t bitmap_size_bytes, dpage_alloc_status_t* out_status);

